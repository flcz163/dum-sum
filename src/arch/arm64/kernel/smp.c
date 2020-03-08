#include <dim-sum/cache.h>
#include <dim-sum/cpu.h>
#include <dim-sum/cpumask.h>
#include <dim-sum/delay.h>
#include <dim-sum/errno.h>
#include <dim-sum/idle.h>
#include <dim-sum/init.h>
#include <dim-sum/percpu.h>
#include <dim-sum/psci.h>
#include <dim-sum/sched.h>
#include <dim-sum/smp.h>

#include <asm/asm-offsets.h>
#include <asm/cacheflush.h>
#include <asm/mmu_context.h>
#include <asm/tlbflush.h>

void (*__smp_raise_ipi)(const struct cpumask *, unsigned int);

void __init smp_set_raise_ipi_call(void (*fn)(const struct cpumask *, unsigned int))
{
	__smp_raise_ipi = fn;
}

void do_IPI(int ipinr, struct exception_spot *regs)
{
	printk("xby_debug in do_IPI, irq is %d, cpu is %d.\n", ipinr, smp_processor_id());
}
