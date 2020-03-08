#include <dim-sum/amba/bus.h>
#include <dim-sum/beehive.h>
#include <dim-sum/device.h>
#include <dim-sum/errno.h>
#include <dim-sum/init.h>
#include <dim-sum/ioremap.h>
#include <dim-sum/virt_space.h>

#include <linux/mod_devicetable.h>

#include <asm/io.h>
#include <asm/memory.h>
#include <asm/page.h>

#define to_amba_driver(d)	container_of(d, struct amba_driver, drv)
struct resource iomem_resource = {
	.name	= "PCI mem",
	.start	= 0,
	.end	= -1,
	.flags	= IORESOURCE_MEM,
};

struct bus_type amba_bustype;

static void amba_device_initialize(struct amba_device *dev, const char *name)
{
	device_initialize(&dev->dev);
	if (name)
		dev_set_name(&dev->dev, "%s", name);
	//dev->dev.release = amba_device_release;
	dev->dev.bus = &amba_bustype;
	//dev->dev.dma_mask = &dev->dev.coherent_dma_mask;
	dev->res.name = dev_name(&dev->dev);
}

/**
 *	amba_device_alloc - allocate an AMBA device
 *	@name: sysfs name of the AMBA device
 *	@base: base of AMBA device
 *	@size: size of AMBA device
 *
 *	Allocate and initialize an AMBA device structure.  Returns %NULL
 *	on failure.
 */
struct amba_device *amba_device_alloc(const char *name, resource_size_t base,
	size_t size)
{
	struct amba_device *dev;

	dev = kzalloc(sizeof(*dev), PAF_KERNEL);
	if (dev) {
		amba_device_initialize(dev, name);
		dev->res.start = base;
		dev->res.end = base + size - 1;
		dev->res.flags = IORESOURCE_MEM;
	}

	return dev;
}


static const struct amba_id *
amba_lookup(const struct amba_id *table, struct amba_device *dev)
{
	int ret = 0;

	while (table->mask) {
		ret = (dev->periphid & table->mask) == table->id;
		if (ret)
			break;
		table++;
	}

	return ret ? table : NULL;
}

static int amba_match(struct device *dev, struct device_driver *drv)
{
	struct amba_device *pcdev = to_amba_device(dev);
	struct amba_driver *pcdrv = to_amba_driver(drv);

	/* When driver_override is set, only bind to the matching driver */
	if (pcdev->driver_override)
		return !strcmp(pcdev->driver_override, drv->name);

	return amba_lookup(pcdrv->id_table, pcdev) != NULL;
}

/**
 *	amba_device_put - put an AMBA device
 *	@dev: AMBA device to put
 */
void amba_device_put(struct amba_device *dev)
{
	loosen_device(&dev->dev);
}

/*
 * Primecells are part of the Advanced Microcontroller Bus Architecture,
 * so we call the bus "amba".
 */
struct bus_type amba_bustype = {
	.name		= "amba",
	.match		= amba_match,
	//.uevent		= amba_uevent,
	//.pm		= &amba_pm,
};

/*
 * These are the device model conversion veneers; they convert the
 * device model structures to our more specific structures.
 */
static int amba_probe(struct device *dev)
{
	struct amba_device *pcdev = to_amba_device(dev);
	struct amba_driver *pcdrv = to_amba_driver(dev->driver);
	const struct amba_id *id = amba_lookup(pcdrv->id_table, pcdev);
	int ret;

	do {
		ret = 0;//dev_pm_domain_attach(dev, true);
		if (ret == -EPROBE_DEFER)
			break;

		ret = 0;//amba_get_enable_pclk(pcdev);
		if (ret) {
			//dev_pm_domain_detach(dev, true);
			break;
		}

		//pm_runtime_get_noresume(dev);
		//pm_runtime_set_active(dev);
		//pm_runtime_enable(dev);

		ret = pcdrv->probe(pcdev, id);
		if (ret == 0)
			break;

		//pm_runtime_disable(dev);
		//pm_runtime_set_suspended(dev);
		//pm_runtime_put_noidle(dev);

		//amba_put_disable_pclk(pcdev);
		//dev_pm_domain_detach(dev, true);
	} while (0);

	return ret;
}

static int amba_remove(struct device *dev)
{
	struct amba_device *pcdev = to_amba_device(dev);
	struct amba_driver *drv = to_amba_driver(dev->driver);
	int ret;

	//pm_runtime_get_sync(dev);
	ret = drv->remove(pcdev);
	//pm_runtime_put_noidle(dev);

	/* Undo the runtime PM settings in amba_probe() */
	//pm_runtime_disable(dev);
	//pm_runtime_set_suspended(dev);
	//pm_runtime_put_noidle(dev);

	//amba_put_disable_pclk(pcdev);
	//dev_pm_domain_detach(dev, true);

	return ret;
}

static void amba_shutdown(struct device *dev)
{
	struct amba_driver *drv = to_amba_driver(dev->driver);
	drv->shutdown(to_amba_device(dev));
}

/**
 *	amba_driver_register - register an AMBA device driver
 *	@drv: amba device driver structure
 *
 *	Register an AMBA device driver with the Linux device model
 *	core.  If devices pre-exist, the drivers probe function will
 *	be called.
 */
int amba_driver_register(struct amba_driver *drv)
{
	drv->drv.bus = &amba_bustype;

	if (drv->probe)
		drv->drv.probe = amba_probe;
	if (drv->remove)
		drv->drv.remove = amba_remove;
	if (drv->shutdown)
		drv->drv.shutdown = amba_shutdown;

	return driver_register(&drv->drv);
}

/**
 *	amba_driver_unregister - remove an AMBA device driver
 *	@drv: AMBA device driver structure to remove
 *
 *	Unregister an AMBA device driver from the Linux device
 *	model.  The device model will call the drivers remove function
 *	for each device the device driver is currently handling.
 */
void amba_driver_unregister(struct amba_driver *drv)
{
	driver_unregister(&drv->drv);
}

static __maybe_unused void amba_device_release(struct device *dev)
{
	struct amba_device *d = to_amba_device(dev);

	//if (d->res.parent)
	//	release_resource(&d->res);
	kfree(d);
}

/**
 *	amba_device_add - add a previously allocated AMBA device structure
 *	@dev: AMBA device allocated by amba_device_alloc
 *	@parent: resource parent for this devices resources
 *
 *	Claim the resource, and read the device cell ID if not already
 *	initialized.  Register the AMBA device with the Linux device
 *	manager.
 */
int amba_device_add(struct amba_device *dev, struct resource *parent)
{
	u32 size;
	void __iomem *tmp;
	int i, ret;

	//WARN_ON(dev->irq[0] == (unsigned int)-1);
	//WARN_ON(dev->irq[1] == (unsigned int)-1);

	//ret = request_resource(parent, &dev->res);
	//if (ret)
	//	goto err_out;

	/* Hard-coded primecell ID instead of plug-n-play */
	if (dev->periphid != 0)
		goto skip_probe;

	/*
	 * Dynamically calculate the size of the resource
	 * and use this for iomap
	 */
	size = resource_size(&dev->res);
	tmp = ioremap(dev->res.start, size);
	if (!tmp) {
		ret = -ENOMEM;
		goto err_release;
	}

	ret = 0;//amba_get_enable_pclk(dev);
	if (ret == 0) {
		u32 pid, cid;

		/*
		 * Read pid and cid based on size of resource
		 * they are located at end of region
		 */
		for (pid = 0, i = 0; i < 4; i++)
			pid |= (readl(tmp + size - 0x20 + 4 * i) & 255) <<
				(i * 8);
		for (cid = 0, i = 0; i < 4; i++)
			cid |= (readl(tmp + size - 0x10 + 4 * i) & 255) <<
				(i * 8);

		//amba_put_disable_pclk(dev);

		if (cid == AMBA_CID || cid == CORESIGHT_CID)
			dev->periphid = pid;

		if (!dev->periphid)
			ret = -ENODEV;
	}

	iounmap(tmp);

	if (ret)
		goto err_release;

skip_probe:
	ret = device_add(&dev->dev);
	if (ret)
		goto err_release;

#if 0
	if (dev->irq[0])
		ret = device_create_file(&dev->dev, &dev_attr_irq0);
	if (ret == 0 && dev->irq[1])
		ret = device_create_file(&dev->dev, &dev_attr_irq1);
#endif
	if (ret == 0)
		return ret;

	device_unregister(&dev->dev);

err_release:
	//release_resource(&dev->res);
//err_out:
	return ret;
}

int __init amba_init(void)
{
	return bus_register(&amba_bustype);
}
