#include <dim-sum/kernel.h>
#include <dim-sum/object.h>

struct object *hold_object(struct object *object)
{
	if (object)
		ref_count_hold(&object->ref);

	return object;
}

static void object_release(struct ref_count *kref)
{
	struct object *obj = container_of(kref, struct object, ref);

	if (obj->release)
		obj->release(obj);
}

void loosen_object(struct object *object)
{
	if (object)
		ref_count_loosen(&object->ref, object_release);
}