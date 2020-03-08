#ifndef __ASM_CACHETYPE_H
#define __ASM_CACHETYPE_H

#include <asm/cputype.h>

/**
 * 缓存行位宽掩码
 */
#define CTR_CWG_SHIFT		24
#define CTR_CWG_MASK		15

#ifndef __ASSEMBLY__

#include <dim-sum/bitops.h>

/**
 * CPU缓存位宽
 */
static inline u32 cache_type_cwg(void)
{
	return (read_cpuid_cachetype() >> CTR_CWG_SHIFT) & CTR_CWG_MASK;
}

#endif	/* __ASSEMBLY__ */

#endif	/* __ASM_CACHETYPE_H */
