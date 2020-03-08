#include <dim-sum/irqflags.h>
#include <dim-sum/preempt.h>
#include <dim-sum/smp_bit_lock.h>

#include <asm/processor.h>

void smp_bit_lock(int bitnum, unsigned long *addr)
{
	preempt_disable();
	while (unlikely(atomic_test_and_set_bit(bitnum, addr))) {
		while (test_bit(bitnum, addr)) {
			preempt_enable();
			cpu_relax();
			preempt_disable();
		}
	}
}

int smp_bit_trylock(int bitnum, unsigned long *addr)
{
	preempt_disable();
	if (unlikely(atomic_test_and_set_bit(bitnum, addr))) {
		preempt_enable();

		return 0;
	}

	return 1;
}

void smp_bit_unlock(int bitnum, unsigned long *addr)
{
	smp_mb();
	atomic_clear_bit(bitnum, addr);
	preempt_enable();
}