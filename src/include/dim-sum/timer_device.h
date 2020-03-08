#ifndef __DIM_SUM_TIMER_DEVICE_H
#define __DIM_SUM_TIMER_DEVICE_H

#include <dim-sum/types.h>

#include <asm/div64.h>

/**
 * 时钟设备可能的状态
 */
enum timer_device_state {
	/**
	 * 电源关闭状态
	 */
	CLK_STATE_POWERDOWN,
	/**
	 * 未被使用
	 */
	CLK_STATE_FREE,
	/**
	 * 工作于周期事件
	 */
	CLK_STATE_PERIODIC,
	/**
	 * 工作于一次性事件
	 * 要模拟周期事件，需要在事件处理中对硬件进行编程
	 */
	CLK_STATE_ONESHOT,
};

enum {
	/**
	 * 具备产生周期性事件的能力
	 */
	__CLK_FEAT_PERIODIC,
	/**
	 * 具备产生一次性事件的能力
	 */
	__CLK_FEAT_ONESHOT,
	/**
	 * CPU进入C3睡眠状态时，该时钟会失效
	 * 此时需要其他核进行事件广播以唤醒它
	 */
	__CLK_FEAT_C3STOP,
};

/**
 *  时钟设备的特性标志
 */
#define CLK_FEAT_PERIODIC (1UL << __CLK_FEAT_PERIODIC)
#define CLK_FEAT_ONESHOT	(1UL << __CLK_FEAT_ONESHOT)
#define CLK_FEAT_C3STOP		(1UL << __CLK_FEAT_C3STOP)

/**
 * 时钟工作模式，用于set_mode()
 */
enum clock_mode {
	CLK_MODE_FREE,
	CLK_MODE_POWERDOWN,
	CLK_MODE_PERIODIC,
	CLK_MODE_ONESHOT,
	CLK_MODE_RESUME,
};

/**
 * 时钟设备描述符
 */
struct timer_device {
	const char		*name;
	unsigned int		features;
	int				rating;
	int				irq;
	enum timer_device_state	state;
	/**
	 * 设置支持的最小时间间隔(ns)
	 */
	u64				min_ns;
	/**
	 * 设置支持的最大时间间隔(ns)
	 */
	u64				max_ns;
	/**
	 * 最小的定时器计数值
	 */
	unsigned long		min_counter;
	/**
	 * 最大的定时器计数值
	 */
	unsigned long		max_counter;
	/**
	 * 用于在ns和counter值之间快速转换
	 * 参见ns_to_timer_counter
	 */
	u32				mult;
	u32				shift;

	/**
	 * 中断事件处理函数
	 */
	void				(*handle)(struct timer_device *);

	void				(*set_mode)(enum clock_mode, struct timer_device *);
	/** 
	 * 控制下一次定时器产生的时间点
	 * 以cycles为单位
	 */
	int			(*trigger_timer)(unsigned long counter, struct timer_device *);

	/**
	 * 通过此字段将设备加入到全局链表
	 */
	struct double_list	list;

};

/**
 * 将纳秒数转换为定时器设备cycles数
 */
static inline unsigned long ns_to_timer_counter(struct timer_device *dev, unsigned long ns)
{
	unsigned long counter;

	counter = (unsigned long long)ns  * dev->mult >> dev->shift;

	return counter;
}

static inline u64
timer_counter_to_ns(struct timer_device *dev, unsigned long counter)
{
	u64 tmp = (u64) counter << dev->shift;

	do_div(tmp, dev->mult);

	return tmp;
}

void register_timer_device(struct timer_device *dev);
void config_timer_device(struct timer_device *dev, u32 freq);

#endif /* __DIM_SUM_TIMER_DEVICE_H */
