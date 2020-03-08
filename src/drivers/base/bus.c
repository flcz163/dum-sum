#include <dim-sum/bus.h>
#include <dim-sum/device.h>
#include <dim-sum/errno.h>
#include <dim-sum/double_list.h>
#include <dim-sum/init.h>
#include <dim-sum/rwsem.h>

#define to_dev(node) container_of(node, struct device, bus_list)
#define to_drv(node) container_of(node, struct device_driver, bus_list)

void device_bind_driver(struct device * dev)
{
	list_insert_behind(&dev->driver_list, &dev->driver->devices);
}

int driver_probe_device(struct device_driver * drv, struct device * dev)
{
	int ret;
	if (drv->bus->match && !drv->bus->match(dev, drv))
		return -ENODEV;

	dev->driver = drv;
	if (dev->bus->probe) {
		ret = dev->bus->probe(dev);
		if (ret)
			goto probe_failed;
	} else if (drv->probe) {
		ret = drv->probe(dev);
		if (ret) {
			dev->driver = NULL;
			goto probe_failed;
		}
	}

	device_bind_driver(dev);

	ret = 0;
probe_failed:
	return ret;
}

/**
 *	device_attach - try to attach device to a driver.
 *	@dev:	device.
 *
 *	Walk the list of drivers that the bus has and call
 *	driver_probe_device() for each pair. If a compatible
 *	pair is found, break out and return.
 */
int device_attach(struct device * dev)
{
 	struct bus_type * bus = dev->bus;
	struct double_list * entry;
	int error;

	if (dev->driver) {
		device_bind_driver(dev);
		return 1;
	}

	if (bus->match) {
		list_for_each(entry, &bus->drivers) {
			struct device_driver * drv = to_drv(entry);
			error = driver_probe_device(drv, dev);
			if (!error)
				/* success, driver matched */
				return 1;
		}
	}

	return 0;
}

int bus_add_device(struct device * dev)
{
	struct bus_type * bus = dev->bus;
	int error = 0;

	if (bus) {
		down_write(&dev->bus->rwsem);
		list_insert_behind(&dev->bus_list, &dev->bus->devices);
		device_attach(dev);
		up_write(&dev->bus->rwsem);
	}
	return error;
}

void driver_attach(struct device_driver * drv)
{
	struct bus_type * bus = drv->bus;
	struct double_list * entry;
	//int error;

	if (!bus->match)
		return;

	list_for_each(entry, &bus->devices) {
		struct device * dev = container_of(entry, struct device, bus_list);
		if (!dev->driver) {
			driver_probe_device(drv, dev);
		}
	}
}

/**
 *	bus_add_driver - Add a driver to the bus.
 *	@drv:	driver.
 *
 */
int bus_add_driver(struct device_driver * drv)
{
	struct bus_type * bus = drv->bus;
	int error = 0;

	if (bus) {
		down_write(&bus->rwsem);
		driver_attach(drv);
		up_write(&bus->rwsem);
	}
	return error;
}


/**
 * 注册总线。新注册的总线被删除到sysfs/bus中。
 */
int bus_register(struct bus_type * bus)
{
	init_rwsem(&bus->rwsem);
	list_init(&bus->devices);
	list_init(&bus->drivers);

	return 0;
}

/**
 * 从系统中删除一个总线。
 */
__maybe_unused void bus_unregister(struct bus_type * bus)
{
	/* TO-DO */
	return;
}

void __init init_bus(void)
{
	amba_init();
}
