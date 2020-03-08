#ifndef __ASM_BITOPS_H
#define __ASM_BITOPS_H

#ifndef __DIM_SUM_BITOPS_H
#error only <dim-sum/bitops.h> can be included directly
#endif

#include <linux/compiler.h>
#include <asm/barrier.h>

/**
 * 原子位操作
 */
extern void atomic_set_bit(int nr, volatile unsigned long *p);
extern void atomic_clear_bit(int nr, volatile unsigned long *p);
extern void atomic_change_bit(int nr, volatile unsigned long *p);
/**
 * 比较并设置位
 * 如果旧值已经置位，则返回true
 */
extern int atomic_test_and_set_bit(int nr, volatile unsigned long *p);
extern int atomic_test_and_clear_bit(int nr, volatile unsigned long *p);
extern int atomic_test_and_change_bit(int nr, volatile unsigned long *p);

#include <asm-generic/bitops/builtin-__ffs.h>
#include <asm-generic/bitops/builtin-ffs.h>
#include <asm-generic/bitops/builtin-__fls.h>
#include <asm-generic/bitops/builtin-fls.h>

#include <asm-generic/bitops/ffz.h>
#include <asm-generic/bitops/fls64.h>
#include <asm-generic/bitops/find.h>

#include <asm-generic/bitops/sched.h>
#include <asm-generic/bitops/hweight.h>
#include <asm-generic/bitops/lock.h>

#include <asm-generic/bitops/non-atomic.h>
#include <asm-generic/bitops/le.h>

#endif /* __ASM_BITOPS_H */
