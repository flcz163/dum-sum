#ifndef __DIM_SUM_CACHE_H
#define __DIM_SUM_CACHE_H

#include <dim-sum/kernel.h>
#include <asm/cache.h>

#ifndef L1_CACHE_ALIGN
#define L1_CACHE_ALIGN(x) ALIGN(x, L1_CACHE_BYTES)
#endif

#ifndef SMP_CACHE_BYTES
#define SMP_CACHE_BYTES L1_CACHE_BYTES
#endif

#ifndef __read_mostly
#define __read_mostly
#endif

#ifndef aligned_cacheline
#define aligned_cacheline __attribute__((__aligned__(SMP_CACHE_BYTES)))
#endif

#ifndef aligned_cacheline_in_smp
#define aligned_cacheline_in_smp aligned_cacheline
#endif

#ifndef CONFIG_ARCH_HAS_CACHE_LINE_SIZE
#define cache_line_size()	L1_CACHE_BYTES
#endif

#endif /* __DIM_SUM_CACHE_H */
