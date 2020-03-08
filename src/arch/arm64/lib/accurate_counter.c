#include <dim-sum/accurate_counter.h>
#include <dim-sum/bug.h>

#include <asm/barrier.h>

long arch_accurate_add(long i, struct accurate_counter *v)
{
	unsigned long tmp;
	long result;

	asm volatile("// arch_accurate_add\n"
		"1:	ldxr	%0, %2\n"
		"	add	%0, %0, %3\n"
		"	stlxr	%w1, %0, %2\n"
		"	cbnz	%w1, 1b"
			: "=&r" (result), "=&r" (tmp), "+Q" (v->counter)
			: "Ir" (i)
			: "memory");

	smp_mb();
	return result;
}

long arch_accurate_sub(long i, struct accurate_counter *v)
{
	unsigned long tmp;
	long result;

	asm volatile("// arch_accurate_add\n"
		"1:	ldxr	%0, %2\n"
		"	sub	%0, %0, %3\n"
		"	stlxr	%w1, %0, %2\n"
		"	cbnz	%w1, 1b"
			: "=&r" (result), "=&r" (tmp), "+Q" (v->counter)
			: "Ir" (i)
			: "memory");

	smp_mb();
	return result;
}

unsigned long arch_accurate_xchg(unsigned long x, volatile void *ptr, int size)
{
	unsigned long ret, tmp;

	switch (size) {
	case 1:
		asm volatile("//	__xchg1\n"
		"1:	ldxrb	%w0, %2\n"
		"	stlxrb	%w1, %w3, %2\n"
		"	cbnz	%w1, 1b\n"
			: "=&r" (ret), "=&r" (tmp), "+Q" (*(u8 *)ptr)
			: "r" (x)
			: "memory");
		break;
	case 2:
		asm volatile("//	__xchg2\n"
		"1:	ldxrh	%w0, %2\n"
		"	stlxrh	%w1, %w3, %2\n"
		"	cbnz	%w1, 1b\n"
			: "=&r" (ret), "=&r" (tmp), "+Q" (*(u16 *)ptr)
			: "r" (x)
			: "memory");
		break;
	case 4:
		asm volatile("//	__xchg4\n"
		"1:	ldxr	%w0, %2\n"
		"	stlxr	%w1, %w3, %2\n"
		"	cbnz	%w1, 1b\n"
			: "=&r" (ret), "=&r" (tmp), "+Q" (*(u32 *)ptr)
			: "r" (x)
			: "memory");
		break;
	case 8:
		asm volatile("//	__xchg8\n"
		"1:	ldxr	%0, %2\n"
		"	stlxr	%w1, %3, %2\n"
		"	cbnz	%w1, 1b\n"
			: "=&r" (ret), "=&r" (tmp), "+Q" (*(u64 *)ptr)
			: "r" (x)
			: "memory");
		break;
	default:
		BUG();
	}

	smp_mb();
	return ret;
}

long arch_accurate_cmpxchg(struct accurate_counter *ptr, long old, long new)
{
	long oldval;
	unsigned long res;

	smp_mb();

	asm volatile("// arch_accurate_cmpxchg\n"
		"1:	ldxr	%1, %2\n"
		"	cmp	%1, %3\n"
		"	b.ne	2f\n"
		"	stxr	%w0, %4, %2\n"
		"	cbnz	%w0, 1b\n"
		"2:"
			: "=&r" (res), "=&r" (oldval), "+Q" (ptr->counter)
			: "Ir" (old), "r" (new)
			: "cc");

	smp_mb();
	return oldval;
}
