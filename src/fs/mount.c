#include <dim-sum/beehive.h>
#include <dim-sum/capability.h>
#include <dim-sum/err.h>
#include <dim-sum/fnode.h>
#include <dim-sum/fs.h>
#include <dim-sum/fs_context.h>
#include <dim-sum/hash.h>
#include <dim-sum/mount.h>
#include <dim-sum/sched.h>
#include <dim-sum/syscall.h>

#include "internal.h"

/**
 * 保护对哈希表及mount描述符的互斥访问
 */
aligned_cacheline_in_smp
struct smp_lock mount_lock = SMP_LOCK_UNLOCKED(mount_lock);
/**
 * 保护全局的mount/unmount操作
 */
static struct rw_semaphore mount_sem = RWSEM_INITIALIZER(mount_sem);
/**
 * 描述符内存分配器
 */
static struct beehive_allotter *mount_allotter;

/**
 * mount描述符散列表。
 * 这里用哈希表是为了加快目录搜索的速度
 */
static struct hash_list_bucket *__hash;
static int __hash_order;
static int __hash_mask;

static void release_mount(struct object *object);

static inline unsigned long
hash(struct mount_desc *mnt, struct filenode_cache *filenode_cache)
{
	return hash_pair(mnt, (unsigned long)filenode_cache,
		__hash_order, __hash_mask);
}

/**
 * 分配并初始化一个mount描述符
 */
static struct mount_desc *alloc_mount(const char *name)
{
	struct mount_desc *desc;
	desc = beehive_alloc(mount_allotter, PAF_KERNEL | __PAF_ZERO);

	if (desc) {
		init_object(&desc->object, NULL, release_mount);
		hash_list_init_node(&desc->hash_node);
		list_init(&desc->child);
		list_init(&desc->children);

		if (name) {
			int size = strlen(name)+1;

			desc->dev_name = kmalloc(size, PAF_KERNEL);
			if (desc->dev_name)
				memcpy(desc->dev_name, name, size);
		}
	}

	return desc;
}

/**
 * 释放由mount描述符
 */
static void free_mount(struct mount_desc *desc)
{
	if (desc->dev_name)
		kfree(desc->dev_name);

	beehive_free(mount_allotter, desc);
}

static void release_mount(struct object *object)
{
	struct mount_desc *desc = container_of(object, struct mount_desc, object);
	struct super_block *sb = desc->super_block;

	loosen_filenode_cache(desc->sticker);
	free_mount(desc);
	deactivate_super(sb);
}

/**
 * 在哈希表中查找一个描述符并返回它的地址。
 */
struct mount_desc *
lookup_mount(struct mount_desc *mnt, struct filenode_cache *filenode_cache)
{
	struct hash_list_bucket *bucket = __hash + hash(mnt, filenode_cache);
	struct hash_list_node *node;
	struct mount_desc *mount;
	struct mount_desc *ret = NULL;

	/**
	 * 获得锁，对哈希表进行互斥访问
	 */
	smp_lock(&mount_lock);
	/**
	 * 遍历哈希桶
	 */
	hlist_for_each(node, bucket) {
		mount = hlist_entry(node, struct mount_desc, hash_node);
		/**
		 * 该文件系统被mount到指定目录了
		 * 返回给调用者
		 */
		if (mount->parent == mnt && mount->mountpoint == filenode_cache) {
			ret = hold_mount(mount);
			break;
		}
	}
	smp_unlock(&mount_lock);

	return ret;
}

/**
 * 内部调用，只加载文件系统，但是不暴露到目录树中
 * 参数有:文件系统类型.安装标志及块设备名.
 * 这处理实际的安装操作并返回一个新安装文件系统描述符的地址.
 */
struct mount_desc *
do_internal_mount(const char *fstype, int flags, const char *name, void *data)
{
	/**
	 * 获得文件系统名称对应的文件系统对象。
	 */
	struct file_system_type *fs = lookup_file_system(fstype);
	struct super_block *sb;
	struct mount_desc *ret;

	/**
	 * 错误的文件系统类型。
	 */
	if (!fs)
		return ERR_PTR(-ENODEV);

	/**
	 * 分配一个新的已安装文件系统的描述符
	 */
	ret = alloc_mount(name);
	if (!ret)/* 内存不足 */
		goto out;

	/**
	 * 调用文件系统的get_sb分配并初始化一个新的超级块。
	 * 例如ext3的load_super_block方法是ext3_load_super_block。
	 */
	sb = fs->load_filesystem(fs, flags, name, data);
	if (IS_ERR(sb)) {
		free_mount(ret);
		ret = (struct mount_desc *)sb;
		goto out;
	}

	ret->super_block = sb;
	/**
	 * 将此字段初始化为与文件系统根目录对应的目录项对象的地址。
	 * 并增加其引用计数值。
	 */
	ret->sticker = hold_dirnode_cache(sb->root_fnode_cache);
	/**
	 * 临时值，无用
	 * 随后修改为真正的挂载点。
	 */
	ret->mountpoint = sb->root_fnode_cache;
	/**
	 * 将mnt_parent指向自身，表示它是一个独立的根。
	 * 上层调用者可以修改它。
	 */
	ret->parent = ret;
	/**
	 * 在加载回调中获得，此处应当将该锁释放
	 */
	up_write(&sb->mount_sem);
out:
	loosen_file_system(fs);
	return (struct mount_desc *)ret;
}

static int bind_mount(struct mount_desc *desc, struct filenode_lookup_s *look)
{
	struct hash_list_bucket *bucket;
	struct mount_desc *parent;
	bool in_cache;

	/**
	 * mount点应该是目录
	 * 被mount的对象也应当(根)目录
	 */
	if (!S_ISDIR(look->filenode_cache->file_node->mode)
	    || !S_ISDIR(desc->sticker->file_node->mode))
		return -ENOTDIR;

	down(&look->filenode_cache->file_node->sem);

	/**
	 * 目录被删除了(但是内存中还存在)
	 */
	if (IS_DEADDIR(look->filenode_cache->file_node)) {
		up(&look->filenode_cache->file_node->sem);
		return -ENOENT;
	}

	/**
	 * 1、根目录总是可以被重新mount的
	 * 2、如果目录还在缓存哈希表中，说明它是有效的，可mount
	 * 否则不能mount
	 */
	in_cache = (look->filenode_cache->flags & FNODECACHE_INHASH);
	if (!IS_ROOT(look->filenode_cache) && !in_cache) {
		up(&look->filenode_cache->file_node->sem);
		return -ENOENT;
	}

	smp_lock(&mount_lock);
	parent = hold_mount(look->mnt);
	hold_mount(desc);
	desc->parent = parent;
	desc->mountpoint = hold_dirnode_cache(look->filenode_cache);
	/**
	 * 根据mount点，确定哈希桶位置
	 */
	bucket = __hash + hash(look->mnt, look->filenode_cache);
	/**
	 * !!应当加在桶前面!!
	 */
	hlist_add_head(&desc->hash_node, bucket);
	/**
	 * 加入上级对象的子对象链表中
	 */
	list_insert_behind(&desc->child, &parent->children);
	look->filenode_cache->mount_count++;
	smp_unlock(&mount_lock);

	up(&look->filenode_cache->file_node->sem);

	return 0;
}

/**
 * 被do_new_mount调用
 */
static int add_mount(struct mount_desc *newmnt,
	struct filenode_lookup_s *look, int mnt_flags)
{
	int ret;

	/**
	 * 获得全局写信号量
	 * 避免并发的执行mount操作
	 */
	down_write(&mount_sem);

	/**
	 * 如果文件系统已经被安装在指定的安装点上，
	 * 或者该安装点是一个符号链接，则释放读写信号量并返回错误。
	 */
	ret = -EBUSY;
	/**
	 * 如果要安装的文件系统已经被安装在指定的安装点上
	 * 则返回错误。
	 */
	if (look->mnt->super_block == newmnt->super_block &&
	    look->mnt->sticker == look->filenode_cache)
		goto unlock;

	ret = -EINVAL;
	/**
	 * 或者文件系统根是一个符号链接，也返回错误。
	 */
	if (S_ISLNK(newmnt->sticker->file_node->mode))
		goto unlock;

	newmnt->flags = mnt_flags;
	/**
	 * graft_tree把新安装的文件系统对象插入到namespace链表、散列表及父文件系统的子链表中。
	 */
	ret = bind_mount(newmnt, look);

unlock:
	/**
	 * 释放读写信号量
	 */
	up_write(&mount_sem);
	/**
	 * 如果出错，这里会直接释放描述符
	 * 否则，在bind_mount里面增加了引用计数值
	 * 这里仅仅将其递减回去而不会释放描述符
	 */
	loosen_mount(newmnt);

	return ret;
}

static int do_new_mount(struct filenode_lookup_s *look, char *type, int flags,
			int mnt_flags, char *name, void *data)
{
	struct mount_desc *mnt;
	int ret;

	if (!type)
		return -EINVAL;

	/**
	 * 加载文件系统超级块
	 */
	mnt = do_internal_mount(type, flags, name, data);
	if (IS_ERR(mnt))
		return PTR_ERR(mnt);

	/**
	 * 该文件系统不允许用户进行mount操作
	 */
	if (mnt->super_block->mount_flags & MFLAG_INTERNAL) {
		loosen_mount(mnt);
		return -EINVAL;
	}

	/**
	 * 将子文件系统实例与父文件系统装载点进行关联
	 */
	ret = add_mount(mnt, look, flags);
	if (ret)
		loosen_mount(mnt);

	return ret;
}

/**
 * 安装文件系统。
 */
long do_mount(char *dev_name, char *mount_dir, char *type_page,
		  unsigned long flags, void *data_page)
{
	struct filenode_lookup_s look = {0};
	int ret = 0;
	int mnt_flags = 0;

	/* 检查路径名是否正常 */
	if (!mount_dir || !*mount_dir)
		return -EINVAL;

	/**
	 * 如果安装标志MS_NOSUID,MS_NODEV,MS_NOEXEC被设置，则清除它们，并在文件系统对象中设置相应的标志。
	 */
	if (flags & MS_NOSUID)
		mnt_flags |= MNT_NOSUID;
	if (flags & MS_NODEV)
		mnt_flags |= MNT_NODEV;
	if (flags & MS_NOEXEC)
		mnt_flags |= MNT_NOEXEC;
	flags &= ~(MS_NOSUID | MS_NOEXEC | MS_NODEV | MS_ACTIVE);

	/**
	 * load_directory_cache查找安装点的路径
	 * 查找其路径分量及所属文件系统
	 */
	ret = load_filenode_cache(mount_dir, FNODE_LOOKUP_READLINK, &look);
	if (ret)
		return ret;

	/**
	 * 目前仅仅支持普通的装载
	 * 而不支持remount等操作
	 */
	ret = do_new_mount(&look, type_page, flags, mnt_flags,
				      dev_name, data_page);

	loosen_fnode_look(&look);
	return ret;
}

/**
 * mount一个文件系统。
 *		dev_name:文件系统所在的设备文件的路径名。如果没有则为NULL。
 *		mount_dir:文件系统将安装于该目录中。
 *		type:文件系统的类型，必须是已经注册的文件系统的名字。
 *		flags:安装标志。安装标志如MS_RDONLY。
 *		data:与文件系统相关的数据结构的指针，可能为空。
 */
asmlinkage int sys_mount(const char __user *dev_name, const char __user *mount_dir,
			  const char __user *type, unsigned long flags,
			  const void __user *data)
{
	int ret;

	/**
	 * do_mount执行真正的安装操作。
	 */
	ret = do_mount((char*)dev_name, (char *)mount_dir, (char*)type,
			  flags, (void*)data);

	return ret;
}

static void unbind_mount(struct mount_desc *mnt)
{
	if (mnt->parent == mnt) {
		smp_unlock(&mount_lock);
	} else {
		struct filenode_cache *mountpoint = mnt->mountpoint;
		struct mount_desc *parent = mnt->parent;

		mnt->parent = mnt;
		mnt->mountpoint = mnt->sticker;
		list_del_init(&mnt->child);
		hlist_del_init(&mnt->hash_node);
		mountpoint->mount_count--;
		smp_unlock(&mount_lock);
		loosen_filenode_cache(mountpoint);
		loosen_mount(parent);
	}
	loosen_mount(mnt);
	smp_lock(&mount_lock);
}

/**
 * 卸载文件系统，并不检查权限，及参数正确性检查。
 */
static int do_umount(struct mount_desc *mnt, int flags)
{
	/**
	 * sb是超级块对象的地址。
	 */
	struct super_block * sb = mnt->super_block;
	int retval = 0;

	/**
	 * 如果要求强制卸载，那么调用umount_begin超级块操作中断任何正在进行的安装操作。
	 */
	if( (flags&MNT_FORCE) && sb->ops->umount_begin)
		sb->ops->umount_begin(sb);

	/**
	 * 如果要卸载的是根文件系统
	 * 并且用户并不想真正将它卸载下来(没有MNT_DETACH标志)
	 * 就调用do_remount_sb，该函数重新安装根文件系统为只读。
	 */
	if (mnt == current->fs_context->root_mount && !(flags & MNT_DETACH)) {
		down_write(&sb->mount_sem);
		if (!(sb->mount_flags & MFLAG_RDONLY))
			retval = remount_filesystem(sb, MFLAG_RDONLY, NULL, 0);
		up_write(&sb->mount_sem);

		return retval;
	}

	/**
	 * 在真正进行写操作前，获取信号量和自旋锁。
	 */
	down_write(&mount_sem);
	smp_lock(&mount_lock);

	retval = -EBUSY;
	/**
	 * 如果没有子安装文件系统的安装点
	 * 则卸载文件系统
	 */
	if (get_object_count(&mnt->object) == 2) {
		unbind_mount(mnt);
		retval = 0;
	}

	/** 
	 * 释放自旋锁和信号量，注意释放顺序。
	 */
	smp_unlock(&mount_lock);
	up_write(&mount_sem);

	return retval;
}

/**
 * umount系统调用用来卸载一个文件系统
 * sys_umount是它的服务例程。
 * name-文件名（安装点目录或是块设备文件名）
 * flags-标志
 */
asmlinkage int sys_umount(const char *name, int flags)
{
	struct filenode_lookup_s look = {0};
	int ret;

	/**
	 * 检查用户是否有相应的权限，没有权限就返回EPERM
	 */
	ret = -EPERM;
	if (!capable(CAP_SYS_ADMIN))
		goto out;

	/**
	 * __user_walk会调用path_lookup，来查找安装点路径名。
	 * 其查找结果存在nd中
	 */
	ret = load_filenode_user((char *)name, FNODE_LOOKUP_READLINK, &look);
	if (ret)
		goto out;

	ret = -EINVAL;
	/**
	 * 该目录不是安装点。
	 */
	if (look.filenode_cache != look.mnt->sticker)
		goto loosen;

	/**
	 * 调用do_umount执行真正的卸载过程。
	 */
	ret = do_umount(look.mnt, flags);
loosen:
	/**
	 * 减少文件系统根目录的目录项对象和已经安装文件系统描述符的引用计数。
	 * 这些计数值由path_loopup增加。
	 */
	loosen_fnode_look(&look);
out:
	return ret;
}

/**
 * 在rootfs中挂载最初的根目录。
 */
static void __init init_mount_tree(void)
{
	struct mount_desc *mnt;

	/**
	 * 调用do_kern_mount，安装rootfs。
	 * 最终会调用rootfs的get_sb方法，即rootfs_get_sb。
	 */
	mnt = do_internal_mount("rootfs", 0, "rootfs", NULL);
	if (IS_ERR(mnt))
		panic("Can't create rootfs");

	/**
	 * 设置进程0的根目录和当前工作目录为根文件系统。
	 */
	set_fs_pwd(current->fs_context, mnt, mnt->sticker);
	set_fs_root(current->fs_context, mnt, mnt->sticker);
}

void __init mnt_init(void)
{
	unsigned long order;

	mount_allotter = beehive_create("mnt_cache", sizeof(struct mount_desc),
			0, BEEHIVE_HWCACHE_ALIGN | BEEHIVE_PANIC, NULL);

	order = 0; 
	__hash = (struct hash_list_bucket *)
		alloc_pages_memory(PAF_ATOMIC, order);

	if (!__hash)
		panic("Failed to allocate mount hash table\n");

	init_hash_list(__hash, PAGE_SIZE << order,
		&__hash_mask, &__hash_order);
	
	init_rootfs();
	init_mount_tree();
}
