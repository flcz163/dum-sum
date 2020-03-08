#ifndef __ASM_CPUFEATURE_H
#define __ASM_CPUFEATURE_H

#include <dim-sum/bitops.h>

#include <asm/hwcap.h>

#define ARM64_WORKAROUND_CLEAN_CACHE		0
#define ARM64_WORKAROUND_DEVICE_LOAD_ACQUIRE	1
#define ARM64_WORKAROUND_845719			2
#define ARM64_HAS_SYSREG_GIC_CPUIF		3
#define ARM64_HAS_PAN				4

#define ARM64_NCAPS				5

static inline bool cpu_have_feature(unsigned int num)
{
	return hw_capibility & (1UL << num);
}

extern DECLARE_BITMAP(cpu_hwcaps, ARM64_NCAPS);
static inline bool cpus_have_cap(unsigned int num)
{
	if (num >= ARM64_NCAPS)
		return false;
	return test_bit(num, cpu_hwcaps);
}

static inline void cpus_set_cap(unsigned int num)
{
	if (num >= ARM64_NCAPS)
		pr_warn("Attempt to set an illegal CPU capability (%d >= %d)\n",
			num, ARM64_NCAPS);
	else
		__set_bit(num, cpu_hwcaps);
}

#endif /* __ASM_CPUFEATURE_H */
