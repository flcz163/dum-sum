#ifndef _DIM_SUM_VIRTIO_H
#define _DIM_SUM_VIRTIO_H

#include <dim-sum/device.h>
#include <linux/mod_devicetable.h>
#include <dim-sum/virtio_ring.h>
#include <dim-sum/types.h>
#include <dim-sum/scatterlist.h>
#include <dim-sum/irq.h>

/* virtio_ring with information needed for host access. */
struct vringh {
	/* Guest publishes used event idx (note: we always do). */
	bool event_indices;

	/* Can we get away with weak barriers? */
	bool weak_barriers;

	/* Last available index we saw (ie. where we're up to). */
	u16 last_avail_idx;

	/* Last index we used. */
	u16 last_used_idx;

	/* How many descriptors we've completed since last need_notify(). */
	u32 completed;

	/* The vring (note: it may contain user pointers!) */
	struct vring vring;

	/* The function to call to notify the guest about added buffers */
	void (*notify)(struct vringh *);
};

struct virtio_device;
typedef void vrh_callback_t(struct virtio_device *, struct vringh *);
struct vringh_config_ops {
	int (*find_vrhs)(struct virtio_device *vdev, unsigned nhvrs,
			 struct vringh *vrhs[], vrh_callback_t *callbacks[]);
	void (*del_vrhs)(struct virtio_device *vdev);
};

/**
 * virtio_device - representation of a device using virtio
 * @index: unique position on the virtio bus
 * @dev: underlying device.
 * @id: the device type identification (used to match it with a driver).
 * @config: the configuration ops for this device.
 * @vringh_config: configuration ops for host vrings.
 * @vqs: the list of virtqueues for this device.
 * @features: the features supported by both driver and device.
 * @priv: private pointer for the driver's use.
 */
struct virtio_device {
	int index;
	unsigned long *base;
	struct device dev;
	struct virtio_device_id id;
	const struct virtio_config_ops *config;
	const struct vringh_config_ops *vringh_config;
	struct double_list vqs;
	/* Note that this is a Linux atomic_set_bit-style bitmap. */
	unsigned long features[1];
	void *priv;
};


/**
 * virtio_driver - operations for a virtio I/O driver
 * @driver: underlying device driver (populate name and owner).
 * @id_table: the ids serviced by this driver.
 * @feature_table: an array of feature numbers supported by this driver.
 * @feature_table_size: number of entries in the feature table array.
 * @probe: the function to call when a device is found.  Returns 0 or -errno.
 * @remove: the function to call when a device is removed.
 * @config_changed: optional function to call when the device configuration
 *    changes; may be called in interrupt context.
 */
struct virtio_driver {
	struct device_driver driver;
	const struct virtio_device_id *id_table;
	const unsigned int *feature_table;
	unsigned int feature_table_size;
	int (*probe)(struct virtio_device *dev);
	void (*scan)(struct virtio_device *dev);
	void (*remove)(struct virtio_device *dev);
	void (*config_changed)(struct virtio_device *dev);
};

struct virtqueue {
	struct double_list list;
	void (*callback)(struct virtqueue *vq);
	const char *name;
	struct virtio_device *vdev;
	unsigned int index;
	unsigned int num_free;
	void *priv;
};

static inline struct virtio_device *dev_to_virtio(struct device *_dev)
{
	return container_of(_dev, struct virtio_device, dev);
}

static inline struct virtio_driver *drv_to_virtio(struct device_driver *drv)
{
	return container_of(drv, struct virtio_driver, driver);
}

void virtqueue_kick(struct virtqueue *vq);

int virtqueue_add_sgs(struct virtqueue *vq,
		      struct scatterlist *sgs[],
		      unsigned int out_sgs,
		      unsigned int in_sgs,
		      void *data,
		      paf_t paf);

void virtqueue_disable_cb(struct virtqueue *vq);
void *virtqueue_get_buf(struct virtqueue *vq, unsigned int *len);
bool virtqueue_enable_cb(struct virtqueue *vq);

int register_virtio_driver(struct virtio_driver *drv);
void unregister_virtio_driver(struct virtio_driver *drv);

int virtqueue_add_inbuf(struct virtqueue *vq,
			struct scatterlist sg[], unsigned int num,
			void *data,
			paf_t paf);
void *virtqueue_detach_unused_buf(struct virtqueue *vq);
int virtqueue_add_outbuf(struct virtqueue *vq,
			 struct scatterlist sg[], unsigned int num,
			 void *data,
			 paf_t paf);
bool virtqueue_enable_cb_delayed(struct virtqueue *vq);
int register_virtio_device(struct virtio_device *dev);
void unregister_virtio_device(struct virtio_device *dev);
bool virtqueue_kick_prepare(struct virtqueue *vq);
void virtqueue_notify(struct virtqueue *vq);
unsigned virtqueue_enable_cb_prepare(struct virtqueue *vq);
bool virtqueue_poll(struct virtqueue *vq, unsigned);
enum isr_result vring_interrupt(int irq, void *_vq);
void vring_del_virtqueue(struct virtqueue *vq);
unsigned int virtqueue_get_vring_size(struct virtqueue *vq);
void virtio_exit(void);
#endif /* _DIM_SUM_VIRTIO_H */
