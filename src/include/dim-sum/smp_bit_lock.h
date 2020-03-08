#ifndef __DIM_SUM_SMP_BIT_LOCK_H
#define __DIM_SUM_SMP_BIT_LOCK_H

#include <dim-sum/bitops.h>

extern void smp_bit_lock(int bitnum, unsigned long *addr);

extern int smp_bit_trylock(int bitnum, unsigned long *addr);

extern void smp_bit_unlock(int bitnum, unsigned long *addr);

static inline int smp_bit_is_locked(int bitnum, unsigned long *addr)
{
	return test_bit(bitnum, addr);
}

#endif /* __DIM_SUM_SMP_BIT_LOCK_H */
