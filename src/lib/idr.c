#include <dim-sum/beehive.h>
#include <dim-sum/errno.h>
#include <dim-sum/idr.h>
#include <dim-sum/string.h>

void *idr_find(struct idr *idp, int id)
{
	return NULL;
}

int idr_pre_get(struct idr *idp, unsigned paf_mask)
{
	return 1;
}

int idr_get_new(struct idr *idp, void *ptr, int *id)
{
	int ret = idp->next_idr;

	if (idp->next_idr <= idp->max_idr) {
		idp->next_idr++;
		
		return ret;
	} else {
		return -ENOSPC;
	}
}

int idr_get_new_above(struct idr *idp, void *ptr, int starting_id, int *id)
{
	return 0;
}

void idr_remove(struct idr *idp, int id)
{
}

void idr_init(struct idr *idp, int max)
{
	idp->next_idr = 0;
	idp->max_idr = max;
	smp_lock_init(&idp->lock);
}

static int expand_space(struct idr_space *s)
{
	u32 length;
	void *bits;

	length = max(32U, s->bitmap_len << 1);

	bits = kmalloc(length, PAF_KERNEL | __PAF_ZERO);
	if (!bits)
		return -ENOMEM;

	if (s->bits) {
		memcpy(bits, s->bits, s->bitmap_len);
		kfree(s->bits);
	}
		
	s->unused_bits = (length - s->bitmap_len) << 3;
	s->bits = bits;
	s->bitmap_len = length;

	return 0;
}

int get_idr_number(struct idr_space *s)
{
	int rval = 0;

	mutex_lock(&s->mutex);
	if (s->unused_bits < 1)
		rval = expand_space(s);
	if (!rval) {
		rval = find_first_zero_bit(s->bits, s->bitmap_len << 3);
		s->unused_bits--;
		__set_bit(rval, s->bits);
	}
	mutex_unlock(&s->mutex);

	return rval;
}

void put_idr_number(struct idr_space *s, int number)
{
	int old_val;

	if (number >= 0) {
		mutex_lock(&s->mutex);
		old_val = __test_and_clear_bit(number, s->bits);
		if (old_val)
			s->unused_bits--;
		mutex_unlock(&s->mutex);
	}
}