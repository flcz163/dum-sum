#include <dim-sum/beehive.h>
#include <dim-sum/boot_allotter.h>
#include <dim-sum/device.h>
#include <dim-sum/errno.h>
#include <dim-sum/hash.h>
#include <dim-sum/hash_list.h>
#include <dim-sum/idr.h>
#include <dim-sum/dev_container.h>

static struct idr_space
dev_numspace = NUMSPACE_INITIALIZER(dev_numspace);

/**
 * 增加kobject对象的引用计数。
 */
struct device * hold_device(struct device * device)
{
	if (device)
		ref_count_hold(&device->ref);

	return device;
}

static void device_release(struct ref_count *kref)
{
	struct device *obj = container_of(kref, struct device, ref);

	if (obj->release)
		obj->release(obj);
}

/**
 * 减小kobject对象的引用计数。
 */
void loosen_device(struct device * kobj)
{
	if (kobj)
		ref_count_loosen(&kobj->ref, device_release);
}

int putin_device_container(struct device_container *domain,
	struct device *device, void *owner)
{
	unsigned long bucket;

	device->owner = owner;
	hash_list_init_node(&device->container);

	bucket = hash_long(device->dev_num, domain->__hash_order);
	down_write(&domain->sem);
	hlist_add_head(&device->container, domain->__hash + bucket);
	up_write(&domain->sem);

	return 0;
}

void takeout_device_container(struct device_container *domain, struct device *device)
{
	down_write(&domain->sem);
	hlist_del_init(&device->container);
	up_write(&domain->sem);
}

struct device *device_container_lookup(struct device_container *domain, devno_t dev, int *index)
{
	unsigned long bucket;
	struct hash_list_node *node;
	struct hash_list_bucket *head;

	bucket = hash_long(dev, domain->__hash_order);
	head = domain->__hash + bucket;

	down_read(&domain->sem);
	hlist_for_each(node, head) {
		struct device *device;

		device = hlist_entry(node, struct device, container);
		if (device->dev_num == dev) {
			*index = MINOR(device->dev_num);
			hold_device(device);

			up_read(&domain->sem); 
			return device;
		}
	}
	up_read(&domain->sem);
	return NULL;
}

void device_container_init(struct device_container *container)
{
	int __hash_order;
	int i;

	memset(container, 0, sizeof(struct device_container));	
	init_rwsem(&container->sem);

	container->__hash = alloc_boot_mem_stretch(sizeof(struct hash_list_bucket),
					10, &__hash_order);
	container->__hash_order = __hash_order;
	container->__hash_mask = (1 << __hash_order) - 1;

	for (i = 0; i < container->__hash_mask; i++)
		hash_list_init_bucket(&container->__hash[i]);
}

unsigned int alloc_dev_major(void)
{
	return get_idr_number(&dev_numspace);
}

void free_dev_major(unsigned int type)
{
	put_idr_number(&dev_numspace, type);
}
