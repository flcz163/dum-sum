#ifndef __DIM_SUM_SMP_RWLOCK_H
#define __DIM_SUM_SMP_RWLOCK_H

#include <asm/smp_rwlock.h>

struct smp_rwlock {
	struct arch_smp_rwlock raw_lock;
};

#define SMP_RWLOCK_UNLOCKED(lockname) \
	(struct smp_rwlock) {	.raw_lock = __ARCH_SMP_RWLOCK_UNLOCKED, }	\

#define smp_rwlock_init(lock)					\
	do { *(lock) = SMP_RWLOCK_UNLOCKED(lock); } while (0)

#define smp_rwlock_can_read(rwlock)		arch_smp_rwlock_can_read(&(rwlock)->raw_lock)
#define smp_rwlock_can_write(rwlock)		arch_smp_rwlock_can_write(&(rwlock)->raw_lock)

extern int smp_tryread(struct smp_rwlock *lock);
extern int smp_trywrite(struct smp_rwlock *lock);
extern void smp_write_lock(struct smp_rwlock *lock);
extern void smp_write_lock_irq(struct smp_rwlock *lock);
extern void smp_read_lock(struct smp_rwlock *lock);

#define smp_read_lock_irqsave(lock, flags)			\
	do {						\
		typecheck(unsigned long, flags);	\
		local_irq_save(flags);			\
		preempt_disable();				\
		arch_smp_read_lock(&(lock)->raw_lock);	\
	} while (0)

#define smp_write_lock_irqsave(lock, flags)			\
	do {						\
		typecheck(unsigned long, flags);	\
		local_irq_save(flags);			\
		preempt_disable();				\
		arch_smp_write_lock(&(lock)->raw_lock);	\
	} while (0)

extern void smp_read_unlock(struct smp_rwlock *lock);
extern void smp_write_unlock(struct smp_rwlock *lock);
extern void smp_write_unlock_irq(struct smp_rwlock *lock);

#define smp_read_unlock_irqrestore(lock, flags)			\
	do {												\
		typecheck(unsigned long, flags);				\
		arch_smp_read_unlock(&(lock)->raw_lock);		\
		local_irq_restore(flags);						\
		preempt_enable();							\
	} while (0)

#define smp_write_unlock_irqrestore(lock, flags)		\
	do {												\
		typecheck(unsigned long, flags);				\
		arch_smp_write_unlock(&(lock)->raw_lock);				\
		local_irq_restore(flags);						\
		preempt_enable();							\
	} while (0)

#define smp_tryread_irqsave(lock, flags) \
({ \
	local_irq_save(flags); \
	smp_tryread(lock) ? \
	1 : ({ local_irq_restore(flags); 0; }); \
})

#define smp_trywrite_irqsave(lock, flags) \
({ \
	local_irq_save(flags); \
	smp_trywrite(lock) ? \
	1 : ({ local_irq_restore(flags); 0; }); \
})

#endif /* __DIM_SUM_SMP_RWLOCK_H */
