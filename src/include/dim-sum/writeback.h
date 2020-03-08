#ifndef _DIM_SUM_WRITEBACK_H
#define _DIM_SUM_WRITEBACK_H

#include <dim-sum/kernel.h>
#include <dim-sum/wait.h>
#include <dim-sum/sched.h>

struct blkdev_infrast;

/**
 * 正在使用的索引节点链表。不脏且i_count不为0.
 */
extern struct double_list filenode_used;
/**
 * 有效未使用的索引节点链表。不脏且i_count为0.用于磁盘高速缓存。
 */
extern struct double_list filenode_unused;

enum writeback_sync_modes {
	WB_SYNC_NONE,
	/**
	 * 等待IO
	 */
	WB_SYNC_WAIT,
	/**
	 * 不等待IO
	 */
	WB_SYNC_NOWAIT,
};

enum {
	/**
	 * 不允许阻塞，回写时使用
	 */
	__WB_NOBLOCK,
	/**
	 * 设备拥塞
	 */
	__WB_CONGESTED,
	/**
	 * 周期性回写
	 */
	__WB_PERIODIC,
	/**
	 * 页面回收
	 */
	__WB_RECLAIM,
};

#define WB_NOBLOCK	(1UL << __WB_NOBLOCK)
#define WB_CONGESTED	(1UL << __WB_CONGESTED)
#define WB_PERIODIC		(1UL << __WB_PERIODIC)
#define WB_RECLAIM		(1UL << __WB_RECLAIM)
/**
 * 脏页刷新控制结构。
 */
struct writeback_control {
	unsigned long flags;
	bool done;
	bool scanned;
	int res;
	/**
	 * 如果不为空，即指向一个backing_dev_info结构。此时，只有属于基本块设备的脏页将会被刷新。
	 */
	struct blkdev_infrast *infrast;	/* If !NULL, only write back this
					   queue */
	/**
	 * 同步模式。
	 *     WB_SYNC_WAIT:表示如果遇到一个上锁的索引节点，必须等待而不能略过它。
	 *     WB_SYNC_NOWAIT:表示把上锁的索引节点放入稍后的链表中。
	 *     WB_SYNC_NONE:表示简单的略过上锁的索引节点。
	 */
	enum writeback_sync_modes sync_mode;
	/**
	 * 如果不为空，就表示应该略过比指定值还新的索引节点。
	 */
	unsigned long *dirtied_jiffies;
	/**
	 * 当前执行流中仍然要写的脏页数量。
	 */ 
	long remain_page_count;		/* Write this many pages, and decrement
					   this for each page written */
	long skipped_page_count;		/* Pages which were not written */

	/*
	 * For ops->writepages(): is start or end are non-zero then this is
	 * a hint that the filesystem need only write out the pages inside that
	 * byterange.  The byte at `end' is included in the writeout request.
	 */
	loff_t start;
	loff_t end;
};


void wake_up_filenode(struct file_node *file_node);
int fnode_schedule(struct bit_wait_task_desc *);

/* writeback.h requires fs.h; it, too, is not included from here. */
static inline void writeback_fnode_wait(struct file_node *file_node)
{
	might_sleep();
	wait_on_bit(&file_node->state, __FNODE_TRANSFERRING, TASK_UNINTERRUPTIBLE);
}

void balance_dirty_pages_ratelimited(struct file_cache_space *space);

extern struct smp_lock filenode_lock;
void sync_filesystem_fnodes(struct super_block *, int wait);

int __writeback_submit_data(struct file_cache_space *space, struct writeback_control *wbc);

int kick_writeback_task(long nr_pages);
int __kick_writeback_task(void (*fn)(unsigned long), unsigned long arg0);
void writeback_period(void);
#endif /* _DIM_SUM_WRITEBACK_H */
