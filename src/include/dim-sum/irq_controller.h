#ifndef __DIM_SUM_IRQCHIP_H
#define __DIM_SUM_IRQCHIP_H

struct irq_desc;

/**
 * 中断控制器描述符
 */
struct irq_controller {
	/**
	 * 中断控制器名称
	 */
	const char	*name;
	/**
	 * 锁函数，保护中断控制器描述符
	 * 可能睡眠!!
	 */
	void		(*lock)(struct irq_desc *data);
	/**
	 * 解锁
	 */
	void		(*unlock)(struct irq_desc *data);
	/**
	 * 初始化某个中断
	 */
	unsigned int	(*setup)(struct irq_desc *data);
	/**
	 * 反初始化某个中断
	 * 当没有中断ISR挂接在上面时调用
	 */
	void		(*shutdown)(struct irq_desc *data);
	/**
	 * 设置某个中断的类型
	 */
	int		(*set_trigger_type)(struct irq_desc *data, unsigned int type);
	/**
	 * 屏蔽某个中断
	 */
	void		(*mask)(struct irq_desc *data);
	/**
	 * 取消屏蔽
	 */
	void		(*unmask)(struct irq_desc *data);
	/**
	 * ack应答某个中断
	 */
	void		(*ack)(struct irq_desc *data);
	/**
	 * eoi应答中断
	 */
	void		(*eoi)(struct irq_desc *data);
	/**
	 * 打开中断，一般为空
	 */
	void		(*enable)(struct irq_desc *data);
	/**
	 * 关闭中断
	 */
	void		(*disable)(struct irq_desc *data);
};

void init_irq_controller(void);

#endif	/* __DIM_SUM_IRQCHIP_H */
