#include <dim-sum/bus.h>
#include <dim-sum/device.h>
#include <dim-sum/errno.h>

void device_initialize(struct device *dev)
{
	list_init(&dev->driver_list);
	list_init(&dev->bus_list);
}

/**
 *	device_add - add device to device hierarchy.
 *	@dev:	device.
 *
 *	This is part 2 of device_register(), though may be called
 *	separately _iff_ device_initialize() has been called separately.
 *
 *	This adds it to the kobject hierarchy via kobject_add(), adds it
 *	to the global and sibling lists for the device, then
 *	adds it to the other relevant subsystems of the driver model.
 */
int device_add(struct device *dev)
{
	int error = -EINVAL;

	dev = hold_device(dev);
	if (!dev || !dev->bus)
		goto Error;

	if ((error = bus_add_device(dev)))
		goto BusError;

 Done:
	loosen_device(dev);
	return error;
 BusError:
 //PMError:
 Error:
	goto Done;
}

/**
 * 往设备驱动程序模型中插入一个新的设备驱动。
 * 并自动的在sysfs文件系统下为其创建一个新的目录
 */
int device_register(struct device *dev)
{
	device_initialize(dev);
	return device_add(dev);
}

/**
 * 从设备驱动程序模型中移走一个设备驱动对象。
 */
void device_unregister(struct device * dev)
{
	/* TO-DO */
	return;
}

int platform_driver_register(struct platform_driver *drv)
{
#if 0
	drv->driver.bus = &platform_bus_type;
	if (drv->probe)
		drv->driver.probe = platform_drv_probe;
	if (drv->remove)
		drv->driver.remove = platform_drv_remove;
	if (drv->shutdown)
		drv->driver.shutdown = platform_drv_shutdown;
#endif

	return driver_register(&drv->driver);
}
