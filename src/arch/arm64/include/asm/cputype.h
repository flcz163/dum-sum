#ifndef __ASM_CPUTYPE_H
#define __ASM_CPUTYPE_H

#ifndef __ASSEMBLY__
#define read_cpuid(reg) ({						\
	u64 __val;							\
	asm("mrs	%0, " #reg : "=r" (__val));			\
	__val;								\
})

static inline u32 __attribute_const__ read_cpuid_cachetype(void)
{
	return read_cpuid(CTR_EL0);
}
#endif

#endif
