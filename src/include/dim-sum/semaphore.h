#ifndef __DIM_SUM_SEMAPHORE_H
#define __DIM_SUM_SEMAPHORE_H

#include <dim-sum/double_list.h>
#include <dim-sum/smp_lock.h>

/**
 * 内核信号量结构
 */
struct semaphore{
	struct smp_lock lock;
	/**
	 * 大于0，表示资源是空闲的。
	 * 等于0，表示信号量是忙的，但是没有进程在等待这个资源。
	 * 小于0，表示资源忙，并且至少有一个进程在等待。
	 * 但是负值并不代表等待的进程数量。
	 */
	unsigned int count;
	struct double_list wait_list;
};

#define SEMAPHORE_INITIALIZER(name, cnt)				\
{									\
	.lock = SMP_LOCK_UNLOCKED((name).lock),	\
	.count = cnt,						\
	.wait_list	= LIST_HEAD_INITIALIZER((name).wait_list),		\
}

static inline void sema_init(struct semaphore *sem, int val)
{
	smp_lock_init(&sem->lock);
	sem->count = val;
	list_init(&sem->wait_list);
}

extern void down(struct semaphore *sem);
extern int __must_check down_interruptible(struct semaphore *sem);
extern int __must_check down_trylock(struct semaphore *sem);
extern int __must_check down_timeout(struct semaphore *sem, long jiffies);
extern void up(struct semaphore *sem);

#endif /* __DIM_SUM_SEMAPHORE_H */
