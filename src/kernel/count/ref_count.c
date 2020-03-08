#include <dim-sum/ref.h>

void ref_count_init(struct ref_count *ref)
{
	accurate_set(&ref->count, 1);
}

void ref_count_hold(struct ref_count *ref)
{
	accurate_inc(&ref->count);
}

void ref_count_loosen(struct ref_count *ref,
	void (*loosen) (struct ref_count *ref))
{
	if (accurate_dec_and_test_zero(&ref->count))
		loosen(ref);
}