#ifndef _DIM_SUM_DEVICE_H_
#define _DIM_SUM_DEVICE_H_

#include <dim-sum/types.h>
#include <dim-sum/hash_list.h>
#include <dim-sum/semaphore.h>
#include <dim-sum/rwsem.h>
#include <dim-sum/devno.h>
#include <dim-sum/ref.h>

struct device_driver;
struct device;

/**
 * 内核所支持的每一种总线类型都由一个bus_type描述。
 */
struct bus_type {
	/**
	 * 总线类型的名称。例如"pci"
	 */
	char			* name;
	struct rw_semaphore rwsem;
	struct double_list devices;
	struct double_list drivers;
	/**
	 * 检验给定的设备驱动程序是否支持特定设备的方法.
	 * 当一个总线上的新设备或者新驱动程序被添加时，会一次或多次调用这个函数。如果指定的驱动程序能够处理指定的设备，该函数返回非0值。
	 */
	int		(*match)(struct device * dev, struct device_driver * drv);
	int (*probe)(struct device *dev);
	int (*remove)(struct device *dev);
};

/**
 * 驱动程序描述符
 */
struct device_driver {
	/**
	 * 驱动程序的名称
	 */
	char			* name;
	/**
	 * 指向总线描述符的指针。
	 */
	struct bus_type		* bus;

	/**
	 * 禁止卸载设备驱动程序的信号量。
	 */
	struct semaphore	unload_sem;

	struct double_list bus_list;

	/**
	 * 驱动程序所支持的所有设备组成的链表的首部。
	 */
	struct double_list	devices;

	/**
	 * 探测设备的方法
	 */
	int	(*probe)	(struct device * dev);
	/**
	 * 移走设备的方法（检测设备驱动程序是否可以控制该设备）
	 */
	int 	(*remove)	(struct device * dev);
	/**
	 * 设备断电时调用的方法。
	 */
	void	(*shutdown)	(struct device * dev);
};

struct device {
	devno_t dev_num;
	/**
	 * 通过此字段，将设备放到容器中
	 */
	struct hash_list_node	container;
	void *owner;
	/**
	 * 设备的引用计数。
	 */
	struct ref_count ref;
	void (*release)(struct device *);
	struct device		*parent;
	const char		*init_name; /* initial name of the device */
	/**
	 * 指向控制设备驱动程序的指针
	 */
	struct device_driver *driver;	/* which driver has allocated this
					   device */
	/**
	 * 指向所连接总线的指针
	 * 标识该设备连接在何种类型的总线上。
	 */
	struct bus_type	* bus;		/* type of bus device is on */
	struct double_list	bus_list;
	struct double_list	driver_list;
	void		*platform_data;	/* Platform specific data, device
					   core doesn't touch it */
	void *private;
};

struct platform_device {
	const char	*name;
	int		id;
	bool		id_auto;
	struct device	dev;
	u32		num_resources;
	struct resource	*resource;

	const struct platform_device_id	*id_entry;
	int irq;
};

static inline int platform_get_irq(struct platform_device *dev, unsigned int num)
{
	return dev->irq;
}

extern void *dev_get_drvdata(const struct device *dev);
static inline void *platform_get_drvdata(const struct platform_device *pdev)
{
	return dev_get_drvdata(&pdev->dev);
}

extern int dev_set_drvdata(struct device *dev, void *data);
static inline void platform_set_drvdata(struct platform_device *pdev,
					void *data)
{
	dev_set_drvdata(&pdev->dev, data);
}

static inline const char *dev_name(const struct device *dev)
{
	return dev->init_name;
}

int dev_set_name(struct device *dev, const char *fmt, ...);

extern int bus_register(struct bus_type * bus);
extern void bus_unregister(struct bus_type * bus);
extern int driver_register(struct device_driver * drv);
extern void driver_unregister(struct device_driver * drv);
extern int device_register(struct device * dev);
extern void device_unregister(struct device * dev);

extern int  device_attach(struct device * dev);
extern void driver_attach(struct device_driver * drv);

struct platform_driver {
	int (*probe)(struct platform_device *);
	int (*remove)(struct platform_device *);
	struct device_driver driver;
	const struct platform_device_id *id_table;
};
int platform_driver_register(struct platform_driver *drv);

extern int bus_add_driver(struct device_driver *);

static inline void *dev_get_platdata(const struct device *dev)
{
	return dev->platform_data;
}

extern void device_initialize(struct device * dev);
extern void loosen_device(struct device * dev);
extern int device_add(struct device * dev);
extern struct device * hold_device(struct device * dev);
extern void device_bind_driver(struct device * dev);
extern int  driver_probe_device(struct device_driver * drv, struct device * dev);

void probe_devices(void);
#endif /* _DIM_SUM_DEVICE_H_ */
