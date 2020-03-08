#include <dim-sum/beehive.h>
#include <dim-sum/cpumask.h>
#include <dim-sum/percpu.h>
#include <dim-sum/errno.h>
#include <dim-sum/mm_types.h>
#include <dim-sum/ioport.h>
#include <dim-sum/irqchip/arm-gic.h>
#include <dim-sum/irq_mapping.h>
#include <dim-sum/irq.h>
#include <dim-sum/init.h>
#include <dim-sum/printk.h>
#include <dim-sum/timex.h>
#include <dim-sum/delay.h>
#include <dim-sum/smp.h>
//#include <dim-sum/spinlock_types.h>
#include <dim-sum/smp_lock.h>
#include <dim-sum/cpu.h>

#include <asm/memory.h>
#include <asm/io.h>
#include <asm/smp.h>
#include <asm/exception.h>

#define NR_GIC_CPU_IF 8
static u8 gic_cpu_map[NR_GIC_CPU_IF];

static int gic_cnt;
	
#ifndef MAX_GIC_NR
#define MAX_GIC_NR	1
#endif

union gic_base {
	void __iomem *common_base;
	void __percpu * __iomem *percpu_base;
};

struct gic_chip_data {
	union gic_base dist_base;
	union gic_base cpu_base;

	struct irq_mapping *domain;
	unsigned int gic_irqs;
};

static struct gic_chip_data gic_data[MAX_GIC_NR];
static struct irq_controller gic_chip;

#define gic_data_dist_base(d)	((d)->dist_base.common_base)
#define gic_data_cpu_base(d)	((d)->cpu_base.common_base)

static struct smp_lock irq_controller_lock = 
			SMP_LOCK_UNLOCKED(irq_controller_lock);

/**
 * 处理GIC中断的入口
 */
static void __exception_irq_entry gic_handle_irq(struct exception_spot *regs)
{
	u32 irqstat, irqnr;
	struct gic_chip_data *gic = &gic_data[0];
	void __iomem *cpu_base = gic_data_cpu_base(gic);

	do {
		irqstat = readl_relaxed(cpu_base + GIC_CPU_INTACK);
		irqnr = irqstat & GICC_IAR_INT_ID_MASK;

		if (likely(irqnr > 15 && irqnr < 1021)) {
			do_hard_irq(gic->domain, irqnr, regs);
			continue;
		}
		if (irqnr < 16) {
			writel_relaxed(irqstat, cpu_base + GIC_CPU_EOI);
			do_IPI(irqnr, regs);

			continue;
		}
		break;
	} while (1);
}

static int gic_irq_extract(struct irq_mapping *map, struct device_node *node,
			const u32 *configs, unsigned int config_size,
			unsigned int *hw_irq, unsigned int *type)
{
	unsigned long ret = 0;

	if (config_size < 3)
		return -EINVAL;

	/**
	 * 0~15用于SGI，例如IPI中断
	 */
	*hw_irq = configs[1] + 16;

	/**
	 * SPI
	 */
	if (!configs[0])
		*hw_irq += 16;

	*type = configs[2] & HWIRQ_TRIGGER_TYPE_MASK;

	return ret;
}

static int gic_irq_init(struct irq_mapping *map, unsigned int virt_irq,
			unsigned int hw_irq)
{
	irq_handler_f handle = handle_fasteoi_irq;

	if (hw_irq < 32)
		handle = handle_percpu_irq;

	prepare_one_irq(map, hw_irq < 32, virt_irq, hw_irq, &gic_chip,
			map->data, handle, NULL, NULL);

	return 0;
}

static void gic_irq_uninit(struct irq_mapping *map, unsigned int virt_irq)
{
}

static const struct irq_mapping_ops gic_irq_domain_ops = {
	.extract = gic_irq_extract,
	.init = gic_irq_init,
	.uninit = gic_irq_uninit,
};

static void gic_raise_softirq(const struct cpumask *mask, unsigned int irq)
{
	int cpu;
	unsigned long flags, map = 0;

	smp_lock_irqsave(&irq_controller_lock, flags);

	/* Convert our logical CPU mask into a physical one. */
	for_each_cpu(cpu, mask)
		map |= gic_cpu_map[cpu];

	/*
	 * Ensure that stores to Normal memory are visible to the
	 * other CPUs before they observe us issuing the IPI.
	 */
	dmb(ishst);

	/* this always happens on GIC0 */
	writel_relaxed(map << 16 | irq, gic_data_dist_base(&gic_data[0]) + GIC_DIST_SOFTINT);

	smp_unlock_irqrestore(&irq_controller_lock, flags);
}

static u8 gic_get_cpumask(struct gic_chip_data *gic)
{
	void __iomem *base = gic_data_dist_base(gic);
	u32 mask, i;

	for (i = mask = 0; i < 32; i += 4) {
		mask = readl_relaxed(base + GIC_DIST_TARGET + i);
		mask |= mask >> 16;
		mask |= mask >> 8;
		if (mask)
			break;
	}

	return mask;
}

static void __init gic_dist_config(void __iomem *base, int gic_irqs,
			    void (*sync_access)(void))
{
	unsigned int i;

	/*
	 * Set all global interrupts to be level triggered, active low.
	 */
	for (i = 32; i < gic_irqs; i += 16)
		writel_relaxed(GICD_INT_ACTLOW_LVLTRIG,
					base + GIC_DIST_CONFIG + i / 4);

	/*
	 * Set priority on all global interrupts.
	 */
	for (i = 32; i < gic_irqs; i += 4)
		writel_relaxed(GICD_INT_DEF_PRI_X4, base + GIC_DIST_PRI + i);

	/*
	 * Disable all interrupts.  Leave the PPI and SGIs alone
	 * as they are enabled by redistributor registers.
	 */
	for (i = 32; i < gic_irqs; i += 32)
		writel_relaxed(GICD_INT_EN_CLR_X32,
					base + GIC_DIST_ENABLE_CLEAR + i / 8);

	if (sync_access)
		sync_access();
}


static void __init gic_dist_init(struct gic_chip_data *gic)
{
	unsigned int i;
	u32 cpumask;
	unsigned int gic_irqs = gic->gic_irqs;
	void __iomem *base = gic_data_dist_base(gic);

	writel_relaxed(GICD_DISABLE, base + GIC_DIST_CTRL);

	/*
	 * Set all global interrupts to this CPU only.
	 */
	cpumask = gic_get_cpumask(gic);
	cpumask |= cpumask << 8;
	cpumask |= cpumask << 16;
	for (i = 32; i < gic_irqs; i += 4)
		writel_relaxed(cpumask, base + GIC_DIST_TARGET + i * 4 / 4);

	gic_dist_config(base, gic_irqs, NULL);

	writel_relaxed(GICD_ENABLE, base + GIC_DIST_CTRL);
}

static void gic_cpu_if_up(void)
{
	void __iomem *cpu_base = gic_data_cpu_base(&gic_data[0]);
	u32 bypass = 0;

	/*
	* Preserve bypass disable bits to be written back later
	*/
	bypass = readl(cpu_base + GIC_CPU_CTRL);
	bypass &= GICC_DIS_BYPASS_MASK;

	writel_relaxed(bypass | GICC_ENABLE, cpu_base + GIC_CPU_CTRL);
}

static void gic_cpu_config(void __iomem *base, void (*sync_access)(void))
{
	int i;

	/*
	 * Deal with the banked PPI and SGI interrupts - disable all
	 * PPI interrupts, ensure all SGI interrupts are enabled.
	 */
	writel_relaxed(GICD_INT_EN_CLR_PPI, base + GIC_DIST_ENABLE_CLEAR);
	writel_relaxed(GICD_INT_EN_SET_SGI, base + GIC_DIST_ENABLE_SET);

	/*
	 * Set priority on PPI and SGI interrupts
	 */
	for (i = 0; i < 32; i += 4)
		writel_relaxed(GICD_INT_DEF_PRI_X4,
					base + GIC_DIST_PRI + i * 4 / 4);

	if (sync_access)
		sync_access();
}

static void gic_cpu_init(struct gic_chip_data *gic)
{
	void __iomem *dist_base = gic_data_dist_base(gic);
	void __iomem *base = gic_data_cpu_base(gic);
	unsigned int cpu_mask, cpu = smp_processor_id();
	int i;

	/*
	 * Get what the GIC says our CPU mask is.
	 */
	BUG_ON(cpu >= NR_GIC_CPU_IF);
	cpu_mask = gic_get_cpumask(gic);
	gic_cpu_map[cpu] = cpu_mask;

	/*
	 * Clear our mask from the other map entries in case they're
	 * still undefined.
	 */
	for (i = 0; i < NR_GIC_CPU_IF; i++)
		if (i != cpu)
			gic_cpu_map[i] &= ~cpu_mask;

	gic_cpu_config(dist_base, NULL);

	writel_relaxed(GICC_INT_PRI_THRESHOLD, base + GIC_CPU_PRIMASK);
	gic_cpu_if_up();
}

static void __init gic_pm_init(struct gic_chip_data *gic)
{
}

static inline void __iomem *gic_dist_base(struct irq_desc *d)
{
	struct gic_chip_data *gic_data = irq_data_get_irq_chip_data(d);
	return gic_data_dist_base(gic_data);
}

static inline void __iomem *gic_cpu_base(struct irq_desc *d)
{
	struct gic_chip_data *gic_data = irq_data_get_irq_chip_data(d);
	return gic_data_cpu_base(gic_data);
}

static inline unsigned int gic_irq(struct irq_desc *d)
{
	return d->hw_irq;
}

/*
 * Routines to acknowledge, disable and enable interrupts
 */
static void gic_poke_irq(struct irq_desc *d, u32 offset)
{
	u32 mask = 1 << (gic_irq(d) % 32);
	writel_relaxed(mask, gic_dist_base(d) + offset + (gic_irq(d) / 32) * 4);
}


static void gic_mask_irq(struct irq_desc *d)
{
	gic_poke_irq(d, GIC_DIST_ENABLE_CLEAR);
}

static void gic_unmask_irq(struct irq_desc *d)
{
	gic_poke_irq(d, GIC_DIST_ENABLE_SET);
}

static void gic_eoi_irq(struct irq_desc *d)
{
	writel_relaxed(gic_irq(d), gic_cpu_base(d) + GIC_CPU_EOI);
}

static int gic_configure_irq(unsigned int irq, unsigned int type,
		       void __iomem *base, void (*sync_access)(void))
{
	u32 enablemask = 1 << (irq % 32);
	u32 enableoff = (irq / 32) * 4;
	u32 confmask = 0x2 << ((irq % 16) * 2);
	u32 confoff = (irq / 16) * 4;
	bool enabled = false;
	u32 val, oldval;
	int ret = 0;

	/*
	 * Read current configuration register, and insert the config
	 * for "irq", depending on "type".
	 */
	val = oldval = readl_relaxed(base + GIC_DIST_CONFIG + confoff);
	if (type & HWIRQ_TYPE_LEVEL_MASK)
		val &= ~confmask;
	else if (type & HWIRQ_TYPE_EDGE_MASK)
		val |= confmask;

	/*
	 * As recommended by the spec, disable the interrupt before changing
	 * the configuration
	 */
	if (readl_relaxed(base + GIC_DIST_ENABLE_SET + enableoff) & enablemask) {
		writel_relaxed(enablemask, base + GIC_DIST_ENABLE_CLEAR + enableoff);
		if (sync_access)
			sync_access();
		enabled = true;
	}

	/*
	 * Write back the new configuration, and possibly re-enable
	 * the interrupt. If we tried to write a new configuration and failed,
	 * return an error.
	 */
	writel_relaxed(val, base + GIC_DIST_CONFIG + confoff);
	if (readl_relaxed(base + GIC_DIST_CONFIG + confoff) != val && val != oldval)
		ret = -EINVAL;

	if (enabled)
		writel_relaxed(enablemask, base + GIC_DIST_ENABLE_SET + enableoff);

	if (sync_access)
		sync_access();

	return ret;
}

static int gic_set_type(struct irq_desc *d, unsigned int type)
{
	void __iomem *base = gic_dist_base(d);
	unsigned int gicirq = gic_irq(d);

	/* Interrupt configuration for SGIs can't be changed */
	if (gicirq < 16)
		return -EINVAL;

	/* SPIs have restrictions on the supported types */
	if (gicirq >= 32 && type != HWIRQ_TYPE_LEVEL_HIGH &&
			    type != HWIRQ_TYPE_EDGE_RISING)
		return -EINVAL;

	return gic_configure_irq(gicirq, type, base, NULL);
}

static struct irq_controller gic_chip = {
	.name			= "GIC",
	.mask		= gic_mask_irq,
	.unmask		= gic_unmask_irq,
	.eoi		= gic_eoi_irq,
	.set_trigger_type		= gic_set_type,
};

#define gic_data_dist_base(d)	((d)->dist_base.common_base)
#define gic_data_cpu_base(d)	((d)->cpu_base.common_base)
#define gic_set_base_accessor(d, f)

void __init gic_init_bases(unsigned int gic_nr, int irq_start,
			   void __iomem *dist_base, void __iomem *cpu_base,
			   u32 percpu_offset, struct device_node *node)
{
	//unsigned int hwirq_base;
	struct gic_chip_data *gic;
	int gic_irqs, i;
	//int irq_base;

	BUG_ON(gic_nr >= MAX_GIC_NR);

	gic = &gic_data[gic_nr];

	{
		/* Normal, sane GIC... */
#if 0
		WARN(percpu_offset,
		     "GIC_NON_BANKED not enabled, ignoring %08x offset!",
		     percpu_offset);
#endif
		gic->dist_base.common_base = dist_base;
		gic->cpu_base.common_base = cpu_base;
		gic_set_base_accessor(gic, gic_get_common_base);
	}

	/*
	 * Initialize the CPU interface map to all CPUs.
	 * It will be refined as each CPU probes its ID.
	 */
	for (i = 0; i < NR_GIC_CPU_IF; i++)
		gic_cpu_map[i] = 0xff;

	/*
	 * Find out how many interrupts are supported.
	 * The GIC only supports up to 1020 interrupt sources.
	 */
	gic_irqs = readl_relaxed(gic_data_dist_base(gic) + GIC_DIST_CTR) & 0x1f;
	gic_irqs = (gic_irqs + 1) * 32;
	if (gic_irqs > 1020)
		gic_irqs = 1020;
	gic->gic_irqs = gic_irqs;

	/* DT case */
	gic->domain = alloc_init_irq_mapping(node, gic_irqs, &gic_irq_domain_ops, gic);

	if (gic_nr == 0) {
#ifdef CONFIG_SMP
		smp_set_raise_ipi_call(gic_raise_softirq);
		//register_cpu_notifier(&gic_cpu_notifier);
#endif
		set_chip_irq_handle(gic_handle_irq);
	}

	gic_dist_init(gic);
	gic_cpu_init(gic);
	gic_pm_init(gic);
}

#define gic_init_physaddr(node)  do { } while (0)

int gic_v2_init(void)
{
	void __iomem *cpu_base;
	void __iomem *dist_base;
	u32 percpu_offset;

	dist_base = ioremap(0x8000000, 0x10000);
	cpu_base = ioremap(0x8010000, 0x10000);
	percpu_offset = 0;

	gic_init_bases(gic_cnt, -1, dist_base, cpu_base, percpu_offset, NULL);

	if (!gic_cnt)
		gic_init_physaddr(node);

	gic_cnt++;

	return 0;
}

void gic_secondary_init(void)
{
	gic_cpu_init(&gic_data[0]);
}
