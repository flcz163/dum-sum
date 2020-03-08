#ifndef __DIM_SUM_APPROXIMATE_COUNTER_H
#define __DIM_SUM_APPROXIMATE_COUNTER_H

#include <dim-sum/smp_lock.h>
#include <dim-sum/percpu.h>

struct approximate_counter {
	struct smp_lock lock;
	long count;
	long *counters;
};

#if MAX_CPUS >= 16
#define FBC_BATCH	(MAX_CPUS*2)
#else
#define FBC_BATCH	(MAX_CPUS*4)
#endif

static inline void approximate_counter_init(struct approximate_counter *fbc)
{
	smp_lock_init(&fbc->lock);
	fbc->count = 0;
	fbc->counters = alloc_percpu(long);
}

static inline void approximate_counter_destroy(struct approximate_counter *fbc)
{
	free_percpu(fbc->counters);
}

void approximate_counter_mod(struct approximate_counter *fbc, long amount);

static inline long approximate_counter_read(struct approximate_counter *fbc)
{
	return fbc->count;
}

static inline long approximate_counter_read_positive(struct approximate_counter *fbc)
{
	long ret = fbc->count;

	barrier();		/* Prevent reloads of fbc->count */
	if (ret > 0)
		return ret;
	return 1;
}

static inline void approximate_counter_inc(struct approximate_counter *fbc)
{
	approximate_counter_mod(fbc, 1);
}

static inline void approximate_counter_dec(struct approximate_counter *fbc)
{
	approximate_counter_mod(fbc, -1);
}

#endif /* __DIM_SUM_APPROXIMATE_COUNTER_H */
