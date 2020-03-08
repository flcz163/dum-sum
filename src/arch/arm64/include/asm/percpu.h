#ifndef __ASM_PERCPU_H_
#define __ASM_PERCPU_H_

register unsigned long current_stack_pointer asm ("sp");

static inline void set_this_cpu_offset(unsigned long off)
{
	asm volatile("msr tpidr_el1, %0" :: "r" (off) : "memory");
}

static inline unsigned long __this_cpu_offset(void)
{
	unsigned long off;

	asm("mrs %0, tpidr_el1" : "=r" (off) :
		"Q" (*(const unsigned long *)current_stack_pointer));

	return off;
}

/**
 * 客官，看清楚了
 * this_cpu_offset后面没有加()
 */
#define this_cpu_offset __this_cpu_offset()
#include <asm-generic/percpu.h>

#endif /* __ASM_PERCPU_H_ */
