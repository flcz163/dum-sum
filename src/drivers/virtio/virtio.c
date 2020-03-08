#include <dim-sum/virtio.h>
#include <dim-sum/smp_lock.h>
#include <dim-sum/virtio_config.h>
#include <dim-sum/idr.h>

/* Unique numbering for virtio devices. */
static DEFINE_IDR(virtio_index_ida, 0x8000000);

static inline int virtio_id_match(const struct virtio_device *dev,
				  const struct virtio_device_id *id)
{
	if (id->device != dev->id.device && id->device != VIRTIO_DEV_ANY_ID)
		return 0;

	return id->vendor == VIRTIO_DEV_ANY_ID || id->vendor == dev->id.vendor;
}

/* This looks through all the IDs a driver claims to support.  If any of them
 * match, we return 1 and the kernel will call virtio_dev_probe(). */
static int virtio_dev_match(struct device *_dv, struct device_driver *_dr)
{
	unsigned int i;
	struct virtio_device *dev = dev_to_virtio(_dv);
	const struct virtio_device_id *ids;

	ids = drv_to_virtio(_dr)->id_table;
	for (i = 0; ids[i].device; i++)
		if (virtio_id_match(dev, &ids[i])) {
			printk("xby_debug in virtio_dev_match, id->vendor:%d, id->device:%d.\n", ids[i].vendor, ids[i].device);
			printk("xby_debug in virtio_dev_match, dev->vendor:%d, dev->device:%d.\n", dev->id.vendor, dev->id.device);
			return 1;
		}
	return 0;
}

static void add_status(struct virtio_device *dev, unsigned status)
{
	dev->config->set_status(dev, dev->config->get_status(dev) | status);
}

void virtio_check_driver_offered_feature(const struct virtio_device *vdev,
					 unsigned int fbit)
{
	unsigned int i;
	struct virtio_driver *drv = drv_to_virtio(vdev->dev.driver);

	for (i = 0; i < drv->feature_table_size; i++)
		if (drv->feature_table[i] == fbit)
			return;
	//BUG();
}

static int virtio_dev_probe(struct device *_d)
{
	int err, i;
	struct virtio_device *dev = dev_to_virtio(_d);
	struct virtio_driver *drv = drv_to_virtio(dev->dev.driver);
	u32 device_features;

	/* We have a driver! */
	add_status(dev, VIRTIO_CONFIG_S_DRIVER);

	/* Figure out what features the device supports. */
	device_features = dev->config->get_features(dev);

	/* Features supported by both device and driver into dev->features. */
	memset(dev->features, 0, sizeof(dev->features));
	for (i = 0; i < drv->feature_table_size; i++) {
		unsigned int f = drv->feature_table[i];
		BUG_ON(f >= 32);
		if (device_features & (1 << f))
			atomic_set_bit(f, dev->features);
	}

	/* Transport features always preserved to pass to finalize_features. */
	for (i = VIRTIO_TRANSPORT_F_START; i < VIRTIO_TRANSPORT_F_END; i++)
		if (device_features & (1 << i))
			atomic_set_bit(i, dev->features);

	dev->config->finalize_features(dev);

	err = drv->probe(dev);
	if (err)
		add_status(dev, VIRTIO_CONFIG_S_FAILED);
	else {
		add_status(dev, VIRTIO_CONFIG_S_DRIVER_OK);
		if (drv->scan)
			drv->scan(dev);
	}

	return err;
}

static int virtio_dev_remove(struct device *_d)
{
	struct virtio_device *dev = dev_to_virtio(_d);
	struct virtio_driver *drv = drv_to_virtio(dev->dev.driver);

	drv->remove(dev);

	/* Driver should have reset device. */
	//WARN_ON_ONCE(dev->config->get_status(dev));

	/* Acknowledge the device's existence again. */
	add_status(dev, VIRTIO_CONFIG_S_ACKNOWLEDGE);
	return 0;
}

static struct bus_type virtio_bus = {
	.name  = "virtio",
	.match = virtio_dev_match,
	.probe = virtio_dev_probe,
	.remove = virtio_dev_remove,
};

int register_virtio_driver(struct virtio_driver *driver)
{
	/* Catch this early. */
	BUG_ON(driver->feature_table_size && !driver->feature_table);
	driver->driver.bus = &virtio_bus;
	return driver_register(&driver->driver);
}

void unregister_virtio_driver(struct virtio_driver *driver)
{
	driver_unregister(&driver->driver);
}

int register_virtio_device(struct virtio_device *dev)
{
	int err;
	int index;

	dev->dev.bus = &virtio_bus;

	/* Assign a unique device index and hence name. */
	err = idr_get_new(&virtio_index_ida, NULL, &index);
	if (err < 0)
		goto out;

	dev->index = index;
	dev_set_name(&dev->dev, "virtio%u", dev->index);

	/* We always start by resetting the device, in case a previous
	 * driver messed it up.  This also tests that code path a little. */
	dev->config->reset(dev);

	/* Acknowledge that we've seen the device. */
	add_status(dev, VIRTIO_CONFIG_S_ACKNOWLEDGE);

	list_init(&dev->vqs);

	/* device_register() causes the bus infrastructure to look for a
	 * matching driver. */
	err = device_register(&dev->dev);
out:
	if (err)
		add_status(dev, VIRTIO_CONFIG_S_FAILED);
	return err;
}

void unregister_virtio_device(struct virtio_device *dev)
{
	int index = dev->index; /* save for after device release */

	device_unregister(&dev->dev);
	idr_remove(&virtio_index_ida, index);
}

int virtio_init(void)
{
	if (bus_register(&virtio_bus) != 0)
		panic("virtio bus registration failed");
	return 0;
}

__maybe_unused void virtio_exit(void)
{
	bus_unregister(&virtio_bus);
}
