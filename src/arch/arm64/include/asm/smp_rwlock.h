#ifndef __ASM_SMP_RWLOCK_H
#define __ASM_SMP_RWLOCK_H

struct arch_smp_rwlock {
	volatile unsigned int lock;
} __attribute__((aligned(4)));

#define __ARCH_SMP_RWLOCK_UNLOCKED		{ 0 }

#define arch_smp_rwlock_can_read(x)			((x)->lock < 0x80000000)
#define arch_smp_rwlock_can_write(x)		((x)->lock == 0)

static inline int arch_smp_tryread(struct arch_smp_rwlock *rw)
{
	unsigned int tmp, tmp2 = 1;

	asm volatile(
	"	ldaxr	%w0, %2\n"
	"	add	%w0, %w0, #1\n"
	"	tbnz	%w0, #31, 1f\n"
	"	stxr	%w1, %w0, %2\n"
	"1:\n"
	: "=&r" (tmp), "+r" (tmp2), "+Q" (rw->lock)
	:
	: "memory");

	return !tmp2;
}

static inline int arch_smp_trywrite(struct arch_smp_rwlock *rw)
{
	unsigned int tmp;

	asm volatile(
		/* LL/SC */
		"1:	ldaxr	%w0, %1\n"
		"	cbnz	%w0, 2f\n"
		"	stxr	%w0, %w2, %1\n"
		"	cbnz	%w0, 1b\n"
		"2:"
		: "=&r" (tmp), "+Q" (rw->lock)
		: "r" (0x80000000)
		: "memory"
	);

	return !tmp;
}

static inline void arch_smp_write_lock(struct arch_smp_rwlock *rw)
{
	unsigned int tmp;

	asm volatile(
	"	sevl\n"
	"1:	wfe\n"
	"2:	ldaxr	%w0, %1\n"
	"	cbnz	%w0, 1b\n"
	"	stxr	%w0, %w2, %1\n"
	"	cbnz	%w0, 2b\n"
	: "=&r" (tmp), "+Q" (rw->lock)
	: "r" (0x80000000)
	: "memory");
}

static inline void arch_smp_read_lock(struct arch_smp_rwlock *rw)
{
	unsigned int tmp, tmp2;

	asm volatile(
	"	sevl\n"
	"1:	wfe\n"
	"2:	ldaxr	%w0, %2\n"
	"	add	%w0, %w0, #1\n"
	"	tbnz	%w0, #31, 1b\n"
	"	stxr	%w1, %w0, %2\n"
	"	cbnz	%w1, 2b\n"
	: "=&r" (tmp), "=&r" (tmp2), "+Q" (rw->lock)
	:
	: "memory");
}

static inline void arch_smp_read_unlock(struct arch_smp_rwlock *rw)
{
	unsigned int tmp, tmp2;

	asm volatile(
	"1:	ldxr	%w0, %2\n"
	"	sub	%w0, %w0, #1\n"
	"	stlxr	%w1, %w0, %2\n"
	"	cbnz	%w1, 1b\n"
	: "=&r" (tmp), "=&r" (tmp2), "+Q" (rw->lock)
	:
	: "memory");
}

static inline void arch_smp_write_unlock(struct arch_smp_rwlock *rw)
{
	asm volatile(
	"	stlr	%w1, %0\n"
	: "=Q" (rw->lock) : "r" (0) : "memory");
}

#endif /* __ASM_SMP_RWLOCK_H */
