#include <dim-sum/preempt.h>
#include <dim-sum/smp_lock.h>

void smp_lock(struct smp_lock *lock)
{
	preempt_disable();
	arch_smp_lock(&lock->lock);
}

int smp_trylock(struct smp_lock *lock)
{
	preempt_disable();

	if (arch_smp_trylock(&lock->lock))
		return 1;

	preempt_enable();

	return 0;
}

void smp_lock_irq(struct smp_lock *lock)
{
	disable_irq();
	preempt_disable();
	arch_smp_lock(&lock->lock);
}

void smp_unlock(struct smp_lock *lock)
{
	arch_smp_unlock(&lock->lock);
	preempt_enable();
}

void smp_unlock_irq(struct smp_lock *lock)
{
	arch_smp_unlock(&lock->lock);
	enable_irq();
	preempt_enable();
}