#include <dim-sum/beehive.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/boot_allotter.h>
#include <dim-sum/block_buf.h>
#include <dim-sum/cdev.h>
#include <dim-sum/errno.h>
#include <dim-sum/fnode.h>
#include <dim-sum/fs.h>
#include <dim-sum/hash.h>
#include <dim-sum/pagemap.h>
#include <dim-sum/sched.h>
#include <dim-sum/string.h>
#include <dim-sum/writeback.h>

#include "internal.h"

static struct semaphore del_sem = 
	SEMAPHORE_INITIALIZER(del_sem, 1);

static unsigned int __hash_mask;
static unsigned int __hash_order;
/**
 * 所有索引节点对象的哈希表。
 */
static struct hash_list_bucket *__hash;

struct double_list filenode_used = 
			LIST_HEAD_INITIALIZER(filenode_used);
/**
 * 未用索引节点链表。
 */
struct double_list filenode_unused = 
			LIST_HEAD_INITIALIZER(filenode_unused);

struct smp_lock filenode_lock = 
			SMP_LOCK_UNLOCKED(filenode_lock);

static struct beehive_allotter * fnode_allotter;

struct filenode_stat fnode_stat;

void fnode_init(struct file_node *fnode)
{
	hash_list_init_node(&fnode->hash_node);
	list_init(&fnode->device);
	sema_init(&fnode->sem, 1);
	INIT_RADIX_TREE(&fnode->cache_data.page_tree, PAF_ATOMIC);
	smp_lock_init(&fnode->cache_data.tree_lock);
	smp_lock_init(&fnode->cache_data.i_mmap_lock);
	smp_lock_init(&fnode->cache_data.block_lock);
	list_init(&fnode->cache_data.i_mmap_nonlinear);
	smp_lock_init(&fnode->i_lock);
	i_size_ordered_init(fnode);
}

static struct file_node *__fnode_alloc(struct super_block *super)
{
	static struct cache_space_ops empty_space_ops;
	static struct file_node_ops empty_fnode_ops;
	static struct file_ops empty_file_ops;
	struct file_node *fnode;

	/**
	 * 一般情况下，文件系统应当分配一个扩展的节点
	 */
	if (super->ops->alloc)
		fnode = super->ops->alloc(super);
	else
		fnode = beehive_alloc(fnode_allotter, PAF_KERNEL | __PAF_ZERO);

	if (fnode) {
		struct file_cache_space *cache_space = &fnode->cache_data;

		fnode_init(fnode);
		fnode->super = super;
		fnode->block_size_order = super->block_size_order;
		accurate_set(&fnode->ref_count, 1);
		fnode->node_ops = &empty_fnode_ops;
		fnode->file_ops = &empty_file_ops;
		fnode->link_count = 1;
		accurate_set(&fnode->write_users, 0);

		cache_space->ops = &empty_space_ops;
 		cache_space->fnode = fnode;
		cache_space_set_allocflags(cache_space, PAF_USER);
		cache_space->blkdev_infrast = &default_blkdev_infrast;

		/**
		 * 如果文件系统与块设备绑定了
		 */
		if (super->blkdev) {
			struct blkdev_infrast *infrast;

			/**
			 * 使用文件系统的块设备
			 */
			infrast = super->blkdev->blkdev_infrast;
			if (!infrast)
				infrast = super->blkdev->fnode->cache_space->blkdev_infrast;
			cache_space->blkdev_infrast = infrast;
		}
		fnode->cache_space = cache_space;
	}

	return fnode;
}

/**
 * 为文件系统分配一个新的文件节点
 * 并将节点添加到全局链表中
 */
struct file_node *fnode_alloc(struct super_block *super)
{
	static unsigned long node_num;
	struct file_node *fnode;

	fnode = __fnode_alloc(super);
	if (fnode) {
		smp_lock(&filenode_lock);
		fnode_stat.nr_inodes++;
		list_insert_front(&fnode->list, &filenode_used);
		list_insert_front(&fnode->super_block, &super->file_nodes);
		node_num++;
		fnode->node_num = node_num;
		smp_unlock(&filenode_lock);
	}

	return fnode;
}

static struct hash_list_bucket *
hash(struct super_block *super, unsigned long val)
{
	struct hash_list_bucket *head;

	head = __hash + hash_pair(super, val, __hash_order, __hash_mask);

	return head;
}

/**
 * 将文件节点 放到哈希表中
 */
void putin_file_node(struct file_node *fnode)
{
	struct hash_list_bucket *head = hash(fnode->super, fnode->node_num);

	smp_lock(&filenode_lock);
	hlist_add_head(&fnode->hash_node, head);
	smp_unlock(&filenode_lock);
}

void takeout_file_node(struct file_node *fnode)
{
	smp_lock(&filenode_lock);
	hlist_del_init(&fnode->hash_node);
	smp_unlock(&filenode_lock);
}

int fnode_schedule(struct bit_wait_task_desc *word)
{
	schedule();
	return 0;
}

/**
 * 等待文件节点数据传送完毕
 */
static void fnode_wait_io_locked(struct file_node *fnode)
{
	smp_unlock(&filenode_lock);
	writeback_fnode_wait(fnode);
	smp_lock(&filenode_lock);
}

void wake_up_filenode(struct file_node *fnode)
{
	smp_mb();
	wake_up_bit(&fnode->state, __FNODE_TRANSFERRING);
}

void __hold_file_node(struct file_node *fnode)
{
	if (accurate_read(&fnode->ref_count)) {
		accurate_inc(&fnode->ref_count);
		return;
	}

	accurate_inc(&fnode->ref_count);
	if (!(fnode->state & (FNODE_DIRTY | FNODE_TRANSFERRING)))
		list_move_to_front(&fnode->list, &filenode_used);
	fnode_stat.nr_unused--;
}

struct file_node *hold_file_node(struct file_node *fnode)
{
	smp_lock(&filenode_lock);
	if (!(fnode->state & FNODE_FREEING))
		__hold_file_node(fnode);
	else
		fnode = NULL;
	smp_unlock(&filenode_lock);
	return fnode;
}

void fnode_update_atime(struct file_node *fnode)
{
	struct timespec now;

	if (IS_NOATIME(fnode))
		return;

	if (IS_NODIRATIME(fnode) && S_ISDIR(fnode->mode))
		return;

	if (IS_RDONLY(fnode))
		return;

	now = current_fs_time(fnode->super);
	if (!timespec_equal(&fnode->access_time, &now)) {
		fnode->access_time = now;
		__mark_filenode_dirty(fnode, __FNODE_DIRTY_SYNC);
	}
}

void fnode_update_time(struct file_node *fnode, int ctime)
{
	struct timespec now;
	int mark = 0;

	if (IS_NOCMTIME(fnode))
		return;
	if (IS_RDONLY(fnode))
		return;

	now = current_fs_time(fnode->super);
	if (!timespec_equal(&fnode->data_modify_time, &now))
		mark = 1;
	fnode->data_modify_time = now;

	if (ctime) {
		if (!timespec_equal(&fnode->meta_modify_time, &now))
			mark = 1;
		fnode->meta_modify_time = now;
	}

	if (mark)
		__mark_filenode_dirty(fnode, __FNODE_DIRTY_SYNC);
}

static void fnode_free(struct file_node *fnode) 
{
	if (fnode->super->ops->free)
		fnode->super->ops->free(fnode);
	else
		beehive_free(fnode_allotter, (fnode));
}

/**
 * 删除文件节点时用。
 * 保存所有节点数据并等待其完成
 */
void fnode_clean(struct file_node *fnode)
{
	might_sleep();

	ASSERT(fnode->state & FNODE_FREEING);
	ASSERT(!(fnode->state & FNODE_CLEAN));

	/**
	 * 等待文件节点上的回写IO完成
	 */
	writeback_fnode_wait(fnode);
	/**
	 * 做一些文件系统的收尾工作
	 * 如ext3清除节点的保留空间
	 */
	if (fnode->super && fnode->super->ops->clean_fnode)
		fnode->super->ops->clean_fnode(fnode);
	/**
	 * 如果节点指向一个设备文件
	 * 则从设备的文件节点链表中删除。
	 */
	if (fnode->blkdev)
		blkdev_detach(fnode);
	if (fnode->chrdev)
		chrdev_detach(fnode);

	fnode->state = FNODE_CLEAN;
}

void generic_delete_fnode(struct file_node *fnode)
{
	struct super_block_ops *ops = fnode->super->ops;

	list_del_init(&fnode->list);
	list_del_init(&fnode->super_block);

	/**
	 * 设置此标志，则不能再次获得其引用
	 */
	fnode->state |= FNODE_FREEING;
	fnode_stat.nr_inodes--;
	smp_unlock(&filenode_lock);

	/**
	 * 删除页面缓存中的数据
	 */
	if (fnode->cache_data.page_count)
		truncate_inode_pages(&fnode->cache_data, 0);

	/**
	 * 文件系统希望在fnode_clean之外做一些额外的清理工作
	 */
	if (ops->delete_fnode) {
		ops->delete_fnode(fnode);
	} else
		fnode_clean(fnode);

	smp_lock(&filenode_lock);
	/**
	 * 从哈希表中摘除文件节点
	 */
	hlist_del_init(&fnode->hash_node);
	smp_unlock(&filenode_lock);
	wake_up_filenode(fnode);
	if (fnode->state != FNODE_CLEAN)
		BUG();
	/**
	 * 释放描述符
	 */
	fnode_free(fnode);
}

/**
 * 删除一个硬连接
 * 但是不调用文件系统的回调来清除文件
 */
static void generic_forget_fnode(struct file_node *fnode)
{
	struct super_block *super = fnode->super;

	if (hash_node_is_hashed(&fnode->hash_node)) {
		if (!(fnode->state & (FNODE_DIRTY | FNODE_TRANSFERRING)))
			list_move_to_front(&fnode->list, &filenode_unused);
		fnode_stat.nr_unused++;
		smp_unlock(&filenode_lock);

		if (!super || (super->mount_flags & MS_ACTIVE))
			return;

		/**
		 * 立即将文件节点信息写到块设备中
		 */
		writeback_fnode_sumbit_wait(fnode, 1);
		smp_lock(&filenode_lock);
		fnode_stat.nr_unused--;
		hlist_del_init(&fnode->hash_node);
	}

	list_del_init(&fnode->list);
	list_del_init(&fnode->super_block);
	fnode_stat.nr_inodes--;
	fnode->state |= FNODE_FREEING;
	smp_unlock(&filenode_lock);

	if (fnode->cache_data.page_count)
		truncate_inode_pages(&fnode->cache_data, 0);

	fnode_clean(fnode);
	fnode_free(fnode);
}

static void generic_release_fnode(struct file_node *fnode)
{
	if (!fnode->link_count)
		generic_delete_fnode(fnode);
	else
		generic_forget_fnode(fnode);
}

static inline void __loosen_file_node(struct file_node *fnode)
{
	struct super_block_ops *ops = fnode->super->ops;

	if (ops && ops->release)
		ops->release(fnode);
	else
		generic_release_fnode(fnode);
}

void loosen_file_node(struct file_node *fnode)
{
	if (fnode) {
		struct super_block_ops *ops = fnode->super->ops;

		WARN_ONCE(fnode->state == FNODE_CLEAN);

		if (ops && ops->loosen)
			ops->loosen(fnode);

		if (atomic_dec_and_lock(&fnode->ref_count, &filenode_lock))
			__loosen_file_node(fnode);
	}
}

static struct file_node *
find_inode(struct super_block *super, struct hash_list_bucket *head,
	int (*test)(struct file_node *, void *), void *data)
{
	struct hash_list_node *hash_node;

repeat:
	hlist_for_each (hash_node, head) { 
		struct file_node * fnode;

		fnode = hlist_entry(hash_node, struct file_node, hash_node);

		if (fnode->super != super)
			continue;

		if (!test(fnode, data))
			continue;

		if (fnode->state & (FNODE_FREEING | FNODE_CLEAN)) {
			fnode_wait_io_locked(fnode);
			goto repeat;
		}

		return fnode;
	}
	
	return NULL;
}

static struct file_node *
fnode_alloc_special(struct super_block *super, struct hash_list_bucket *head,
	int (*test)(struct file_node *, void *), int (*set)(struct file_node *, void *),
	void *data)
{
	struct file_node *ret;

	ret = __fnode_alloc(super);
	if (ret) {
		struct file_node *found;

		smp_lock(&filenode_lock);
		/**
		 * 在哈希表中重新查找
		 */
		found = find_inode(super, head, test, data);
		/**
		 * 缓存中仍然没有，必须要用新分配的节点
		 */
		if (!found) {
			if (set(ret, data)) {
				smp_unlock(&filenode_lock);
				fnode_free(ret);
				return NULL;
			}

			fnode_stat.nr_inodes++;
			list_insert_front(&ret->list, &filenode_used);
			list_insert_front(&ret->super_block, &super->file_nodes);
			hlist_add_head(&ret->hash_node, head);
			ret->state = FNODE_TRANSFERRING | FNODE_NEW;
			smp_unlock(&filenode_lock);

			return ret;
		}

		/**
		 * 竟然有人已经添加了节点
		 * 先引用该节点，并释放刚分配的节点
		 */
		__hold_file_node(found);
		smp_unlock(&filenode_lock);
		fnode_free(ret);
		ret = found;
		/**
		 * 等待旧节点数据传输完毕
		 */
		writeback_fnode_wait(ret);
	}

	return ret;
}

/**
 * 在哈希表中查找文件节点，提供文件系统特定的比较方法
 * 如果不存在则创建一个
 */
struct file_node *fnode_find_alloc_special(struct super_block *super,
	unsigned long hashval, int (*test)(struct file_node *, void *),
	int (*set)(struct file_node *, void *), void *data)
{
	struct hash_list_bucket *head;
	struct file_node *fnode;

	ASSERT(test && set);
	head = hash(super, hashval);

	smp_lock(&filenode_lock);
	fnode = find_inode(super, head, test, data);
	if (fnode) {
		__hold_file_node(fnode);
		smp_unlock(&filenode_lock);

		writeback_fnode_wait(fnode);

		return fnode;
	} else
		smp_unlock(&filenode_lock);

	return fnode_alloc_special(super, head, test, set, data);
}

static int fnode_test(struct file_node *fnode, void *data)
{
	return fnode->node_num == *(unsigned long *)data;
}

static int fnode_set(struct file_node *fnode, void *data)
{
	fnode->node_num = *(unsigned long *)data;

	return 0;
}

struct file_node *
fnode_find_alloc(struct super_block *super, unsigned long fnode_num)
{
	return fnode_find_alloc_special(super, fnode_num,
		fnode_test, fnode_set, &fnode_num);
}

/**
 * 从块设备中加载文件节点信息
 * 并将节点添加到内存数据结构中
 */
struct file_node *
fnode_read(struct super_block *super, unsigned long fnode_num)
{
	struct file_node *file_node = fnode_find_alloc(super, fnode_num);
	
	if (file_node && (file_node->state & FNODE_NEW)) {
		super->ops->read_fnode(file_node);
		file_node->state &= ~(FNODE_TRANSFERRING|FNODE_NEW);
		wake_up_filenode(file_node);
	}

	return file_node;
}

/**
 * 将空闲文件节点收集到链表中，待释放
 */
static int collect_free_fnode(struct double_list *head, struct double_list *dispose)
{
	struct double_list *tmp, *next;
	int busy = 0, count = 0;

	list_for_each_safe(tmp, next, head) {
		struct file_node * fnode;

		fnode = list_container(tmp, struct file_node, super_block);
		/**
		 * 可以安全的释放节点
		 */
		if (!accurate_read(&fnode->ref_count)) {
			hlist_del_init(&fnode->hash_node);
			list_del(&fnode->super_block);
			list_move_to_front(&fnode->list, dispose);
			fnode->state |= FNODE_FREEING;
			count++;
			continue;
		}
		/**
		 * 有节点正在使用中
		 */
		busy = 1;
	}

	fnode_stat.nr_unused -= count;

	return busy;
}

/**
 * 批量的释放空闲文件节点
 */
static void bulk_free(struct double_list *head)
{
	int count = 0;

	while (!list_is_empty(head)) {
		struct file_node *fnode;

		fnode = list_container(head->next, struct file_node, list);
		list_del(&fnode->list);

		if (fnode->cache_data.page_count)
			truncate_inode_pages(&fnode->cache_data, 0);
		fnode_clean(fnode);
		fnode_free(fnode);
		count++;
	}
	smp_lock(&filenode_lock);
	fnode_stat.nr_inodes -= count;
	smp_unlock(&filenode_lock);
}

/**
 * 将文件系统内所有节点失效
 */
int invalidate_fnode_filesystem(struct super_block *super)
{
	struct double_list throw_away = 
		LIST_HEAD_INITIALIZER(throw_away);
	int busy;

	down(&del_sem);
	smp_lock(&filenode_lock);
	busy = collect_free_fnode(&super->file_nodes, &throw_away);
	smp_unlock(&filenode_lock);

	bulk_free(&throw_away);
	up(&del_sem);

	return busy;
}

int invalidate_fnode_device(struct block_device *bdev, int sync)
{
	struct super_block *super;
	int ret;

	if (sync)
		fsync_blkdev(bdev);

	ret = 0;
	super = get_associated_superblock(bdev);
	if (super) {
		/**
		 * 回收文件系统的文件节点缓存
		 */
		shrink_fnode_cache(super);
		/**
		 * 使文件系统中的文件节点无效
		 */
		ret = invalidate_fnode_filesystem(super);
		/**
		 * 释放文件系统
		 */
		unlock_loosen_super_block(super);
	}

	/**
	 * 使块设备的页面缓存失效
	 */
	invalidate_page_cache(bdev->fnode->cache_space);

	return ret;
}

/**
 * FIFO/SOCKET类型的文件暂不实现
 */
static int sock_bad_open(struct file_node *fnode, struct file *file)
{
	return -ENXIO;
}

struct file_ops bad_sock_fops = {
	.open = sock_bad_open,
};

static int fifo_bad_open(struct file_node *fnode, struct file *file)
{
	return -ENXIO;
}

struct file_ops bad_fifo_fops = {
	.open		= fifo_bad_open,	
};

/**
 * 设置特殊节点的回调方法
 */
void init_special_filenode(struct file_node *fnode,
	umode_t mode, devno_t rdev)
{
	fnode->mode = mode;
	if (S_ISCHR(mode)) {
		fnode->file_ops = &def_chrdev_fops;
		fnode->devno = rdev;
	} else if (S_ISBLK(mode)) {
		fnode->file_ops = &def_blk_fops;
		fnode->devno = rdev;
	} else if (S_ISFIFO(mode))
		fnode->file_ops = &bad_fifo_fops;
	else if (S_ISSOCK(mode))
		fnode->file_ops = &bad_sock_fops;
	else
		WARN("bogus mode (%o)\n", mode);
}

/**
 * file-node早期初始化
 * 为哈希表分配大块内存，并初始化哈希表
 */
void __init init_file_node_early(void)
{
	int i;

	__hash = alloc_boot_mem_stretch(sizeof(struct hash_list_bucket),
					10, &__hash_order);
	__hash_mask = (1 << __hash_order) - 1;

	for (i = 0; i < __hash_mask; i++)
		hash_list_init_bucket(&__hash[i]);
}

void __init init_file_node(void)
{
	fnode_allotter = beehive_create("file_node", sizeof(struct file_node),
				0, BEEHIVE_PANIC, NULL);
}
