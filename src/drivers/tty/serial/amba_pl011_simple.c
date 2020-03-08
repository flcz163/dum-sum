#include <dim-sum/amba/bus.h>
#include <dim-sum/amba/serial.h>
#include <dim-sum/beehive.h>
#include <dim-sum/errno.h>
#include <dim-sum/init.h>
#include <dim-sum/irq.h>
#include <dim-sum/irq_mapping.h>
#include <dim-sum/serial.h>
#include <dim-sum/serial_core.h>
#include <dim-sum/simple_console.h>
#include <dim-sum/workqueue.h>
#include <linux/mod_devicetable.h>
#include <asm/memory.h>
#include <asm/io.h>

#include <dim-sum/linkage.h>
#include <dim-sum/types.h>

#include <dim-sum/cache.h>
#include <dim-sum/of.h>
#include <dim-sum/printk.h>
#include <dim-sum/delay.h>
#include <asm/early_map.h>
#include <asm/io.h>

static void pl011_simple_putc(struct simple_console_device *dev, int c)
{
	unsigned char __iomem	*membase = dev->priv;
	
	while (readl(membase + UART01x_FR) & UART01x_FR_TXFF)
		;
	writeb(c, membase + UART01x_DR);
	while (readl(membase + UART01x_FR) & UART01x_FR_BUSY)
		;
}

static void pl011_simple_write(struct console *con, const char *s, unsigned count)
{
	struct simple_console_device *dev = con->data;
	unsigned int i;

	for (i = 0; i < count; i++, s++) {
		if (*s == '\n')
			pl011_simple_putc(dev, '\r');
		pl011_simple_putc(dev, *s);
	}
}

static void __iomem * __init simple_console_map(unsigned long paddr, size_t size)
{
	void __iomem *base;

	early_mapping(EARLY_MAP_EARLYCON, paddr & PAGE_MASK, EARLY_MAP_IO);
	base = (void __iomem *)early_map_to_virt(EARLY_MAP_EARLYCON);
	base += paddr & ~PAGE_MASK;

	return base;
}

#if CONFIG_QEMU
#define AON_UART_BASE_ADDR                   0x9000000
#else
#define AON_UART_BASE_ADDR                  0x0011f000
#endif
static unsigned char __iomem	* earlycon_base = NULL;
static int __init pl011_init_simple(struct simple_console_device *device,
					    const char *opt)
{
	earlycon_base = simple_console_map(AON_UART_BASE_ADDR, PAGE_SIZE);
	device->priv = earlycon_base;

	if (!device->priv)
		return -ENODEV;

	device->con->write = pl011_simple_write;
	return 0;
}

const struct simple_console_id simple_console_pl011 = {
				.name  = "pl011",
		     		.init = pl011_init_simple,
		     	};
