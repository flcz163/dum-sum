#include <dim-sum/sched.h>
#include <dim-sum/rwsem.h>

#define WAITING_FOR_READ 0x00000001
#define WAITING_FOR_WRITE 0x00000002

struct rwsem_waiter {
	struct double_list list;
	struct task_desc *task;
	unsigned int flags;
};

void init_rwsem(struct rw_semaphore *sem)
{
	sem->count = 0;
	smp_lock_init(&sem->wait_lock);
	list_init(&sem->wait_list);
}

void fastcall __sched __down_read(struct rw_semaphore *sem)
{
	struct rwsem_waiter waiter;
	struct task_desc *tsk;

	smp_lock(&sem->wait_lock);

	if (sem->count >= 0 && list_is_empty(&sem->wait_list)) {
		sem->count++;
		smp_unlock(&sem->wait_lock);

		return;
	}

	tsk = current;
	set_task_state(tsk, TASK_UNINTERRUPTIBLE);

	waiter.task = tsk;
	waiter.flags = WAITING_FOR_READ;
	hold_task_desc(tsk);
	list_insert_behind(&waiter.list, &sem->wait_list);

	smp_unlock(&sem->wait_lock);

	for (;;) {
		if (!waiter.task)
			break;
		schedule();
		set_task_state(tsk, TASK_UNINTERRUPTIBLE);
	}

	tsk->state = TASK_RUNNING;
}

int fastcall __down_read_trylock(struct rw_semaphore *sem)
{
	int ret = 0;

	smp_lock(&sem->wait_lock);

	if (sem->count >= 0 && list_is_empty(&sem->wait_list)) {
		sem->count++;
		ret = 1;
	}

	smp_unlock(&sem->wait_lock);

	return ret;
}

void fastcall __sched __down_write(struct rw_semaphore *sem)
{
	struct rwsem_waiter waiter;
	struct task_desc *tsk;

	smp_lock(&sem->wait_lock);

	/**
	 * 可用，并且竟然没有其他等待者
	 */
	if (sem->count == 0 && list_is_empty(&sem->wait_list)) {
		sem->count = -1;
		smp_unlock(&sem->wait_lock);

		return;
	}

	tsk = current;
	set_task_state(tsk, TASK_UNINTERRUPTIBLE);

	waiter.task = tsk;
	waiter.flags = WAITING_FOR_WRITE;
	hold_task_desc(tsk);
	list_insert_behind(&waiter.list, &sem->wait_list);

	smp_unlock(&sem->wait_lock);

	while (1) {
		if (!waiter.task)
			break;
		schedule();
		set_task_state(tsk, TASK_UNINTERRUPTIBLE);
	}

	tsk->state = TASK_RUNNING;
}

int fastcall __down_write_trylock(struct rw_semaphore *sem)
{
	int ret = 0;

	smp_lock(&sem->wait_lock);

	/**
	 * 可用，并且竟然没有其他等待者
	 */
	if (sem->count == 0 && list_is_empty(&sem->wait_list)) {
		sem->count = -1;
		ret = 1;
	}

	smp_unlock(&sem->wait_lock);

	return ret;
}

void fastcall __up_read(struct rw_semaphore *sem)
{
	smp_lock(&sem->wait_lock);

	sem->count--;
	/**
	 * 所有读者都退出，唤醒第一个写者
	 */
	if (sem->count == 0 && !list_is_empty(&sem->wait_list)) {
		struct rwsem_waiter *waiter;
		struct task_desc *tsk;

		/**
		 * 取出第一节点并从链表中删除
		 */
		sem->count = -1;
		waiter = list_first_container(&sem->wait_list,
			struct rwsem_waiter, list);
		list_del(&waiter->list);

		/**
		 * 唤醒写者任务
		 */
		tsk = waiter->task;
		waiter->task = NULL;
		wake_up_process(tsk);
		loosen_task_desc(tsk);
	}

	smp_unlock(&sem->wait_lock);
}

/**
 * 释放写锁时，唤醒等待任务
 */
static inline void __do_wake(struct rw_semaphore *sem, int wake_write)
{
	struct rwsem_waiter *waiter;
	struct task_desc *tsk;
	int woken;

	waiter = list_first_container(&sem->wait_list, struct rwsem_waiter, list);
	/**
	 * 允许唤醒写者，并且第一个等待者正好是写者
	 */
	if (wake_write && (waiter->flags & WAITING_FOR_WRITE)) {
		sem->count = -1;
		list_del(&waiter->list);
		tsk = waiter->task;
		smp_mb();
		waiter->task = NULL;
		wake_up_process(tsk);
		loosen_task_desc(tsk);

		return;
	}

	/**
	 * 第一个等待者是写者，但是不允许唤醒写者，只能退出
	 */
	if (!wake_write) {
		if (waiter->flags & WAITING_FOR_WRITE)
			return;
	}

	/**
	 * 连续唤醒多个读者
	 */
	woken = 0;
	while (waiter->flags & WAITING_FOR_READ) {
		list_del(&waiter->list);
		tsk = waiter->task;
		waiter->task = NULL;
		wake_up_process(tsk);
		loosen_task_desc(tsk);
		woken++;

		if (list_is_empty(&sem->wait_list))
			break;

		waiter = list_next_entry(waiter, list);
	}

	sem->count += woken;
}

void fastcall __up_write(struct rw_semaphore *sem)
{
	smp_lock(&sem->wait_lock);

	sem->count = 0;
	if (!list_is_empty(&sem->wait_list))
		__do_wake(sem, 1);

	smp_unlock(&sem->wait_lock);
}

void fastcall __downgrade_write(struct rw_semaphore *sem)
{
	smp_lock(&sem->wait_lock);

	/**
	 * 降为读者后，目前读者数量为1
	 */
	sem->count = 1;
	if (!list_is_empty(&sem->wait_list))
		__do_wake(sem, 0);

	smp_unlock(&sem->wait_lock);
}
