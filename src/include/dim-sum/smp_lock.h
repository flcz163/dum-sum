#ifndef __DIM_SUM_SMP_LOCK_H
#define __DIM_SUM_SMP_LOCK_H

#include <dim-sum/accurate_counter.h>
#include <dim-sum/irqflags.h>
#include <dim-sum/preempt.h>

#include <asm/smp_lock.h>

struct smp_lock {
	struct arch_smp_lock lock;
};

#define __SMP_LOCK_INITIALIZER(lockname)	\
	{					\
		.lock = __ARCH_SMP_LOCK_UNLOCKED,	\
	}

#define SMP_LOCK_UNLOCKED(lockname)	\
	(struct smp_lock) __SMP_LOCK_INITIALIZER(lockname)

static inline void smp_lock_init(struct smp_lock *lock)
{
	*(lock) = SMP_LOCK_UNLOCKED(lock);
}

static inline int smp_lock_is_locked(struct smp_lock *lock)
{
	return arch_smp_lock_is_locked(&lock->lock);
}

static inline void assert_smp_lock_is_locked(struct smp_lock *lock)
{
	BUG_ON(!smp_lock_is_locked(lock));
}

extern void smp_lock(struct smp_lock *lock);

#define smp_lock_irqsave(slock, flags)				\
do {								\
	typecheck(unsigned long, flags);	\
	local_irq_save(flags);			\
	preempt_disable();				\
	arch_smp_lock(&(slock)->lock);	\
} while (0)

extern int smp_trylock(struct smp_lock *lock);
extern void smp_lock_irq(struct smp_lock *lock);

#define smp_trylock_irqsave(lock, flags)			\
({ \
	local_irq_save(flags); \
	smp_trylock(lock) ? \
	1 : ({ local_irq_restore(flags); 0; }); \
})

extern void smp_unlock(struct smp_lock *lock);

#define smp_unlock_irqrestore(slock, flags)		\
{										\
	typecheck(unsigned long, flags);		\
	arch_smp_unlock(&(slock)->lock);		\
	local_irq_restore(flags);				\
	preempt_enable();					\
}

extern void smp_unlock_irq(struct smp_lock *lock);

extern int _atomic_dec_and_lock(struct accurate_counter *atomic, struct smp_lock *lock);
#define atomic_dec_and_lock(atomic, lock) \
		__cond_lock(lock, _atomic_dec_and_lock(atomic, lock))

#endif /* __DIM_SUM_SMP_LOCK_H */
