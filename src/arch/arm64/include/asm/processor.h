#ifndef __ASM_PROCESSOR_H
#define __ASM_PROCESSOR_H

#ifndef __ASSEMBLY__

#include <asm/types.h>

struct cpu_context {
	unsigned long x19;
	unsigned long x20;
	unsigned long x21;
	unsigned long x22;
	unsigned long x23;
	unsigned long x24;
	unsigned long x25;
	unsigned long x26;
	unsigned long x27;
	unsigned long x28;
	unsigned long fp;
	unsigned long sp;
	unsigned long pc;
};

struct task_spot {
	struct cpu_context cpu_context;
};

static inline void cpu_relax(void)
{
	asm volatile("yield" ::: "memory");
}


/**
 * ????
 */
#define ARCH_HAS_PREFETCH
static inline void prefetch(const void *ptr)
{
	asm volatile("prfm pldl1keep, %a0\n" : : "p" (ptr));
}

#define ARCH_HAS_PREFETCHW
static inline void prefetchw(const void *ptr)
{
	asm volatile("prfm pstl1keep, %a0\n" : : "p" (ptr));
}

#define ARCH_HAS_SPINLOCK_PREFETCH
static inline void spin_lock_prefetch(const void *x)
{
	prefetchw(x);
}

extern void cpu_do_idle(void);
void init_arch_cpu(void);

#endif /* __ASSEMBLY__ */

#endif /* __ASM_PROCESSOR_H */
