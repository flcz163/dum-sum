#ifndef __ASM_ACCURATE_COUNTER_H
#define __ASM_ACCURATE_COUNTER_H

extern long arch_accurate_add(long i, struct accurate_counter *v);
long arch_accurate_sub(long i, struct accurate_counter *v);
long arch_accurate_cmpxchg(struct accurate_counter *ptr,
						long old, long new);
unsigned long arch_accurate_xchg(unsigned long x, volatile void *ptr, int size);
#endif /* __ASM_ACCURATE_COUNTER_H */
