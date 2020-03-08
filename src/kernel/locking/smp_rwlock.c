#include <dim-sum/irqflags.h>
#include <dim-sum/preempt.h>
#include <dim-sum/smp_rwlock.h>

int smp_tryread(struct smp_rwlock *lock)
{
	preempt_disable();

	if (arch_smp_tryread(&lock->raw_lock))
		return 1;
	
	preempt_enable();

	return 0;
}

int smp_trywrite(struct smp_rwlock *lock)
{
	preempt_disable();

	if (arch_smp_trywrite(&lock->raw_lock))
		return 1;
	
	preempt_enable();

	return 0;
}

void smp_write_lock(struct smp_rwlock *lock)
{
	preempt_disable();
	arch_smp_write_lock(&lock->raw_lock);
}

void smp_write_lock_irq(struct smp_rwlock *lock)
{
	disable_irq();
	preempt_disable();
	arch_smp_write_lock(&lock->raw_lock);
}

void smp_read_lock(struct smp_rwlock *lock)
{
	preempt_disable();
	arch_smp_read_lock(&lock->raw_lock);;
}

void smp_read_unlock(struct smp_rwlock *lock)
{
	arch_smp_read_unlock(&lock->raw_lock);
	preempt_enable();
}

void smp_write_unlock(struct smp_rwlock *lock)
{
	arch_smp_write_unlock(&lock->raw_lock);
	preempt_enable();
}

void smp_write_unlock_irq(struct smp_rwlock *lock)
{
	arch_smp_write_unlock(&lock->raw_lock);
	enable_irq();
	preempt_enable();
}