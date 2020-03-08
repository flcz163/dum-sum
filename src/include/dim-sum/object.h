#ifndef __DIM_SUM_OBJECT_H
#define __DIM_SUM_OBJECT_H

#include <dim-sum/ref.h>

struct object {
	struct ref_count ref;
	struct object *parent;
	void (*release)(struct object *);
};

static inline void init_object(struct object *object,
	struct object *parent, void (*release)(struct object *))
{
	ref_count_init(&object->ref);
	object->parent = parent;
	object->release = release;
}

static inline unsigned long get_object_count(struct object *object)
{
	return get_ref_count(&object->ref);
}

struct object *hold_object(struct object *object);
void loosen_object(struct object *object);

#endif /* __DIM_SUM_OBJECT_H */