#ifndef __DIM_SUM_IRQ_H
#define __DIM_SUM_IRQ_H

#include <dim-sum/cpumask.h>
#include <dim-sum/device_tree.h>
#include <dim-sum/irqhandler.h>
#include <dim-sum/of.h>
#include <dim-sum/wait.h>
#include <dim-sum/cache.h>
#include <dim-sum/irq_controller.h>

#include <asm/ptrace.h>
#include <asm/irq.h>

struct irq_desc;
struct irq_configure;

#ifndef NR_IRQS
#define NR_IRQS 128
#endif

/**
 * 中断处理结果
 */
enum isr_result {
	/**
	 * ISR没有处理该中断
	 */
	ISR_SKIP,
	/**
	 * ISR*吃掉*了本次中断
	 */
	ISR_EATEN,
};

#ifndef ARCH_IRQ_INIT_FLAGS
#define ARCH_IRQ_INIT_FLAGS	0
#endif

#define IRQ_DEFAULT_INIT_FLAGS	ARCH_IRQ_INIT_FLAGS

enum {
	/**
	 * 需要注释一下吗:)
	 */
	__HWIRQ_TYPE_EDGE_RISING,
	__HWIRQ_TYPE_EDGE_FALLING,
	__HWIRQ_TYPE_LEVEL_HIGH,
	__HWIRQ_TYPE_LEVEL_LOW,

	/**
	 * 禁止挂接ISR
	 * 当中断被用于控制器级联时
	 * 禁止驱动申请相应的级联中断
	 */
	__HWIRQ_DISALBE_ISR,
	/**
	 * 相应的中断是每CPU中断
	 * 如本地时钟
	 */
	__HWIRQ_PER_CPU,
};

/**
 * 硬件中断的类型
 */
#define HWIRQ_TYPE_NONE		0
#define HWIRQ_TYPE_EDGE_RISING	(1UL << __HWIRQ_TYPE_EDGE_RISING)
#define HWIRQ_TYPE_EDGE_FALLING	(1UL << __HWIRQ_TYPE_EDGE_FALLING)
#define HWIRQ_TYPE_EDGE_MASK	\
		(HWIRQ_TYPE_EDGE_RISING | HWIRQ_TYPE_EDGE_FALLING)

#define HWIRQ_TYPE_LEVEL_HIGH	(1UL << __HWIRQ_TYPE_LEVEL_HIGH)
#define HWIRQ_TYPE_LEVEL_LOW		(1UL << __HWIRQ_TYPE_LEVEL_LOW)

#define HWIRQ_TYPE_LEVEL_MASK	\
		(HWIRQ_TYPE_LEVEL_HIGH | HWIRQ_TYPE_LEVEL_LOW)

#define HWIRQ_TRIGGER_TYPE_MASK \
	(HWIRQ_TYPE_EDGE_MASK | HWIRQ_TYPE_LEVEL_MASK)

#define HWIRQ_DISALBE_ISR		(1UL << __HWIRQ_DISALBE_ISR)
#define HWIRQ_PER_CPU			(1UL << __HWIRQ_PER_CPU)

enum {
	__IRQFLAG_TRIGGER_RISING,
	__IRQFLAG_TRIGGER_FALLING,
	__IRQFLAG_TRIGGER_HIGH,
	__IRQFLAG_TRIGGER_LOW,
	/**
	 * 是否允许在设备间共享
	 * 即多个设备共用同一个中断号
	 */
	__IRQFLAG_SHARED,
	/**
	 * 时钟中断
	 */
	__IRQFLAG_TIMER,
	/**
	 * 该中断是否是每CPU中断，如Local timer
	 */
	__IRQFLAG_PERCPU,
	/**
	 * 别线程化这些中断，即使强制线程化功能打开也是如此
	 * 如Timer、级联中断。
	 */
	__IRQFLAG_NO_THREAD,
};

/**
 * 注册中断处理回调函数时指定的标志
 */
#define IRQFLAG_TRIGGER_RISING		(1UL << __IRQFLAG_TRIGGER_RISING)
#define IRQFLAG_TRIGGER_FALLING	(1UL << __IRQFLAG_TRIGGER_FALLING)
#define IRQFLAG_TRIGGER_HIGH		(1UL << __IRQFLAG_TRIGGER_HIGH)
#define IRQFLAG_TRIGGER_LOW		(1UL << __IRQFLAG_TRIGGER_LOW)
#define IRQFLAG_TRIGGER_MASK	(IRQFLAG_TRIGGER_HIGH | IRQFLAG_TRIGGER_LOW | \
				 IRQFLAG_TRIGGER_RISING | IRQFLAG_TRIGGER_FALLING)

#define IRQFLAG_SHARED		(1UL << __IRQFLAG_SHARED)
#define IRQFLAG_TIMER 		((1UL << __IRQFLAG_TIMER) || (1UL << __IRQFLAG_NO_THREAD))
#define IRQFLAG_PERCPU		(1UL << __IRQFLAG_PERCPU)
#define IRQFLAG_NO_THREAD	(1UL << __IRQFLAG_NO_THREAD)

/**
 * 单个中断的当前状态
 */
enum {
	/**
	 * 被禁止，即软件不响应中断
	 */
	__IRQSTATE_IRQ_DISABLED,
	/**
	 * 被屏蔽，即硬件不发送中断
	 */
	__IRQSTATE_IRQ_MASKED,
	/**
	 * 正在被处理
	 */
	__IRQSTATE_IRQ_INPROGRESS,
	/**
	 * 随后的标志用于保留用途
	 */
	__IRQSTATE_COUNT,
};

#define IRQSTATE_IRQ_DISABLED		(1UL << __IRQSTATE_IRQ_DISABLED)
#define IRQSTATE_IRQ_MASKED		(1UL << __IRQSTATE_IRQ_MASKED)
#define IRQSTATE_IRQ_INPROGRESS	(1UL << __IRQSTATE_IRQ_INPROGRESS)

/**
 * 描述特定中断属性的描述符
 */
struct irq_configure {
	/**
	 * 配置参数个数
	 */
	int args_count;
	/**
	 * 配置参数值
	 */
	uint32_t args[MAX_PHANDLE_ARGS];
	/**
	 * 所属设备节点，以后支持设备树时需要
	 */
	struct device_node *np;
};

typedef enum isr_result (*isr_handler_f)(unsigned int irq, void *data);
/**
 * 中断处理回调描述符
 */
struct irq_callback {
	/**
	 * 描述符名称
	 */
	const char		*name;
	/**
	 * 标志，如IRQFLAG_PERCPU
	 */
	unsigned int		flags;	
	/**
	 * 回调函数
	 */
	isr_handler_f	handler;
	/**
	 * 传递给回调函数的设备参数
	 */
	void				*dev_id;
	/**
	 * 对于percpu ISR来说，传入该参数
	 * 各个CPU不一样
	 */
	void __percpu		*percpu_dev_id;
	/**
	 * 链接到isr_list链表*针对IRQFLAG_SHARE中断来说*
	 */
	struct double_list	list;
} aligned_cacheline_in_smp;

/**
 * IRQ描述符
 */
struct irq_desc {
	/**
	 * 名称，以后procfs用
	 */
	const char		*name;
	/**
	 * 虚拟中断号
	 */
	unsigned int		virt_irq;
	/**
	 * 硬件中断号
	 */
	unsigned long		hw_irq;
	/**
	 * 硬件中断标志，如HWIRQ_XX
	 */
	unsigned int		hwflag;
	/**
	 * 保护本描述符的锁
	 */
	struct smp_lock		lock;
	/**
	 * 共享状态
	 *	1、ISR链表为空
	 *	2、允许共享
	 *	3、不允许共享
	 */
	enum {
		IRQ_SHARE_EMPTY,
		IRQ_SHARE_ALLOWED,
		IRQ_SHARE_DISALLOWED,
	} share_state;
	/**
	 * 已经存在的ISR，其ISR标志
	 * 用来确定是否允许后续ISR与现有ISR共享
	 */
	unsigned int share_flag;
	/**
	 * 状态，分两部分 
	 *	1、ISR可以访问的公共状态，如IRQSTATE_XX
	 *	2、中断子系统内部状态，如IRQS_XX
	 */
	unsigned int		state;
	/**
	 * 中断处理函数，注意不是ISR
	 * 根据中断类型不同，调用不同的函数，如:
	 *		handle_xx_irq
	 */
	irq_handler_f		handle;
	/**
	 * ISR链
	 */
	struct double_list	isr_list;
	/**
	 * 中断控制器
	 */
	struct irq_controller	*controller;
	/**
	 * 中断控制器的私有数据
	 */
	void			*controller_priv;
	/**
	 * 中断映射描述符
	 */
	struct irq_mapping	*map;
	/**
	 * 暂时不用
	 */
	struct cpumask	*percpu_enabled;
};

extern struct irq_desc all_irq_desc[];
/**
 * 得到中断号对应的中断描述符
 */
static inline struct irq_desc *get_irq_desc(unsigned int irq)
{
	return (irq < NR_IRQS) ? &all_irq_desc[irq] : NULL;
}

static inline u32 get_irq_trigger_type(unsigned int irq)
{
	struct irq_desc *desc = get_irq_desc(irq);

	return desc ? (desc->hwflag & HWIRQ_TRIGGER_TYPE_MASK) : 0;
}

static inline void irq_state_clear(struct irq_desc *d, unsigned int mask)
{
	d->state &= ~mask;
}

static inline void irq_state_set(struct irq_desc *d, unsigned int mask)
{
	d->state |= mask;
}

static inline void *irq_data_get_irq_chip_data(struct irq_desc *d)
{
	return d->controller_priv;
}

/**
 * 中断处理函数，注意不是ISR
 *	handle_bad_irq未设置中断触发类型时调用
 *	handle_percpu_irq每CPU中断
 *	handle_fasteoi_irq普通外设中断
 */
extern void handle_bad_irq(unsigned int irq, struct irq_desc *desc);
extern void handle_percpu_irq(unsigned int irq, struct irq_desc *desc);
extern void handle_fasteoi_irq(unsigned int irq, struct irq_desc *desc);

extern int __must_check
register_percpu_irq_handle(unsigned int irq, isr_handler_f handler,
		   const char *devname, void __percpu *percpu_dev_id);
extern void unregister_percpu_irq_handle(unsigned int, void __percpu *);

extern int __must_check
register_isr_handle(unsigned int irq, isr_handler_f handler, unsigned long flags,
	    const char *name, void *dev);
extern void unregister_isr_handle(unsigned int, void *);

extern void enable_percpu_irq(unsigned int irq, unsigned int type);
extern void disable_percpu_irq(unsigned int irq);

void irq_enable(struct irq_desc *desc);
void irq_disable(struct irq_desc *desc);

/**
 * 标记中断为percpu中断
 */
int mark_percpu_irq(unsigned int irq);

void synchronize_irq(unsigned int irq);

extern void irq_preface(int irq);
extern void irq_tail(int irq, struct exception_spot *regs);
extern int do_hard_irq(struct irq_mapping *map, unsigned int hw_irq,
					struct exception_spot *regs);

void exception_tail(struct exception_spot *regs);

#define hardirq_count()	(preempt_count() & HARDIRQ_MASK)
#define softirq_count()	(preempt_count() & SOFTIRQ_MASK)
#define irq_count()	(preempt_count() & (HARDIRQ_MASK | SOFTIRQ_MASK))

#define in_hard_interrupt() (hardirq_count())
#define in_interrupt()		(irq_count())

/**
 * 初始化某个硬件中断
 * 	1、绑定其映射关系
 *	2、设置中断硬件类型
 */
unsigned int init_one_hwirq(struct irq_configure *irq_desc);

extern void init_IRQ(void);

#endif /* __DIM_SUM_IRQ_H */
