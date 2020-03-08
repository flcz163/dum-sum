#ifndef __ASM_SMP_LOCK_H
#define __ASM_SMP_LOCK_H

#include <linux/compiler.h>

#define TICKET_SHIFT	16

struct arch_smp_lock {
#ifdef __AARCH64EB__
	u16 next;
	u16 owner;
#else
	u16 owner;
	u16 next;
#endif
} __attribute__((aligned(4)));

#define __ARCH_SMP_LOCK_UNLOCKED {0, 0}

static inline int arch_smp_lock_value_unlocked(struct arch_smp_lock lock)
{
	return lock.owner == lock.next;
}

static inline int arch_smp_lock_is_locked(struct arch_smp_lock *lock)
{
	return !arch_smp_lock_value_unlocked(READ_ONCE(*lock));
}

static inline void arch_smp_lock(struct arch_smp_lock *lock)
{
	unsigned int tmp;
	struct arch_smp_lock lockval, newval;

	asm volatile(
	/* Atomically increment the next ticket. */
"	prfm	pstl1strm, %3\n"
"1:	ldaxr	%w0, %3\n"
"	add	%w1, %w0, %w5\n"
"	stxr	%w2, %w1, %3\n"
"	cbnz	%w2, 1b\n"
	/* Did we get the lock? */
"	eor	%w1, %w0, %w0, ror #16\n"
"	cbz	%w1, 3f\n"
	/*
	 * No: spin on the owner. Send a local event to avoid missing an
	 * unlock before the exclusive load.
	 */
"	sevl\n"
"2:	wfe\n"
"	ldaxrh	%w2, %4\n"
"	eor	%w1, %w2, %w0, lsr #16\n"
"	cbnz	%w1, 2b\n"
	/* We got the lock. Critical section starts here. */
"3:"
	: "=&r" (lockval), "=&r" (newval), "=&r" (tmp), "+Q" (*lock)
	: "Q" (lock->owner), "I" (1 << TICKET_SHIFT)
	: "memory");
}

static inline int arch_smp_trylock(struct arch_smp_lock *lock)
{
	unsigned int tmp;
	struct arch_smp_lock lockval;

	asm volatile(
"	prfm	pstl1strm, %2\n"
"1:	ldaxr	%w0, %2\n"
"	eor	%w1, %w0, %w0, ror #16\n"
"	cbnz	%w1, 2f\n"
"	add	%w0, %w0, %3\n"
"	stxr	%w1, %w0, %2\n"
"	cbnz	%w1, 1b\n"
"2:"
	: "=&r" (lockval), "=&r" (tmp), "+Q" (*lock)
	: "I" (1 << TICKET_SHIFT)
	: "memory");

	return !tmp;
}

static inline void arch_smp_unlock(struct arch_smp_lock *lock)
{
	asm volatile(
"	stlrh	%w1, %0\n"
	: "=Q" (lock->owner)
	: "r" (lock->owner + 1)
	: "memory");
}

#endif /* __ASM_SMP_LOCK_H */
