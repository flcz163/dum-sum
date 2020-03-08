#ifndef __ASM_TLBFLUSH_H
#define __ASM_TLBFLUSH_H

#ifndef __ASSEMBLY__

#include <dim-sum/sched.h>
#include <asm/cputype.h>

static inline void flush_tlb_all(void)
{
	dsb(ishst);
	asm("tlbi	vmalle1is");
	dsb(ish);
	isb();
}

static inline void __flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	unsigned long addr;
	start >>= 12;
	end >>= 12;

	dsb(ishst);
	for (addr = start; addr < end; addr += 1 << (PAGE_SHIFT - 12))
		asm("tlbi vaae1is, %0" : : "r"(addr));
	dsb(ish);
	isb();
}

#define MAX_TLB_RANGE	(1024UL << PAGE_SHIFT)

static inline void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	if ((end - start) <= MAX_TLB_RANGE)
		__flush_tlb_kernel_range(start, end);
	else
		flush_tlb_all();
}

#endif /* __ASSEMBLY__ */

#endif /* __ASM_TLBFLUSH_H */
