#include <dim-sum/approximate_counter.h>

void approximate_counter_mod(struct approximate_counter *fbc, long amount)
{
	long count;
	long *pcount;

	pcount = hold_percpu_ptr(fbc->counters);
	count = *pcount + amount;
	if (count >= FBC_BATCH || count <= -FBC_BATCH) {
		smp_lock(&fbc->lock);
		fbc->count += count;
		smp_unlock(&fbc->lock);
		count = 0;
	}
	*pcount = count;
	loosen_percpu_ptr(fbc->counters);
}
