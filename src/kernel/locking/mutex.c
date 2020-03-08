#include <dim-sum/errno.h>
#include <dim-sum/mutex.h>
#include <dim-sum/sched.h>
#include <dim-sum/smp_lock.h>

/*
 * 挂在互斥锁等待队列上的任务
 */
struct mutex_waiter {
	/**
	 * 通过此字段将任务添加到等待队列中
	 */
	struct double_list list;
	/**
	 * 等待任务
	 */
	struct task_desc *task;
};

void mutex_init(struct mutex *lock)
{
	accurate_set(&lock->count, 1);
	smp_lock_init(&lock->wait_lock);
	list_init(&lock->wait_list);
}

static int __mutex_lock_slow(struct mutex *lock, long state)
{
	struct task_desc *task = current;
	struct mutex_waiter waiter;
	unsigned long count;

	smp_lock(&lock->wait_lock);

	waiter.task = task;
	list_init(&waiter.list);
	/**
	 * 按FIFO的方式，插入到等待链表的最后
	 */
	list_insert_behind(&waiter.list, &lock->wait_list);

	while (1) {
		count = accurate_xchg(&lock->count, -1);
		if (count == 1)
			break;

		/**
 		 * 被信号打断，返回错误
		 */
		if (unlikely(state == TASK_INTERRUPTIBLE &&
		    signal_pending(task))) {
			list_del(&waiter.list);
			smp_unlock(&lock->wait_lock);

			return -EINTR;
		}

		__set_task_state(task, state);
		smp_unlock(&lock->wait_lock);
		schedule();
		smp_lock(&lock->wait_lock);
	}

	list_del(&waiter.list);
	if (likely(list_is_empty(&lock->wait_list)))
		accurate_set(&lock->count, 0);

	smp_unlock(&lock->wait_lock);

	return 0;
}

void mutex_lock(struct mutex *lock)
{
	if (unlikely(accurate_dec(&lock->count) < 0))
		__mutex_lock_slow(lock, TASK_UNINTERRUPTIBLE);
}

int fastcall __sched mutex_lock_interruptible(struct mutex *lock)
{
	if (unlikely(accurate_dec(&lock->count) < 0))
		return __mutex_lock_slow(lock, TASK_INTERRUPTIBLE);
	else
		return 0;
}

int fastcall __sched mutex_trylock(struct mutex *lock)
{
	int count;

	smp_lock(&lock->wait_lock);

	count = accurate_xchg(&lock->count, -1);
	if (likely(list_is_empty(&lock->wait_list)))
		accurate_set(&lock->count, 0);

	smp_unlock(&lock->wait_lock);

	return count == 1;
}

void mutex_unlock(struct mutex *lock)
{
	/**
	 * accurate_inc有屏障的作用!
	 */
	if (unlikely(accurate_inc(&lock->count) <= 0)) {
		smp_lock(&lock->wait_lock);

		if (!list_is_empty(&lock->wait_list)) {
			struct mutex_waiter *waiter;

			waiter = list_first_container(&lock->wait_list,
				struct mutex_waiter, list);
			wake_up_process(waiter->task);
		}

		smp_unlock(&lock->wait_lock);
	}
}
