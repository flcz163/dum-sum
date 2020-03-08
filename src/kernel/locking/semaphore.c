#include <dim-sum/errno.h>
#include <dim-sum/sched.h>
#include <dim-sum/semaphore.h>

struct semaphore_waiter {
	struct double_list list;
	struct task_desc *task;
};

static inline int __sched
__down_common(struct semaphore *sem, long state, long timeout)
{
	struct semaphore_waiter waiter;
	struct task_desc *task;
	int ret = 0;

	task = current;
	waiter.task = task;
	list_init(&waiter.list);
	list_insert_behind(&waiter.list, &sem->wait_list);

	while (1) {
		if (signal_pending(task)) {
			ret = -EINTR;
			goto out;
		}

		if (unlikely(timeout <= 0)) {
			ret = -ETIME;
			goto out;
		}

		__set_task_state(task, state);
		smp_unlock_irq(&sem->lock);
		timeout = schedule_timeout(timeout);
		smp_lock_irq(&sem->lock);

		if (waiter.task == NULL)
			return ret;
	}

 out:
	list_del(&waiter.list);
	return ret;
}

void down(struct semaphore *sem)
{
	unsigned long flags;

	smp_lock_irqsave(&sem->lock, flags);

	if (likely(sem->count > 0))
		sem->count--;
	else
		__down_common(sem, TASK_UNINTERRUPTIBLE, MAX_SCHEDULE_TIMEOUT);

	smp_unlock_irqrestore(&sem->lock, flags);
}

int down_interruptible(struct semaphore *sem)
{
	unsigned long flags;
	int ret = 0;

	smp_lock_irqsave(&sem->lock, flags);

	if (likely(sem->count > 0))
		sem->count--;
	else
		ret = __down_common(sem, TASK_INTERRUPTIBLE, MAX_SCHEDULE_TIMEOUT);

	smp_unlock_irqrestore(&sem->lock, flags);

	return ret;
}

int down_trylock(struct semaphore *sem)
{
	unsigned long flags;
	int count;

	smp_lock_irqsave(&sem->lock, flags);

	count = sem->count - 1;
	if (likely(count >= 0))
		sem->count = count;

	smp_unlock_irqrestore(&sem->lock, flags);

	return count < 0;
}

int down_timeout(struct semaphore *sem, long timeout)
{
	unsigned long flags;
	int ret = 0;

	smp_lock_irqsave(&sem->lock, flags);

	if (likely(sem->count > 0))
		sem->count--;
	else
		ret = __down_common(sem, TASK_UNINTERRUPTIBLE, timeout);

	smp_unlock_irqrestore(&sem->lock, flags);

	return ret;
}

void up(struct semaphore *sem)
{
	unsigned long flags;

	smp_lock_irqsave(&sem->lock, flags);

	if (likely(list_is_empty(&sem->wait_list)))
		sem->count++;
	else {
		struct semaphore_waiter *waiter;

		waiter = list_first_container(&sem->wait_list,
			struct semaphore_waiter, list);
		list_del(&waiter->list);
		waiter->task = NULL;
		wake_up_process(waiter->task);
	}

	smp_unlock_irqrestore(&sem->lock, flags);
}
