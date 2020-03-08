#ifndef __DIM_SUM_MUTEX_H
#define __DIM_SUM_MUTEX_H

#include <dim-sum/double_list.h>
#include <dim-sum/smp_lock.h>
#include <dim-sum/linkage.h>
#include <dim-sum/accurate_counter.h>

#include <asm/current.h>
#include <asm/processor.h>

/**
 * 睡眠互斥锁
 */
struct mutex {
	/**
	 * 1、未锁
	 * 0、锁定，无等待者
	 * -1、锁定，有等待者
	 */
	struct accurate_counter count;
	/**
	 * 保护等待队列的锁
	 */
	struct smp_lock wait_lock;
	/**
	 * 等待队列
	 */
	struct double_list wait_list;
};

#define MUTEX_INITIALIZER(lockname) \
	{							\
		.count = ACCURATE_COUNTER_INIT(1),		\
		.wait_lock = SMP_LOCK_UNLOCKED(lockname.wait_lock),	\
		.wait_list = LIST_HEAD_INITIALIZER(lockname.wait_list)	\
	}
extern void mutex_init(struct mutex *lock);
static inline void mutex_destroy(struct mutex *lock)
{
}

static inline int mutex_is_locked(struct mutex *lock)
{
	return accurate_read(&lock->count) != 1;
}

extern void mutex_lock(struct mutex *lock);
extern int __must_check mutex_lock_interruptible(struct mutex *lock);
extern int mutex_trylock(struct mutex *lock);
extern void mutex_unlock(struct mutex *lock);

#endif /* __DIM_SUM_MUTEX_H */
