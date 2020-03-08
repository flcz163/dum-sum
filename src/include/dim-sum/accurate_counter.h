#ifndef __DIM_SUM_ACCURATE_COUNTER_H
#define __DIM_SUM_ACCURATE_COUNTER_H

#include <linux/compiler.h>

struct accurate_counter {
	long counter;
};

#include <asm/accurate_counter.h>

#define ACCURATE_COUNTER_INIT(i)	{ (i) }

static inline long accurate_read(struct accurate_counter *l)
{
	return ACCESS_ONCE(l->counter);
}

static inline void accurate_set(struct accurate_counter *l, long i)
{
	l->counter = i;
}

static inline long accurate_inc(struct accurate_counter *l)
{
	return arch_accurate_add(1UL, l);
}

static inline long accurate_dec(struct accurate_counter *l)
{
	return arch_accurate_sub(1UL, l);
}

static inline long accurate_add(long i, struct accurate_counter *l)
{
	return arch_accurate_add(i, l);
}

static inline long accurate_sub(long i, struct accurate_counter *l)
{
	return arch_accurate_sub(i, l);
}

static inline long accurate_sub_and_test_zero(long i, struct accurate_counter *l)
{
	return arch_accurate_sub(i, l) == 0;
}

static inline long accurate_dec_and_test_zero(struct accurate_counter *l)
{
	return arch_accurate_sub(1UL, l) == 0;
}

static inline long accurate_inc_and_test_zero(struct accurate_counter *l)
{
	return arch_accurate_add(1UL, l) == 0;
}

static inline long accurate_add_test_negative(long i, struct accurate_counter *l)
{
	return arch_accurate_add(i, l) < 0;
}

static inline long accurate_add_ifneq(struct accurate_counter *l, long a, long u)
{
	long c, old;

	c = accurate_read(l);
	while (c != u && (old = arch_accurate_cmpxchg(l, c, c + a)) != c)
		c = old;

	return c != u;
}

#define accurate_inc_not_zero(l) 	accurate_add_ifneq((l), 1LL, 0LL)

#define accurate_cmpxchg(l, old, new) \
	(arch_accurate_cmpxchg((struct accurate_counter *)(l), (old), (new)))

#define xchg_ptr(ptr,x) \
({ \
	__typeof__(*(ptr)) __ret; \
	__ret = (__typeof__(*(ptr))) \
		arch_accurate_xchg((unsigned long)(x), (ptr), sizeof(*(ptr))); \
	__ret; \
})

#define accurate_xchg(v, new) (xchg_ptr(&((v)->counter), new))

#endif /* __DIM_SUM_ACCURATE_COUNTER_H */
