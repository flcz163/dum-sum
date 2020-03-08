#include <dim-sum/beehive.h>
#include <dim-sum/device.h>
#include <dim-sum/errno.h>
#include <dim-sum/string.h>

char *kvasprintf(paf_t paf, const char *fmt, va_list ap)
{
	unsigned int len;
	char *p;
	va_list aq;

	va_copy(aq, ap);
	len = vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);

	p = kmalloc(len+1, paf);
	if (!p)
		return NULL;

	vsnprintf(p, len+1, fmt, ap);

	return p;
}

int dev_set_name(struct device *dev, const char *fmt, ...)
{
	va_list vargs;
	char *s;
	const char *old_name = dev->init_name;

	va_start(vargs, fmt);

	if (!fmt)
		return 0;

	dev->init_name = kvasprintf(PAF_KERNEL, fmt, vargs);
	if (!dev->init_name)
		return -ENOMEM;

	/* ewww... some of these buggers have '/' in the name ... */
	while ((s = strchr(dev->init_name, '/')))
		s[0] = '!';

	kfree(old_name);
	
	va_end(vargs);
	return 0;
}

void *dev_get_drvdata(const struct device *dev)
{
	return dev->private;
}

int dev_set_drvdata(struct device *dev, void *data)
{
	dev->private = data;

	return 0;
}

/**
 * 往设备驱动程序模型中插入一个新的device_driver对象
 * 并自动在sysfs文件系统下为其创建一个新的目录。
 */
int driver_register(struct device_driver * drv)
{
	list_init(&drv->devices);
	sema_init(&drv->unload_sem, 0);
	return bus_add_driver(drv);
}

/**
 * 从设备驱动程序模型中移走一个device_driver对象
 */
void driver_unregister(struct device_driver * drv)
{
	/* TO-DO */
}
