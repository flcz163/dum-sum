#include <dim-sum/beehive.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/block_buf.h>
#include <dim-sum/err.h>
#include <dim-sum/errno.h>
#include <dim-sum/fs.h>
#include <dim-sum/idr.h>
#include <dim-sum/mount.h>
#include <dim-sum/sched.h>
#include <dim-sum/writeback.h>

struct double_list all_super_blocks =
	LIST_HEAD_INITIALIZER(all_super_blocks);
struct smp_lock super_block_lock =
	SMP_LOCK_UNLOCKED(all_super_blocks);

static struct idr_space anon_blkdev_numspace =
	NUMSPACE_INITIALIZER(anon_blkdev_numspace);

static struct mutex sync_mutex = MUTEX_INITIALIZER(sync_mutex);

int anon_blkdev_major = 0;

int superblock_set_blocksize(struct super_block *super, int size)
{
	int order = -1;

	ASSERT(size >= 512);

	if (set_blocksize(super->blkdev, size))
		return 0;

	super->block_size = size;
	while (size) {
		order++;
		size >>= 1;
	}

	super->block_size_order = order;
	return super->block_size;
}

static struct super_block *alloc_super(void)
{
	struct super_block *super;
	static struct super_block_ops default_op;

	super = kmalloc(sizeof(struct super_block),  PAF_KERNEL | __PAF_ZERO);
	if (super) {
		list_init(&super->dirty_nodes);
		list_init(&super->sync_nodes);
		list_init(&super->files);
		list_init(&super->cognate);
		list_init(&super->file_nodes);
		init_rwsem(&super->mount_sem);
		sema_init(&super->sem, 1);
		down_write(&super->mount_sem);
		super->ref_count = S_BIAS;
		accurate_set(&super->active_count, 1);
		super->max_file_size = MAX_NON_LFS;
		super->ops = &default_op;
		super->gran_ns = 1000000000;
	}

	return super;
}

static inline void free_super(struct super_block *super)
{
	kfree(super);
}

int __loosen_super(struct super_block *super)
{
	int ret = 0;

	super->ref_count--;
	if (!super->ref_count) {
		free_super(super);
		ret = 1;
	}

	return ret;
}

void loosen_super(struct super_block *super)
{
	smp_lock(&super_block_lock);
	__loosen_super(super);
	smp_unlock(&super_block_lock);
}

void unlock_loosen_super_block(struct super_block *super)
{
	up_read(&super->mount_sem);
	loosen_super(super);
}

static int activate_super(struct super_block *super)
{
	super->ref_count++;
	smp_unlock(&super_block_lock);
	down_write(&super->mount_sem);
	if (super->root_fnode_cache) {
		smp_lock(&super_block_lock);
		if (super->ref_count > S_BIAS) {
			accurate_inc(&super->active_count);
			super->ref_count--;
			smp_unlock(&super_block_lock);
			return 1;
		}
		smp_unlock(&super_block_lock);
	}
	up_write(&super->mount_sem);
	loosen_super(super);

	return 0;
}

void deactivate_super(struct super_block *super)
{
	struct file_system_type *fs = super->fs_type;
	if (atomic_dec_and_lock(&super->active_count, &super_block_lock)) {
		super->ref_count -= S_BIAS-1;
		smp_unlock(&super_block_lock);
		down_write(&super->mount_sem);
		fs->unload_filesystem(super);
		loosen_super(super);
	}
}

/**
 * 查找装载在块设备上的超级块
 */
struct super_block *get_associated_superblock(struct block_device *blkdev)
{
	struct super_block *super;
	struct double_list *list;

	if (!blkdev)
		return NULL;

try_again:
	smp_lock(&super_block_lock);

	list_for_each(list, &all_super_blocks) {
		super = list_container(list, struct super_block, list);

		if (super->blkdev == blkdev) {
			super->ref_count++;
			smp_unlock(&super_block_lock);
			down_read(&super->mount_sem);

			if (super->root_fnode_cache)
				return super;

			unlock_loosen_super_block(super);
			goto try_again;
		}
	}

	smp_unlock(&super_block_lock);
	return NULL;
}

/**
 * 查找超级块
 * 如果没有就创建一个
 */
static struct super_block *
super_find_alloc(struct file_system_type *type,
			int (*test)(struct super_block *,void *),
			int (*set)(struct super_block *,void *),
			void *data)
{
	struct super_block *new = NULL;
	struct double_list *list;
	int err;

try_again:
	smp_lock(&super_block_lock);
	if (test) {
		list_for_each(list, &type->superblocks) {
			struct super_block *old;

			old = list_container(list, struct super_block, cognate);
			if (!test(old, data))
				continue;
			if (!activate_super(old))
				goto try_again;
			if (new)
				free_super(new);

			return old;
		}
	}

	if (!new) {
		smp_unlock(&super_block_lock);
		new = alloc_super();
		if (!new)
			return ERR_PTR(-ENOMEM);

		goto try_again;
	}

	err = set(new, data);
	if (err) {
		smp_unlock(&super_block_lock);
		free_super(new);

		return ERR_PTR(err);
	}

	new->fs_type = type;
	strlcpy(new->blkdev_name, type->name, sizeof(new->blkdev_name));
	list_insert_behind(&new->list, &all_super_blocks);
	list_insert_front(&new->cognate, &type->superblocks);

	smp_unlock(&super_block_lock);

	return new;
}


/**
 * 初始化特殊文件系统的超级。
 */
int init_isolate_superblock(struct super_block *super, void *data)
{
	int dev;

	dev = get_idr_number(&anon_blkdev_numspace);
	if (dev < 0)
		return -ENOSPC;

	if ((dev & MAX_ID_MASK) == (1 << MINORBITS)) {
		put_idr_number(&anon_blkdev_numspace, dev);
		return -EMFILE;
	}

	super->dev_num = MKDEV(anon_blkdev_major, dev & MINORMASK);

	return 0;
}

/**
 * 在文件系统卸载的时候调用
 * 将所有文件节点刷新到磁盘
 */
int fsync_filesystem(struct super_block *super)
{
	sync_filesystem_fnodes(super, 0);

	down(&super->sem);
	if (super->dirty && super->ops->write_super)
		super->ops->write_super(super);
	up(&super->sem);

	if (super->ops->sync_fs)
		super->ops->sync_fs(super, 1);

	blkdev_sync(super->blkdev);

	sync_filesystem_fnodes(super, 1);

	return blkdev_sync(super->blkdev);
}

void sync_filesystems(int wait)
{
	struct super_block *super;
	struct double_list *list;

	/**
	 * 防止多个进程同时执行sync操作
	 */
	mutex_lock(&sync_mutex);

	smp_lock(&super_block_lock);
	list_for_each(list, &all_super_blocks) {
		super = list_container(list, struct super_block, list);

		/**
		 * 没有定义回写方法，略过
		 */
		if (!super->ops->sync_fs)
			continue;
		/**
		 * 只读模式，不需要回写
		 */
		if (super->mount_flags & MFLAG_RDONLY)
			continue;

		super->need_sync_fs = 1;
	}
	smp_unlock(&super_block_lock);

try_again:
	smp_lock(&super_block_lock);
	/* 再次遍历所有超级块 */
	list_for_each(list, &all_super_blocks) {
		super = list_container(list, struct super_block, list);
	
		if (!super->need_sync_fs)
			continue;
		super->need_sync_fs = 0;

		/**
		 * 只读模式，不需要回写
		 * 这里需要再次判断一下
		 */
		if (super->mount_flags & MFLAG_RDONLY)
			continue;
	
		super->ref_count++;
		smp_unlock(&super_block_lock);

		/**
		 * 获取信号量，防止回写过程中文件系统被卸载
		 */
		down_read(&super->mount_sem);
		/* 调用文件系统的方法进行回写 */
		if (super->root_fnode_cache && (wait || super->dirty))
			super->ops->sync_fs(super, wait);

		unlock_loosen_super_block(super);

		goto try_again;
	}

	smp_unlock(&super_block_lock);
	mutex_unlock(&sync_mutex);
}

/**
 * 将所有超级块回写到磁盘
 */
void sync_superblocks(void)
{
	struct super_block *super;
	struct double_list *list;

try_again:
	smp_lock(&super_block_lock);

	list_for_each(list, &all_super_blocks) {
		super = list_container(list, struct super_block, list);

		if (super->dirty) {
			super->ref_count++;
			smp_unlock(&super_block_lock);
			down_read(&super->mount_sem);

			down(&super->sem);
			if (super->root_fnode_cache && super->ops->write_super)
				super->ops->write_super(super);
			up(&super->sem);

			unlock_loosen_super_block(super);
			goto try_again;
		}
	}

	smp_unlock(&super_block_lock);
}

static void mark_files_ro(struct super_block *super)
{
	struct double_list *list;
	struct file *file;

	/**
	 * 这里应该将锁的粒度细化一下
	 */
	smp_lock(&files_lock);

	list_for_each(list, &super->files) {
		file = list_container(list, struct file, super);

		if (S_ISREG(file->fnode_cache->file_node->mode) && file_refcount(file))
			file->f_mode &= ~FMODE_WRITE;
	}

	smp_unlock(&files_lock);
}

/**
 * 有几种途径重新装载系统:
 *	1、用户要求
 *	2、卸载根路径
 *	3、重新加载单实例文件系统
 */
int remount_filesystem(struct super_block *super, int flags, void *data, int force)
{
	int retval;

	/**
	 * 试图在只读设备上加载可写的文件系统
	 */
	if (!(flags & MFLAG_RDONLY) && blkdev_read_only(super->blkdev))
		return -EACCES;

	shrink_fnode_cache(super);
	fsync_filesystem(super);

	/**
	 * 可写变只读
	 */
	if ((flags & MFLAG_RDONLY) && !(super->mount_flags & MFLAG_RDONLY)) {
		if (force)
			mark_files_ro(super);
		else if (!file_readonly_all(super))
			return -EBUSY;
	}

	if (super->ops->remount) {
		down(&super->sem);
		retval = super->ops->remount(super, &flags, data);
		up(&super->sem);
		if (retval)
			return retval;
	}
	super->mount_flags = (super->mount_flags & ~MS_RMT_MASK) |
							(flags & MS_RMT_MASK);

	return 0;
}

static int compare_single(struct super_block *super, void *data)
{
	return 1;
}

struct super_block *load_single_filesystem(struct file_system_type *fs_type,
	int flags, void *data,
	int (*fill_super)(struct super_block *, void *, int))
{
	struct super_block *super;
	int err;

	/**
	 * compare_single确保只会为文件系统生成一个实例
	 */
	super = super_find_alloc(fs_type, compare_single,
		init_isolate_superblock, NULL);
	if (IS_ERR(super))
		return super;

	/**
	 * 避免多次初始化
	 */
	if (!super->root_fnode_cache) {
		super->mount_flags = flags;
		err = fill_super(super, data, flags & MS_VERBOSE ? 1 : 0);
		if (err) {
			up_write(&super->mount_sem);
			deactivate_super(super);

			return ERR_PTR(err);
		}
		super->mount_flags |= MS_ACTIVE;
	}

	/**
	 * 可能已经装载过，以新标志重新装载
	 */
	remount_filesystem(super, flags, data, 0);
	return super;
}

/**
 * 加载特殊内存文件系统
 */
struct super_block *load_ram_filesystem(struct file_system_type *fs_type,
	int flags, void *data,
	int (*fill_super)(struct super_block *, void *, int))
{
	struct super_block *super;
	int err;

	super = super_find_alloc(fs_type, NULL, init_isolate_superblock, NULL);
	if (IS_ERR(super))
		return super;

	super->mount_flags = flags;

	/**
	 * 调用文件系统的回调函数填充超级块
	 */
	err = fill_super(super, data, flags & MS_VERBOSE ? 1 : 0);
	if (err) {
		up_write(&super->mount_sem);
		deactivate_super(super);
		return ERR_PTR(err);
	}

	super->mount_flags |= MS_ACTIVE;

	return super;
}

static struct super_block_ops dummy_superblock_ops =
{
	.statfs = simple_statfs,
};
/**
 * 加载伪文件系统，这些文件仅供内部使用
 * 并不加载到用户根文件系统中
 */
struct super_block *
load_isolate_filesystem(struct file_system_type *fs_type, char *name,
	struct super_block_ops *ops, unsigned long magic)
{
	struct file_name file_name = {.name = name, .len = strlen(name)};
	struct filenode_cache *fnode_cache;
	struct file_node *fnode_root;
	struct super_block *super;
	struct timespec now;

	/**
	 * 查找文件系统超级块，没有就创建一个
	 */
	super = super_find_alloc(fs_type, NULL, init_isolate_superblock, NULL);
	if (IS_ERR(super))
		return super;

	super->mount_flags = MFLAG_INTERNAL;
	super->max_file_size = ~0ULL;
	super->block_size = 1024;
	super->block_size_order = 10;
	super->magic = magic;
	super->ops = ops ? ops : &dummy_superblock_ops;
	super->gran_ns = 1;

	fnode_root = fnode_alloc(super);
	if (!fnode_root)
		goto out;

	fnode_root->mode = S_IFDIR | S_IRUSR | S_IWUSR;
	fnode_root->uid = fnode_root->gid = 0;
	now = CURRENT_TIME;
	fnode_root->access_time = now;
	fnode_root->data_modify_time = now;
	fnode_root->meta_modify_time = now;

	fnode_cache = fnode_cache_alloc(NULL, &file_name);
	if (!fnode_cache) {
		loosen_file_node(fnode_root);
		goto out;
	}
	fnode_cache_stick(fnode_cache, fnode_root);
	fnode_cache->parent = fnode_cache;

	super->root_fnode_cache = fnode_cache;
	super->mount_flags |= MS_ACTIVE;

	return super;

out:
	up_write(&super->mount_sem);
	deactivate_super(super);

	return ERR_PTR(-ENOMEM);
}

static int compare_common(struct super_block *super, void *data)
{
	return (void *)super->blkdev == data;
}

static int set_common(struct super_block *super, void *data)
{
	super->blkdev = data;
	super->dev_num = super->blkdev->devno;

	return 0;
}

/**
 * 普通磁盘文件系统的加载函数
 */
struct super_block *
load_common_filesystem(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data,
	int (*fill_super)(struct super_block *, void *, int))
{
	struct block_device *blkdev;
	struct super_block *super;
	int err = 0;

	/**
	 * 在块设备文件系统中，以独占方式打开块设备。
	 * 获得块设备描述符的指针。
	 */
	blkdev = blkdev_open_exclude(dev_name, flags, fs_type);
	if (IS_ERR(blkdev))
		return (struct super_block *)blkdev;

	down(&blkdev->mount_sem);
	/**
	 * 搜索文件系统的超级块对象链表。
	 * 如果找到一个与块设备相关的超级块，则返回它的地址，
	 * 否则分配并初始化一个超级块新对象
	 */
	super = super_find_alloc(fs_type, compare_common, set_common, blkdev);
	up(&blkdev->mount_sem);

	if (IS_ERR(super))
		goto out;

	/**
	 * 此时持有超级块的mount_sem!
	 */
	if (super->root_fnode_cache) {
		/**
		 * 与以前的装载有冲突
		 */
		if ((flags ^ super->mount_flags) & MFLAG_RDONLY) {
			up_write(&super->mount_sem);
			/**
			 * 递减super_find_alloc中增加的引用计数
			 */
			deactivate_super(super);
			super = ERR_PTR(-EBUSY);
		}
		/**
		 * 不需要额外的工作，直接返回即可
		 * 保持对超级块对象的引用
		 */
		goto out;
	} else {
		/**
		 * 复制安装标志。及其他文件系统相关的值。
		 */
		super->mount_flags = flags;
		format_block_devname(blkdev, super->blkdev_name);
		super->block_size_device = block_size(blkdev);
		superblock_set_blocksize(super, super->block_size_device);
		/**
		 * 回调文件系统特定的初始化方法
		 */
		err = fill_super(super, data, flags & MS_VERBOSE ? 1 : 0);
		if (err) {
			up_write(&super->mount_sem);
			deactivate_super(super);
			super = ERR_PTR(err);
		} else
			super->mount_flags |= MS_ACTIVE;
	}

	return super;

out:
	blkdev_close_exclude(blkdev);
	return super;
}

static void generic_unload_filesystem(struct super_block *super)
{
	struct filenode_cache *fnode_cache = super->root_fnode_cache;
	struct super_block_ops *sop = super->ops;

	if (fnode_cache) {
		super->root_fnode_cache = NULL;
		shrink_dcache_parent(fnode_cache);
		loosen_filenode_cache(fnode_cache);
		fsync_filesystem(super);

		down(&super->sem);

		super->mount_flags &= ~MS_ACTIVE;
		invalidate_fnode_filesystem(super);
		if (sop->write_super && super->dirty)
			sop->write_super(super);
		if (sop->loosen_super)
			sop->loosen_super(super);

		WARN_ON(invalidate_fnode_filesystem(super),
			"VFS: Busy inodes after unmount." );

		up(&super->sem);
	}
	/**
	 * 从全局链表中摘除
	 */
	smp_lock(&super_block_lock);
	list_del_init(&super->list);
	list_del(&super->cognate);
	smp_unlock(&super_block_lock);
	up_write(&super->mount_sem);
}

/**
 * 移除特殊文件系统的超级块。
 */
void unload_isolate_filesystem(struct super_block *super)
{
	int minor = MINOR(super->dev_num);

	generic_unload_filesystem(super);
	put_idr_number(&anon_blkdev_numspace, minor);
}

void unload_common_filesystem(struct super_block *super)
{
	struct block_device *blkdev = super->blkdev;

	generic_unload_filesystem(super);
	set_blocksize(blkdev, super->block_size_device);
	/**
	 * 关闭块设备，并且解除排它性设置
	 */
	blkdev_close_exclude(blkdev);
}

void __init init_file_systems(void)
{
	init_iosched_noop();
	init_devfs();
	init_journal();
	init_lext3();
}
