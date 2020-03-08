#ifndef __DIM_SUM_IRQ_MAPPING_H
#define __DIM_SUM_IRQ_MAPPING_H

#include <dim-sum/irqhandler.h>

struct irq_mapping;
struct device_node;
struct irq_configure;
struct irq_controller;

struct irq_mapping_ops {
	/**
	 * 解析中断配置，得到硬件中断号及其类型
	 */
	int (*extract)(struct irq_mapping *map, struct device_node *node,
		     	const u32 *configs, unsigned int config_size,
		     	unsigned int *hw_irq, unsigned int *type);
	/**
	 * 初始化中断
	 */
	int (*init)(struct irq_mapping *map, unsigned int virt_irq,
			unsigned int hw_irq);
	/**
	 * 反初始化
	 */
	void (*uninit)(struct irq_mapping *map, unsigned int virt_irq);
};

/**
 * 该描述符处理逻辑中断和硬件中断号之间的映射
 */
struct irq_mapping {
	const char *name;
	/**
	 * 中断控制器处理映射的回调
	 */
	const struct irq_mapping_ops *ops;
	/**
	 * 私有数据，驱动自行使用
	 */
	void *data;
	/**
	 * 中断数量，决定映射表大小
	 */
	unsigned int irq_count;
	/**
	 * 线性映射表，必须放在最后面
	 */
	unsigned int linear_map[0];
};

/**
 * 分配并初始化一个新的映射描述符
 */
struct irq_mapping *alloc_init_irq_mapping(struct device_node *of_node,
	unsigned int hwirq_count, const struct irq_mapping_ops *ops, void *data);

/**
 * 做一些软件方面的初始
 * 为中断正式运行做准备
 */
extern void prepare_one_irq(struct irq_mapping *map, bool is_percpu,
				unsigned int virq, unsigned int hw_irq, struct irq_controller *controller,
				void *controller_priv, irq_handler_f handler,
				void *handler_data, const char *handler_name);

/**
 * 得到一个硬件中断号对应的软件中断号
 */
unsigned int get_virt_irq(struct irq_mapping *map, unsigned int hw_irq);

#endif /* __DIM_SUM_IRQ_MAPPING_H */
