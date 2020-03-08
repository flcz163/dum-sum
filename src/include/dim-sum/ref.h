#ifndef __DIM_SUM_REF_H
#define __DIM_SUM_REF_H

#include <dim-sum/accurate_counter.h>

struct ref_count {
	struct accurate_counter count;
};

static inline unsigned long get_ref_count(struct ref_count *ref)
{
	return accurate_read(&ref->count);
}

void ref_count_init(struct ref_count *ref);
void ref_count_hold(struct ref_count *ref);
void ref_count_loosen(struct ref_count *ref,
	void (*loosen) (struct ref_count *ref));

#endif /* __DIM_SUM_REF_H */
