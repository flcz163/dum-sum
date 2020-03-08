#include <dim-sum/amba/bus.h>
#include <dim-sum/device.h>
#include <dim-sum/tty.h>

#include <clocksource/arm_arch_timer.h>

static int __init add_amba_device(void)
{
	struct amba_device *dev;
	int i;
	int ret;
	
	dev = amba_device_alloc(NULL, 0, 0);

	if (!dev) {
		goto err_clear_flag;
	}

	/* setup generic device info */
	//dev->dev.of_node = of_node_get(node);
	//dev->dev.parent = parent ? : &platform_bus;
	//dev->dev.platform_data = platform_data;
	dev_set_name(&dev->dev, "%s", "pl011");
	//of_dma_configure(&dev->dev, dev->dev.of_node);

	/* Allow the HW Peripheral ID to be overridden */
	dev->periphid = 0;

	/* Decode the IRQs and address ranges */
	for (i = 0; i < AMBA_NR_IRQS; i++)
		dev->irq[i] = i == 0 ? 38 : 0;

	dev->res.start = 0x9000000;
	dev->res.end = 0x9000fff;
	ret = amba_device_add(dev, &iomem_resource);
	if (ret) {
		goto err_free;
	}

	pl011_init();

	return 0;

err_free:
	amba_device_put(dev);
err_clear_flag:
	//of_node_clear_flag(node, OF_POPULATED);
	return ret;
}

int __init init_quirk_device(void)
{
	add_amba_device();

	return 0;
}

void probe_devices(void)
{
	init_timer_arch();
	virtio_init();
	virtio_mmio_init();
	virtio_blk_init();
	virtio_net_driver_init();

	/**
	 * 有些奇怪的设备，放在这里处理
	 */
	init_quirk_device();
}
