#ifndef ASMARM_AMBA_H
#define ASMARM_AMBA_H

#include <dim-sum/device.h>
#include <dim-sum/ioport.h>
#include <linux/mod_devicetable.h>

#define AMBA_NR_IRQS	9
#define AMBA_CID	0xb105f00d
#define CORESIGHT_CID	0xb105900d

struct amba_device {
	struct device		dev;
	struct resource		res;
	unsigned int		periphid;
	unsigned int		irq[AMBA_NR_IRQS];
	char			*driver_override;
};

struct amba_driver {
	struct device_driver	drv;
	int			(*probe)(struct amba_device *, const struct amba_id *);
	void                    (*shutdown)(struct amba_device *);
	int			(*remove)(struct amba_device *);
	const struct amba_id	*id_table;
};

#define to_amba_device(d)	container_of(d, struct amba_device, dev)

struct amba_device *amba_device_alloc(const char *, resource_size_t, size_t);
void amba_device_put(struct amba_device *);

#define AMBA_REV_BITS(a) (((a) >> 20) & 0x0f)
#define amba_rev(d)	AMBA_REV_BITS((d)->periphid)

#define amba_get_drvdata(d)	dev_get_drvdata(&d->dev)
#define amba_set_drvdata(d,p)	dev_set_drvdata(&d->dev, p)

int amba_device_add(struct amba_device *dev, struct resource *parent);
int amba_driver_register(struct amba_driver *);
void amba_driver_unregister(struct amba_driver *);
#endif
