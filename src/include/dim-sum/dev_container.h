#ifndef _DIM_SUM_KOBJECT_H_
#define _DIM_SUM_KOBJECT_H_

#include <dim-sum/types.h>
#include <dim-sum/double_list.h>
#include <dim-sum/ref.h>
#include <dim-sum/rwsem.h>

struct device;

struct device_container {
	struct rw_semaphore sem;
	struct hash_list_bucket *__hash;
	unsigned int __hash_order;
	unsigned int __hash_mask;
};

extern struct device * hold_device(struct device *);
extern void loosen_device(struct device *);

int putin_device_container(struct device_container *domain,
	struct device *device, void *owner);
void takeout_device_container(struct device_container *, struct device *);
struct device *device_container_lookup(struct device_container *domain, devno_t dev, int *index);
void device_container_init(struct device_container *container);

static inline int register_device(struct device *obj)
{
	return 0;
}

static inline void unregister_device(struct device *obj)
{
}

extern unsigned int alloc_dev_major(void);
extern void free_dev_major(unsigned int type);

#endif /* _DIM_SUM_KOBJECT_H_ */
