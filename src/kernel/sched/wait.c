#include <dim-sum/errno.h>
#include <dim-sum/fs.h>
#include <dim-sum/hash.h>
#include <dim-sum/mm.h>
#include <dim-sum/sched.h>
#include <dim-sum/timer.h>
#include <dim-sum/wait.h>

#include "internal.h"

void __init_waitqueue(struct wait_queue *q, const char *name)
{
	smp_lock_init(&q->lock);
	list_init(&q->awaited);
}

void fastcall add_to_wait_queue(struct wait_queue *q, struct wait_task_desc *wait)
{
	unsigned long flags;

	wait->flags &= ~WQ_FLAG_EXCLUSIVE;
	smp_lock_irqsave(&q->lock, flags);
	__add_to_wait_queue(q, wait);
	smp_unlock_irqrestore(&q->lock, flags);
}

void fastcall del_from_wait_queue(struct wait_queue *q, struct wait_task_desc *wait)
{
	unsigned long flags;

	smp_lock_irqsave(&q->lock, flags);
	__del_from_wait_queue(q, wait);
	smp_unlock_irqrestore(&q->lock, flags);
}

void
prepare_to_wait(struct wait_queue *q, struct wait_task_desc *wait, int state)
{
	unsigned long flags;

	wait->flags &= ~WQ_FLAG_EXCLUSIVE;
	smp_lock_irqsave(&q->lock, flags);
	if (list_is_empty(&wait->list))
		__add_to_wait_queue(q, wait);
	set_current_state(state);
	smp_unlock_irqrestore(&q->lock, flags);
}

void
prepare_to_wait_exclusive(struct wait_queue *q, struct wait_task_desc *wait, int state)
{
	unsigned long flags;

	wait->flags |= WQ_FLAG_EXCLUSIVE;
	smp_lock_irqsave(&q->lock, flags);
	if (list_is_empty(&wait->list))
		__add_to_wait_queue_tail(q, wait);
	set_current_state(state);
	smp_unlock_irqrestore(&q->lock, flags);
}

void fastcall finish_wait(struct wait_queue *q, struct wait_task_desc *wait)
{
	unsigned long flags;

	__set_current_state(TASK_RUNNING);
	smp_lock_irqsave(&q->lock, flags);
	if (!list_is_empty(&wait->list)) {
		list_del_init(&wait->list);
	}
	smp_unlock_irqrestore(&q->lock, flags);
}

/**
 * 非互斥进程由default_wake_function唤醒。它是try_to_wake_up的一个简单封装。
 */
int wakeup_one_task_simple(struct wait_task_desc *curr, unsigned mode,
		int sync, void *data)
{
	struct task_desc *p = curr->task;

	return wake_up_process_special(p, mode, sync);
}

/**
 * 唤醒等待队列上的等待者
 */
int wakeup_one_task(struct wait_task_desc *wait, unsigned mode, int sync, void *data)
{
	int ret = wakeup_one_task_simple(wait, mode, sync, data);

	if (ret)
		list_del_init(&wait->list);
	return ret;
}

void __wake_up_common(struct wait_queue *q, unsigned int mode,
			     int nr_exclusive, int sync, void *data)
{
	struct double_list *curr, *next;

	list_for_each_safe(curr, next, &q->awaited) {
		struct wait_task_desc *wait;
		unsigned flags;
		wait = list_container(curr, struct wait_task_desc, list);
		flags = wait->flags;
		/**
		 * wakeup_one_task
		 * wakeup_one_task_simple
		 * wakeup_one_task_bit
		 */
		if (wait->wakeup(wait, mode, sync, data) &&
		    (flags & WQ_FLAG_EXCLUSIVE) &&
		    !--nr_exclusive)
			break;
	}
}

void fastcall __wake_up(struct wait_queue *q, unsigned int mode,
				int nr_exclusive, void *data)
{
	unsigned long flags;

	smp_lock_irqsave(&q->lock, flags);
	__wake_up_common(q, mode, nr_exclusive, 0, data);
	smp_unlock_irqrestore(&q->lock, flags);
}

/**
 * 开始在位图上等待
 */
int __sched
__wait_on_bit(struct wait_queue *wq, struct bit_wait_task_desc *q,
	      wait_bit_schedule_f *sched, unsigned mode)
{
	int ret = 0;

	do {
		/**
		 * 将当前任务挂到等待 队列上
		 */
		prepare_to_wait(wq, &q->wait, mode);
		/**
		 * 如果位图中的位已经置位，就必须等待了
		 */
		if (test_bit(q->bit_nr, q->addr))
			/**
			 * 根据调用者的要求，调用相应的调度函数
			 * 把自己调度出去。
			 */
			ret = sched(q);
	/**
	 * 虽然被唤醒了，但是可能存在以下情况需要重试
	 *	1、位图被其他人重新置位
	 *	2、伪唤醒，需要再次睡眠
	 * 理论上，第一个条件可以不要
	 * 调用者必须在外层调用处再次判断
	 */
	} while (test_bit(q->bit_nr, q->addr) && !ret);

	finish_wait(wq, &q->wait);

	return ret;
}

void abort_exclusive_wait(struct wait_queue *q, struct wait_task_desc *wait,
			unsigned int mode, void *key)
{
	unsigned long flags;

	__set_current_state(TASK_RUNNING);
	smp_lock_irqsave(&q->lock, flags);
	if (!list_is_empty(&wait->list))
		list_del_init(&wait->list);
	else if (waitqueue_active(q))
		__wake_up_common(q, mode, 1, 0, key);
	smp_unlock_irqrestore(&q->lock, flags);
}

/**
 * 在位图上等待，成功后锁住位图
 */
int __sched
__wait_on_bit_lock(struct wait_queue *wq, struct bit_wait_task_desc *q,
			wait_bit_schedule_f *sched, unsigned mode)
{
	do {
		int ret;

		/**
		 * 先把自己挂到队列上
		 */
		prepare_to_wait_exclusive(wq, &q->wait, mode);
		/**
		 * 位图可用，试图锁住该位
		 */
		if (!test_bit(q->bit_nr, q->addr))
			continue;

		/**
		 * 把自己调度出去，等待其他人唤醒
		 */
		ret = sched(q);
		/**
		 * 意料之中的唤醒，赶快去锁住
		 */
		if (!ret)
			continue;

		/**
		 * 意外的唤醒，失败退出
		 */
		abort_exclusive_wait(wq, &q->wait, mode, q);

		return ret;
	} while (atomic_test_and_set_bit(q->bit_nr, q->addr));

	/**
	 * 结束等待，将自己从队列中摘除
	 * 同时设置任务状态为运行状态
	 */
	finish_wait(wq, &q->wait);

	return 0;
}

/**
 * 唤醒位图上等待的任务
 */
int wakeup_one_task_bit(struct wait_task_desc *wait, unsigned mode, int sync, void *arg)
{
	struct wait_bit_key *key = arg;
	struct bit_wait_task_desc *wait_bit
		= container_of(wait, struct bit_wait_task_desc, wait);

	/**
	 * 因为同一个等待队列中，不同任务在等待不同的位图
	 * 因此首先判断唤醒的位图是不是我们期望的
	 * 其次看位图是否真的被清除了。
	 */
	if (wait_bit->addr != key->addr ||
			wait_bit->bit_nr != key->bit_nr ||
			test_bit(key->bit_nr, key->addr))
		return 0;
	else
		/**
		 * 唤醒此任务
		 */
		return wakeup_one_task(wait, mode, sync, key);
}

/**
 * 找到等待位图所在队列
 */
struct wait_queue *bit_waitqueue(void *word, int bit)
{
	const int shift = BITS_PER_LONG == 32 ? 5 : 6;
	const struct page_area *pg_area = page_to_pgarea(linear_virt_to_page(word));
	unsigned long val = (unsigned long)word << shift | bit;

	return &pg_area->wait_table[hash_long(val, pg_area->wait_table_bits)];
}

/**
 * 唤醒队列中，符合条件的位图等待者
 */
void __wake_up_bit(struct wait_queue *wq, void *word, int bit)
{
	struct wait_bit_key key = __WAIT_BIT_KEY_INITIALIZER(word, bit);

	if (waitqueue_active(wq))
		__wake_up(wq, TASK_NORMAL, 1, &key);
}

void wake_up_bit(void *word, int bit)
{
	__wake_up_bit(bit_waitqueue(word, bit), word, bit);
}

__sched int sched_bit_wait(struct bit_wait_task_desc *word)
{
	if (signal_pending(current))
		return 1;

	schedule();

	return 0;
}

__sched int iosched_bit_wait(struct bit_wait_task_desc *word)
{
	struct file_cache_space *cache;
	struct page_frame *page;
	void *addr = word->addr;

	page = container_of((unsigned long *)addr, struct page_frame, flags);
	cache = page->cache_space;
	if (cache && cache->ops && cache->ops->sync_page)
		cache->ops->sync_page(page);

	io_schedule();

	return 0;
}

__sched int sched_timeout_bit_wait(struct bit_wait_task_desc *word)
{
	unsigned long now = ACCESS_ONCE(jiffies);

	if (time_after_eq(now, word->timeout))
		return -EAGAIN;

	schedule_timeout(word->timeout - now);

	return 0;
}
