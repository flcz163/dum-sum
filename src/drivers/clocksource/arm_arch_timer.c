#include <dim-sum/delay.h>
#include <dim-sum/types.h>
#include <dim-sum/init.h>
#include <dim-sum/timer_device.h>
#include <dim-sum/smp.h>
#include <dim-sum/irq.h>
#include <dim-sum/irq_mapping.h>
#include <dim-sum/timer_device.h>
#include <dim-sum/percpu.h>
#include <dim-sum/cpumask.h>

#include <clocksource/arm_arch_timer.h>
#include <asm/arch_timer.h>
#include <asm/percpu.h>
//#include <asm/pgtable-types.h>
#include <asm/io.h>

static u32 arch_timer_rate;

#define ARCH_CP15_TIMER	BIT(0)
#define ARCH_MEM_TIMER	BIT(1)

struct arch_timer {
	void __iomem *base;
	struct timer_device evt;
};

#define to_arch_timer(e) container_of(e, struct arch_timer, evt)

#define CNTTIDR		0x08
#define CNTTIDR_VIRT(n)	(BIT(1) << ((n) * 4))

#define CNTVCT_LO	0x08
#define CNTVCT_HI	0x0c
#define CNTFRQ		0x10
#define CNTP_TVAL	0x28
#define CNTP_CTL	0x2c
#define CNTV_TVAL	0x38
#define CNTV_CTL	0x3c

DEFINE_PER_CPU(struct timer_device, arch_timer_evt);
u32 arch_timer_get_rate(void)
{
	return arch_timer_rate;
}

static void
arch_timer_detect_rate(void)
{
	arch_timer_rate = arch_timer_get_cntfrq();
	loops_per_second = arch_timer_rate;

	printk("timer_rate is %d.\n", arch_timer_rate);
	arch_timer_arch_init();
}

enum ppi_nr {
	PHYS_SECURE_PPI,
	PHYS_NONSECURE_PPI,
	VIRT_PPI,
	HYP_PPI,
	MAX_TIMER_PPI
};

static int arch_timer_ppi[MAX_TIMER_PPI];

static struct irq_configure arch_timer_of_desc[MAX_TIMER_PPI] =
	{
		[PHYS_SECURE_PPI] = {3, {1, 13, 1}, NULL},
		[PHYS_NONSECURE_PPI] = {3, {1, 14, 1}},
		[VIRT_PPI] = {3, {1, 11, 1}},
		[HYP_PPI] = {3, {1, 10, 1}},
	};

static __always_inline
void arch_timer_reg_write(int access, enum arch_timer_reg reg, u32 val,
			  struct timer_device *clk)
{
	if (access == ARM_TIMER_MEM_REAL_ACCESS) {
		struct arch_timer *timer = to_arch_timer(clk);
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			writel_relaxed(val, timer->base + CNTP_CTL);
			break;
		case ARCH_TIMER_REG_TVAL:
			writel_relaxed(val, timer->base + CNTP_TVAL);
			break;
		}
	} else if (access == ARM_TIMER_MEM_VIRT_ACCESS) {
		struct arch_timer *timer = to_arch_timer(clk);
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			writel_relaxed(val, timer->base + CNTV_CTL);
			break;
		case ARCH_TIMER_REG_TVAL:
			writel_relaxed(val, timer->base + CNTV_TVAL);
			break;
		}
	} else {
		arch_timer_reg_write_cp15(access, reg, val);
	}
}


static __always_inline
u32 arch_timer_reg_read(int access, enum arch_timer_reg reg,
			struct timer_device *clk)
{
	u32 val;

	if (access == ARM_TIMER_MEM_REAL_ACCESS) {
		struct arch_timer *timer = to_arch_timer(clk);
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			val = readl_relaxed(timer->base + CNTP_CTL);
			break;
		case ARCH_TIMER_REG_TVAL:
			val = readl_relaxed(timer->base + CNTP_TVAL);
			break;
		}
	} else if (access == ARM_TIMER_MEM_VIRT_ACCESS) {
		struct arch_timer *timer = to_arch_timer(clk);
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			val = readl_relaxed(timer->base + CNTV_CTL);
			break;
		case ARCH_TIMER_REG_TVAL:
			val = readl_relaxed(timer->base + CNTV_TVAL);
			break;
		}
	} else {
		val = arch_timer_reg_read_cp15(access, reg);
	}

	return val;
}

static __always_inline enum isr_result timer_handler(const int access,
					struct timer_device *evt)
{
	unsigned long ctrl;

	ctrl = arch_timer_reg_read(access, ARCH_TIMER_REG_CTRL, evt);
	if (ctrl & ARCH_TIMER_CTRL_IT_STAT) {
		ctrl |= ARCH_TIMER_CTRL_IT_MASK;
		arch_timer_reg_write(access, ARCH_TIMER_REG_CTRL, ctrl, evt);
		//printk("xby_debug in timer_handler, %p.\n", evt->handle);
		evt->handle(evt);
		return ISR_EATEN;
	}

	return ISR_SKIP;
}


static enum isr_result arch_timer_handler_phys(unsigned int irq, void *dev_id)
{
	struct timer_device *evt = dev_id;

	return timer_handler(ARM_TIMER_CP15_REAL_ACCESS, evt);
}

static __always_inline void timer_set_mode(const int access, int mode,
				  struct timer_device *clk)
{
	unsigned long ctrl;
	switch (mode) {
	case CLK_MODE_FREE:
	case CLK_MODE_POWERDOWN:
		ctrl = arch_timer_reg_read(access, ARCH_TIMER_REG_CTRL, clk);
		ctrl &= ~ARCH_TIMER_CTRL_ENABLE;
		arch_timer_reg_write(access, ARCH_TIMER_REG_CTRL, ctrl, clk);
		break;
	default:
		break;
	}
}


static void arch_timer_set_mode_phys(enum clock_mode mode,
				     struct timer_device *clk)
{
	timer_set_mode(ARM_TIMER_CP15_REAL_ACCESS, mode, clk);
}

static __always_inline void trigger_timer(const int access, unsigned long evt,
					   struct timer_device *clk)
{
	unsigned long ctrl;
	ctrl = arch_timer_reg_read(access, ARCH_TIMER_REG_CTRL, clk);
	ctrl |= ARCH_TIMER_CTRL_ENABLE;
	ctrl &= ~ARCH_TIMER_CTRL_IT_MASK;
	arch_timer_reg_write(access, ARCH_TIMER_REG_TVAL, evt, clk);
	arch_timer_reg_write(access, ARCH_TIMER_REG_CTRL, ctrl, clk);
}


static int arch_timer_set_next_event_phys(unsigned long evt,
					  struct timer_device *clk)
{
	trigger_timer(ARM_TIMER_CP15_REAL_ACCESS, evt, clk);
	return 0;
}

static void __arch_timer_setup(unsigned type,
			       struct timer_device *clk)
{
	clk->features = CLK_FEAT_ONESHOT;

	clk->features |= CLK_FEAT_C3STOP;
	clk->name = "arch_sys_timer";
	clk->rating = 450;
	clk->irq = arch_timer_ppi[PHYS_SECURE_PPI];
	clk->set_mode = arch_timer_set_mode_phys;
	clk->trigger_timer = arch_timer_set_next_event_phys;

	clk->set_mode(CLK_MODE_POWERDOWN, clk);

	clk->min_counter = 0xf;
	clk->max_counter = 0x7fffffff;
	config_timer_device(clk, arch_timer_rate);
	register_timer_device(clk);
	
	clk->trigger_timer((unsigned long) arch_timer_rate / HZ, clk);
}

static void arch_counter_set_user_access(void)
{
	u32 cntkctl = arch_timer_get_cntkctl();

	/* Disable user access to the timers and the physical counter */
	/* Also disable virtual event stream */
	cntkctl &= ~(ARCH_TIMER_USR_PT_ACCESS_EN
			| ARCH_TIMER_USR_VT_ACCESS_EN
			| ARCH_TIMER_VIRT_EVT_EN
			| ARCH_TIMER_USR_PCT_ACCESS_EN);

	/* Enable user access to the virtual counter */
	cntkctl |= ARCH_TIMER_USR_VCT_ACCESS_EN;

	arch_timer_set_cntkctl(cntkctl);
}

static int arch_timer_setup(struct timer_device *clk)
{
	__arch_timer_setup(ARCH_CP15_TIMER, clk);

	enable_percpu_irq(arch_timer_ppi[PHYS_SECURE_PPI], 0);
	if (arch_timer_ppi[PHYS_NONSECURE_PPI])
		enable_percpu_irq(arch_timer_ppi[PHYS_NONSECURE_PPI], 0);

	arch_counter_set_user_access();

	return 0;
}

static int __init arch_timer_register(void)
{
	int err;
	int ppi;

	ppi = arch_timer_ppi[PHYS_SECURE_PPI];
	err = register_percpu_irq_handle(ppi, arch_timer_handler_phys,
					 "arch_timer", &arch_timer_evt);
	if (!err && arch_timer_ppi[PHYS_NONSECURE_PPI]) {
		ppi = arch_timer_ppi[PHYS_NONSECURE_PPI];
		err = register_percpu_irq_handle(ppi, arch_timer_handler_phys,
					 "arch_timer", &arch_timer_evt);
		if (err)
			unregister_percpu_irq_handle(arch_timer_ppi[PHYS_SECURE_PPI],
					&arch_timer_evt);
	}

	if (err) {
		pr_err("arch_timer: can't register interrupt %d (%d)\n",
		       ppi, err);
		goto out_free;
	}

	/* Immediately configure the timer on the boot CPU */
	arch_timer_setup(this_cpu_var(&arch_timer_evt));
	
out_free:
	//free_percpu(&arch_timer_evt);
//out:
	return err;
}

void __init init_time_arch(void)
{
	arch_timer_detect_rate();
}

void __init init_timer_arch(void)
{
	int i;

	for (i = PHYS_SECURE_PPI; i < MAX_TIMER_PPI; i++)
		arch_timer_ppi[i] = init_one_hwirq(&arch_timer_of_desc[i]);

	arch_timer_register();
}

void arch_timer_secondary_init(void)
{
	arch_timer_setup(this_cpu_var(&arch_timer_evt));
}
