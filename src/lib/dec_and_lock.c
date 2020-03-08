//#include <dim-sum/module.h>
#include <dim-sum/smp_lock.h>
#include <dim-sum/accurate_counter.h>

/*
 * This is an implementation of the notion of "decrement a
 * reference count, and return locked if it decremented to zero".
 *
 * NOTE NOTE NOTE! This is _not_ equivalent to
 *
 *	if (accurate_dec_and_test_zero(&atomic)) {
 *		smp_lock(&lock);
 *		return 1;
 *	}
 *	return 0;
 *
 * because the spin-lock and the decrement must be
 * "atomic".
 */
int _atomic_dec_and_lock(struct accurate_counter *atomic, struct smp_lock *lock)
{
	/* Subtract 1 from counter unless that drops it to 0 (ie. it was 1) */
	if (accurate_add_ifneq(atomic, -1, 1))
		return 0;

	/* Otherwise do it the slow way */
	smp_lock(lock);
	if (accurate_dec_and_test_zero(atomic))
		return 1;
	smp_unlock(lock);
	return 0;
}

//EXPORT_SYMBOL(_atomic_dec_and_lock);
