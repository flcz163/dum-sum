#include <dim-sum/blk_dev.h>
#include <dim-sum/blk_infrast.h>
#include <dim-sum/block_buf.h>
#include <dim-sum/delay.h>
#include <dim-sum/fs.h>
#include <dim-sum/pagemap.h>
#include <dim-sum/pagevec.h>
#include <dim-sum/timer.h>
#include <dim-sum/writeback.h>

/**
 * 单次回写的页面数量
 * 避免一次提交过多请求而导致设备拥塞
 */
#define MAX_WRITEBACK_PAGES	1024
int vm_dirty_ratio = 40;
int dirty_background_ratio = 10;

struct writeback_state
{
	unsigned long dirty_pages;
	unsigned long mapped_pages;
};

static void get_writeback_state(struct writeback_state *stat)
{
	stat->dirty_pages = approximate_page_statistics(fs_dirty);
	stat->mapped_pages = approximate_page_statistics(proc_mapped);
}

static void
get_dirty_limits(struct writeback_state *stat, long *pbackground,
	long *pdirty, struct file_cache_space *space)
{
	int background_ratio;
	int dirty_ratio;
	int unmapped_ratio;
	long background;
	long dirty;

	get_writeback_state(stat);

	unmapped_ratio = 100 - (stat->mapped_pages * 100) / total_pages;

	dirty_ratio = vm_dirty_ratio;
	if (dirty_ratio > unmapped_ratio / 2)
		dirty_ratio = unmapped_ratio / 2;

	if (dirty_ratio < 5)
		dirty_ratio = 5;

	background_ratio = dirty_background_ratio;
	if (background_ratio >= dirty_ratio)
		background_ratio = dirty_ratio / 2;

	background = (background_ratio * total_pages) / 100;
	dirty = (dirty_ratio * total_pages) / 100;

	if (rt_task(current)) {
		background += background / 4;
		dirty += dirty / 4;
	}

	*pbackground = background;
	*pdirty = dirty;
}

int test_clear_page_writeback(struct page_frame *page)
{
	struct file_cache_space *space = page_cache_space(page);
	int ret;

	if (space) {
		unsigned long flags;

		smp_lock_irqsave(&space->tree_lock, flags);
		ret = pgflag_test_clear_writeback(page);
		if (ret)
			radix_tree_tag_clear(&space->page_tree,
						page_index(page),
						PAGECACHE_TAG_WRITEBACK);
		smp_unlock_irqrestore(&space->tree_lock, flags);
	} else
		ret = pgflag_test_clear_writeback(page);

	return ret;
}

int test_set_page_writeback(struct page_frame *page)
{
	struct file_cache_space *space;
	int ret;

	space = page_cache_space(page);

	if (space) {
		unsigned long flags;

		smp_lock_irqsave(&space->tree_lock, flags);
		ret = pgflag_test_set_writeback(page);
		if (!ret)
			radix_tree_tag_set(&space->page_tree,
						page_index(page),
						PAGECACHE_TAG_WRITEBACK);
		if (!pgflag_dirty(page))
			radix_tree_tag_clear(&space->page_tree,
						page_index(page),
						PAGECACHE_TAG_DIRTY);
		smp_unlock_irqrestore(&space->tree_lock, flags);
	} else
		ret = pgflag_test_set_writeback(page);

	return ret;

}

int __set_page_dirty_nobuffers(struct page_frame *page)
{
	int ret = 0;

	if (!test_set_pageflag_dirty(page)) {
		struct file_cache_space *space = page_cache_space(page);
		struct file_cache_space *space_refresh;

		if (space) {
			smp_lock_irq(&space->tree_lock);
			space_refresh = page_cache_space(page);
			if (space_refresh) {
				BUG_ON(space_refresh != space);

				if (!space->blkdev_infrast->mem_device)
					inc_page_statistics(fs_dirty);

				radix_tree_tag_set(&space->page_tree,
					page_index(page), PAGECACHE_TAG_DIRTY);
			}
			smp_unlock_irq(&space->tree_lock);
			if (space->fnode) {
				__mark_filenode_dirty(space->fnode,
							FNODE_DIRTY_PAGES);
			}
		}
	}

	return ret;
}

int redirty_page_for_writepage(struct writeback_control *control, struct page_frame *page)
{
	control->skipped_page_count++;

	return __set_page_dirty_nobuffers(page);
}

/**
 * 在将文件节点加入到文件系统哈希表中以后
 * 调用本函数将页面标记为脏
 */
void __mark_filenode_dirty(struct file_node *fnode, int flags)
{
	struct super_block *super = fnode->super;

	/**
	 * 如果节点为脏，则通知文件系统
	 */
	if (flags & (FNODE_DIRTY_SYNC | FNODE_DIRTY_DATASYNC)) {
		if (super->ops->dirty_fnode)
			super->ops->dirty_fnode(fnode);
	}

	smp_mb();

	if ((fnode->state & flags) == flags)
		return;

	smp_lock(&filenode_lock);
	if ((fnode->state & flags) != flags) {
		const int was_dirty = fnode->state & FNODE_DIRTY;

		fnode->state |= flags;

		/*
		 * 如果节点正在回写数据，则回写过程会管理链表
		 */
		if (fnode->state & FNODE_TRANSFERRING)
			goto out;

		if (!S_ISBLK(fnode->mode)) {
			if (hash_node_is_unhashed(&fnode->hash_node))
				goto out;
		}

		if (fnode->state & (FNODE_FREEING | FNODE_CLEAN))
			goto out;

		if (!was_dirty) {
			fnode->dirtied_jiffies = jiffies;
			list_move_to_front(&fnode->list, &super->dirty_nodes);
		}
	}
out:
	smp_unlock(&filenode_lock);
}

int clear_page_dirty_for_io(struct page_frame *page)
{
	struct file_cache_space *space = page_cache_space(page);

	if (space) {
		if (pgflag_test_clear_dirty(page)) {
			if (!space->blkdev_infrast->mem_device)
				dec_page_statistics(fs_dirty);
	
			return 1;
		}

		return 0;
	}

	return pgflag_test_clear_dirty(page);
}

int test_clear_page_dirty(struct page_frame *page)
{
	struct file_cache_space *space;
	unsigned long flags;

	space = page_cache_space(page);

	if (space) {
		smp_lock_irqsave(&space->tree_lock, flags);
		if (pgflag_test_clear_dirty(page)) {
			radix_tree_tag_clear(&space->page_tree,
						page_index(page),
						PAGECACHE_TAG_DIRTY);
			smp_unlock_irqrestore(&space->tree_lock, flags);

			if (!space->blkdev_infrast->mem_device)
				dec_page_statistics(fs_dirty);

			return 1;
		}
		smp_unlock_irqrestore(&space->tree_lock, flags);

		return 0;
	}

	return pgflag_test_clear_dirty(page);
}

/**
 * 在锁的保护下，回写文件节点到磁盘
 */
static int __sync_fnode(struct file_node *fnode,
	struct writeback_control *control)
{
	struct file_cache_space *space;
	struct super_block *super;
	unsigned dirty;
	int wait;
	int ret;

	space = fnode->cache_space;
	super = fnode->super;
	wait = control->sync_mode == WB_SYNC_WAIT;
	dirty = fnode->state & FNODE_DIRTY;

	ASSERT(!(fnode->state & FNODE_TRANSFERRING));

	fnode->state |= FNODE_TRANSFERRING;
	fnode->state &= ~FNODE_DIRTY;

	/**
	 * 释放上层函数获得的文件节点锁
	 */
	smp_unlock(&filenode_lock);

	/**
	 * 调用文件系统的write_page回调来回写页面
	 */
	ret = __writeback_submit_data(space, control);

	/**
	 * 如果文件节点元数据脏
	 */
	if (dirty & (FNODE_DIRTY_SYNC | FNODE_DIRTY_DATASYNC)) {
		int err = 0;

		/**
		 * 回写元数据
		 */
		if (fnode->super->ops->write_fnode && !is_bad_file_node(fnode))
			err = fnode->super->ops->write_fnode(fnode, wait);
	
		if (ret == 0)
			ret = err;
	}

	if (wait) {
		int err = writeback_wait_data(space);

		if (ret == 0)
			ret = err;
	}

	smp_lock(&filenode_lock);
	/**
	 * 清除*传输*标志
	 */
	fnode->state &= ~FNODE_TRANSFERRING;
	if (!(fnode->state & FNODE_FREEING)) {
		/**
		 * 节点脏标志被当前过程清除以后
		 * 没有其他进程再置
		 * 但是实际上没有将页面数据全部写入磁盘
		 */
		if (!(fnode->state & FNODE_DIRTY_PAGES) &&
		    cache_space_tagged(space, PAGECACHE_TAG_DIRTY)) {
			/**
			 * 将节点放回脏链表并重新置脏标志
			 */
			fnode->state |= FNODE_DIRTY_PAGES;
			list_move_to_behind(&fnode->list, &super->dirty_nodes);
			if (!(control->flags & WB_PERIODIC))
				fnode->dirtied_jiffies = jiffies;
		} else if (fnode->state & FNODE_DIRTY)
			list_move_to_front(&fnode->list, &super->dirty_nodes);
		else if (accurate_read(&fnode->ref_count))
			list_move_to_front(&fnode->list, &filenode_used);
		else {
			list_move_to_front(&fnode->list, &filenode_unused);
			fnode_stat.nr_unused++;
		}
	}

	wake_up_filenode(fnode);

	return ret;
}

/**
 * 回写脏页缓冲区
 */
static int
__writeback_fnode_sumbit(struct file_node *fnode,
			struct writeback_control *control)
{
	struct wait_queue *wqh;

	/**
	 * 如果正在传输节点数据
	 * 并且调用者不希望等待，则退出
	 */
	if ((control->sync_mode != WB_SYNC_WAIT) && (fnode->state & FNODE_TRANSFERRING)) {
		list_move_to_front(&fnode->list, &fnode->super->dirty_nodes);
		return 0;
	}

	/**
	 * 如果必须等待文件节点传输完毕，就等待。
	 */
	if (fnode->state & FNODE_TRANSFERRING) {
		DEFINE_WAIT_BIT(wq, &fnode->state, __FNODE_TRANSFERRING);

		wqh = bit_waitqueue(&fnode->state, __FNODE_TRANSFERRING);
		do {
			__hold_file_node(fnode);
			smp_unlock(&filenode_lock);
			__wait_on_bit(wqh, &wq, fnode_schedule, TASK_UNINTERRUPTIBLE);
			loosen_file_node(fnode);
			smp_lock(&filenode_lock);
		} while (fnode->state & FNODE_TRANSFERRING);
	}

	return __sync_fnode(fnode, control);
}

/**
 * 提交文件节点并等待其完成
 */
int writeback_fnode_sumbit_wait(struct file_node *fnode, int sync)
{
	int ret;
	struct writeback_control control = {
		.remain_page_count = LONG_MAX,
		.sync_mode = WB_SYNC_WAIT,
	};

	if (fnode->cache_space->blkdev_infrast->mem_device)
		return 0;

	might_sleep();
	smp_lock(&filenode_lock);
	ret = __writeback_fnode_sumbit(fnode, &control);
	smp_unlock(&filenode_lock);
	if (sync)
		writeback_fnode_wait(fnode);
	return ret;
}

/**
 * 将文件回写到磁盘
 */
int sync_fnode(struct file_node *fnode, struct writeback_control *control)
{
	int ret;

	smp_lock(&filenode_lock);
	ret = __writeback_fnode_sumbit(fnode, control);
	smp_unlock(&filenode_lock);

	return ret;
}

/**
 * 将超级块中的脏页写回到磁盘。
 */
static void
__writeback_filesystem_fnodes(struct super_block *super,
	struct writeback_control *control)
{
	const unsigned long start = jiffies;

	/**
	 * 周期性回写只关注sync链表
	 */
	if (!(control->flags & WB_PERIODIC) || list_is_empty(&super->sync_nodes)) {
		/**
		 * 将脏节点添加到sync链表
		 */
		list_combine_front(&super->dirty_nodes, &super->sync_nodes);
		list_init(&super->dirty_nodes);
	}

	/**
	 * 遍历sync链表中的所有节点，将其写入到磁盘
	 */
	while (!list_is_empty(&super->sync_nodes)) {
		struct file_cache_space *space;
		struct blkdev_infrast *infrast;
		long skipped_page_count;
		struct file_node *fnode;

		fnode = list_first_container(&super->sync_nodes,
						struct file_node, list);
		space = fnode->cache_space;
		infrast = space->blkdev_infrast;
		if (infrast->mem_device) {
			list_move_to_front(&fnode->list, &super->dirty_nodes);
			/**
			 * 对于块设备文件系统来说，每个节点的块设备不一定相同
			 * 略过当前内存块设备
			 */
			if (super == blkfs_superblock)
				continue;
			/*
			 * 对普通文件系统来说，所有文件节点的块设备是同一个
			 * 略过整个文件系统
			 */
			break;
		}

		/**
		 * 处理设备拥塞的情况
		 */
		if ((control->flags & WB_NOBLOCK) && blkdev_write_congested(infrast)) {
			control->flags |= WB_CONGESTED;
			if (super != blkfs_superblock)
				break;
			list_move_to_front(&fnode->list, &super->dirty_nodes);
			continue;
		}

		/**
		 * 如果用户指定 了回写设备
		 * 并且当前节点的块设备不匹配，则略过
		 */
		if (control->infrast && infrast != control->infrast) {
			if (super != blkfs_superblock)
				break;
			list_move_to_front(&fnode->list, &super->dirty_nodes);
			continue;
		}

		/**
		 * 本函数开始执行后才变脏的节点，略过
		 */
		if (time_after(fnode->dirtied_jiffies, start))
			continue;

		/**
		 * 用户控制了节点脏的时间
		 */
		if (control->dirtied_jiffies && time_after(fnode->dirtied_jiffies,
						*control->dirtied_jiffies))
			continue;

		ASSERT(!(fnode->state & FNODE_FREEING));
		/**
		 * 运行到这里，说明该脏页需要写回到磁盘。
		 * 首先将节点引用计数加1
		 */
		__hold_file_node(fnode);
		skipped_page_count = control->skipped_page_count;
		/**
		 * 回写脏缓冲区。
		 */
		__writeback_fnode_sumbit(fnode, control);
		if (control->sync_mode == WB_SYNC_NOWAIT) {
			fnode->dirtied_jiffies = jiffies;
			list_move_to_front(&fnode->list, &super->dirty_nodes);
		}

		/**
		 * 如果略过了节点中的某些页
		 * 就将这些锁定的页移到脏链表中。
		 */
		if (control->skipped_page_count != skipped_page_count)
			list_move_to_front(&fnode->list, &super->dirty_nodes);

		smp_unlock(&filenode_lock);
		/**
		 * 释放节点引用计数
		 */
		loosen_file_node(fnode);
		smp_lock(&filenode_lock);
		/**
		 * 回写完指定的页面
		 */
		if (control->remain_page_count <= 0)
			break;
	}

	return;		/* Leave any unwritten inodes on sync_nodes */
}

void sync_filesystem_fnodes(struct super_block *super, int wait)
{
	struct writeback_control control = {
			.sync_mode	= wait ? WB_SYNC_WAIT : WB_SYNC_NOWAIT,
		};
	unsigned long dirty_pages = approximate_page_statistics(fs_dirty);

	smp_lock(&filenode_lock);
	control.remain_page_count = dirty_pages +
			(fnode_stat.nr_inodes - fnode_stat.nr_unused);
	control.remain_page_count += control.remain_page_count / 2;
	__writeback_filesystem_fnodes(super, &control);
	smp_unlock(&filenode_lock);
}

static void set_syncing(int val)
{
	struct super_block *super;
	struct double_list *list;

	smp_lock(&super_block_lock);
	list_for_each(list, &all_super_blocks) {
		super = list_container(list, struct super_block, list);
		super->s_syncing = val;
	}
	smp_unlock(&super_block_lock);
}

static struct super_block *get_super_to_sync(void)
{
	struct super_block *super;
	struct double_list *list;

try_again:
	smp_lock(&super_block_lock);
	list_for_each(list, &all_super_blocks) {
		super = list_container(list, struct super_block, list);
		if (super->s_syncing)
			continue;

		super->s_syncing = 1;
		super->ref_count++;
		smp_unlock(&super_block_lock);

		down_read(&super->mount_sem);
		if (!super->root_fnode_cache) {
			unlock_loosen_super_block(super);
			goto try_again;
		}

		return super;
	}
	smp_unlock(&super_block_lock);

	return NULL;
}


/**
 * 将文件节点写入到磁盘中
 * 被sync系统调用所调用
 * 同步文件系统以后再同步其底层块设备
 */
void sync_file_nodes(int wait)
{
	struct super_block *super;

	set_syncing(0);

	while ((super = get_super_to_sync()) != NULL) {
		sync_filesystem_fnodes(super, 0);
		blkdev_sync(super->blkdev);
		unlock_loosen_super_block(super);
	}

	if (wait) {
		set_syncing(0);
		while ((super = get_super_to_sync()) != NULL) {
			sync_filesystem_fnodes(super, 1);
			blkdev_sync(super->blkdev);
			unlock_loosen_super_block(super);
		}
	}
}

/**
 * 在所有未锁定文件节点中，回写指定数量的脏页
 */
static void writeback_file_nodes(struct writeback_control *control)
{
	struct super_block *super;
	struct double_list *list;

	might_sleep();

	smp_lock(&super_block_lock);
try_again:
	/**
	 * 遍历所有超级块，含块设备文件系统
	 */
	list_for_each(list, &all_super_blocks) {
		super = list_container(list, struct super_block, list);
		/**
		 * 第一项是检查超级块的脏文件节点。
		 * 第二项是检查等待被传输到磁盘的文件节点。
		 */
		if (!list_is_empty(&super->dirty_nodes) || 
			!list_is_empty(&super->sync_nodes)) {
			super->ref_count++;
			smp_unlock(&super_block_lock);

			/**
			 * 无法获取超级块的mount锁，说明文件系统正在被卸载
			 * 直接略过即可
			 */
			if (down_read_trylock(&super->mount_sem)) {
				if (super->root_fnode_cache) {
					smp_lock(&filenode_lock);
					/**
					 * 回写单个文件系统内的文件
					 */
					__writeback_filesystem_fnodes(super, control);
					smp_unlock(&filenode_lock);
				}
				up_read(&super->mount_sem);
			}
			smp_lock(&super_block_lock);
			/**
			 * 超级块可能已经被卸载，导致全局链表变化
			 * 重新扫描链表
			 */
			if (list_is_empty(&super->list)) {
				__loosen_super(super);
				goto try_again;
			}
			__loosen_super(super);
		}
		/**
		 * 到达预期数量，停止扫描。
		 */
		if (control->remain_page_count <= 0)
			break;
	}
	smp_unlock(&super_block_lock);
}

/**
 * 扫描并回写脏页
 * 直到达到指定数量的页面或者 脏页低于阀值
 * 或者全部页面已经回写
 * page_count:	要刷新到磁盘的最少页数。
 */
static void writeback_background(unsigned long page_count)
{
	struct writeback_control control = {
		.flags	= WB_NOBLOCK,
		.infrast		= NULL,
		.sync_mode	= WB_SYNC_NONE,
		.dirtied_jiffies = NULL,
		.remain_page_count	= 0,
	};

	while (1) {
		struct writeback_state stat;
		long write_thresh;
		long dirty_thresh;

		get_dirty_limits(&stat, &write_thresh, &dirty_thresh, NULL);
		/**
		 * 请求的数量已经回写完毕，并且脏页数量不多
		 */
		if ((stat.dirty_pages <= dirty_thresh) && (page_count <= 0))
			break;

		control.flags &= ~WB_CONGESTED;
		control.remain_page_count = MAX_WRITEBACK_PAGES;
		control.skipped_page_count = 0;
		/**
		 * 遍历所有文件系统，回写所有文件
		 */
		writeback_file_nodes(&control);
		/**
		 * 检查有效写过的页的数量，并减少需要写的页的个数。
		 */
		page_count -= MAX_WRITEBACK_PAGES - control.remain_page_count;
		/**
		 * 没有更多的页面需要回写了，可以退出
		 */
		if (control.remain_page_count)
			break;
	}
}

/**
 * 唤醒磁盘回写内核线程。
 * 当内存不足，或者用户显示地请求刷新操作时，会执行此函数。
 *     用户态进程发出sync系统调用。
 *     分配一个新缓冲区页时失败。
 *     页面回收算法调用发现内存紧张
 * page_count:	应该刷新的脏页数量。0表示所有脏页都应该写回磁盘。
 */
int kick_writeback_task(long page_count)
{
	if (page_count == 0) {
		struct writeback_state stat;

		get_writeback_state(&stat);
		page_count = stat.dirty_pages;
	}

	/**
	 * 唤醒空闲回写线程，并委托它执行writeout_background函数。
	 */
	return __kick_writeback_task(writeback_background, page_count);
}

/**
 * 周期性的将旧文件写入到磁盘
 */
void writeback_period(void)
{
	/* TO-DO */
	//sys_sync();
}


/**
 * 写文件时，如果脏页过多
 * 就同步写一部分页面到磁盘中
 */
void balance_dirty_pages_ratelimited(struct file_cache_space *space)
{
	/* TO-DO */
}

