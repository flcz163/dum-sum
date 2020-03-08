#include <dim-sum/timer_device.h>
#include <dim-sum/cpumask.h>
#include <dim-sum/irq.h>
#include <dim-sum/percpu.h>
#include <dim-sum/smp.h>
#include <dim-sum/timer.h>

/**
 * 保护定时器设备链表的锁
 */
static struct smp_lock clk_lock = SMP_LOCK_UNLOCKED(clk_lock);
/**
 * 当前可用的定时器设备
 */
static struct double_list clk_devices = LIST_HEAD_INITIALIZER(clk_devices);

static void
calc_mult_shift(u32 *mult, u32 *shift, u32 from, u32 to, u32 maxsec)
{
	u64 tmp;
	u32 sft, sftacc= 32;

	tmp = ((u64)maxsec * from) >> 32;
	while (tmp) {
		tmp >>=1;
		sftacc--;
	}

	for (sft = 32; sft > 0; sft--) {
		tmp = (u64) to << sft;
		tmp += from / 2;
		do_div(tmp, from);
		if ((tmp >> sftacc) == 0)
			break;
	}

	*mult = tmp;
	*shift = sft;
}

static u64
calc_max_min_ns(unsigned long latch, struct timer_device *evt, bool ismax)
{
	u64 tmp = (u64) latch << evt->shift;
	u64 rnd;

	if (unlikely(!evt->mult)) {
		evt->mult = 1;
		WARN("mult is zero.\n");
	}
	rnd = (u64) evt->mult - 1;

	/**
	 * 检查计算溢出
	 */
	if ((tmp >> evt->shift) != (u64)latch)
		tmp = ~0ULL;

	if ((~0ULL - tmp > rnd) &&
	    (!ismax || evt->mult <= (1ULL << evt->shift)))
		tmp += rnd;

	do_div(tmp, evt->mult);

	/**
	 * 最小的定时器间隔至少保持1微秒
	 * 否则太消耗CPU
	 */
	return tmp > 1000 ? tmp : 1000;
}

/**
 * 初始化定时器设备
 */
void config_timer_device(struct timer_device *dev, u32 freq)
{
	u64 max_sec;

	/**
	 * 周期性的，没啥好配置的了
	 */
	if (!(dev->features & CLK_FEAT_ONESHOT))
		return;

	/**
	 * 计算最大的秒数
	 */
	max_sec = dev->max_counter;
	do_div(max_sec, freq);
	/**
	 * 至少一秒
	 */
	if (!max_sec)
		max_sec = 1;
	else if (max_sec > 600 && dev->max_counter > UINT_MAX)
		max_sec = 600;

	/**
	 * 计算设备的mult，shift值，加快counter与ns的转换
	 */
	calc_mult_shift(&dev->mult, &dev->shift, NSEC_PER_SEC, freq, max_sec);

	/* 将cycles转换为ns，得到最大和最小的ns */
	dev->min_ns = calc_max_min_ns(dev->min_counter, dev, false);
	dev->max_ns = calc_max_min_ns(dev->max_counter, dev, true);
}

/**
 * 注册定时器设备
 */
void register_timer_device(struct timer_device *dev)
{
	unsigned long flags;

	dev->state = CLK_STATE_FREE;
	dev->handle = hrtimer_interrupt;
	
	smp_lock_irqsave(&clk_lock, flags);
	list_insert_front(&dev->list, &clk_devices);
	smp_unlock_irqrestore(&clk_lock, flags);
}
