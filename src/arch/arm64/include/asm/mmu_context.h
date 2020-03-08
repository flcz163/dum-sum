#ifndef __ASM_MMU_CONTEXT_H
#define __ASM_MMU_CONTEXT_H

#include <asm/memory.h>

extern void *empty_zero_page;

static inline void set_ttbr0(unsigned long ttbr)
{
	asm(
	"	msr	ttbr0_el1, %0			// set TTBR0\n"
	"	isb"
	:
	: "r" (ttbr));
}

extern u64 idmap_t0sz;

static inline bool __cpu_uses_extended_idmap(void)
{
	return (!IS_ENABLED(CONFIG_ARM64_VA_BITS_48) &&
		unlikely(idmap_t0sz != TCR_T0SZ(VA_BITS)));

	return false;
}

static inline void __cpu_set_tcr_t0sz(u64 t0sz)
{
	unsigned long tcr;

	if (__cpu_uses_extended_idmap())
		asm volatile (
		"	mrs	%0, tcr_el1	;"
		"	bfi	%0, %1, %2, %3	;"
		"	msr	tcr_el1, %0	;"
		"	isb"
		: "=&r" (tcr)
		: "r"(t0sz), "I"(TCR_T0SZ_OFFSET), "I"(TCR_TxSZ_WIDTH));
}

static inline void cpu_set_default_tcr_t0sz(void)
{
	__cpu_set_tcr_t0sz(TCR_T0SZ(VA_BITS));
}

#endif /* __ASM_MMU_CONTEXT_H */
