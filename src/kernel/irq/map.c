#include <dim-sum/beehive.h>
#include <dim-sum/irq.h>
#include <dim-sum/irq_mapping.h>

#include "internals.h"

static struct irq_mapping *irq_default_domain;
static struct mutex map_mutex = MUTEX_INITIALIZER(map_mutex);

/**
 * 得到一个硬件中断号对应的软件中断号
 */
unsigned int get_virt_irq(struct irq_mapping *map, unsigned int hw_irq)
{
	if (map == NULL)
		map = irq_default_domain;
	if (map == NULL)
		return 0;

	if (hw_irq < map->irq_count)
		return map->linear_map[hw_irq];

	return 0;
}

/**
 * 分配并初始化中断映射表
 */
struct irq_mapping *alloc_init_irq_mapping(struct device_node *of_node,
	unsigned int hwirq_count, const struct irq_mapping_ops *ops, void *data)
{
	struct irq_mapping *map;

	/**
	 * 分配映射表
	 */
	map = kzalloc(sizeof(*map) + (sizeof(map->linear_map[0]) * hwirq_count),
			      PAF_KERNEL);
	if (!map)
		return NULL;

	map->ops = ops;
	map->data = data;
	map->irq_count = hwirq_count;

	irq_default_domain = map;

	return map;
}

/**
 * 根据设备树节点查找相应的映射表
 * 以后会支持
 */
static inline struct irq_mapping *get_node_map(struct device_node *node)
{
	/**
	 * 目前直接返回默认映射表即可
	 * 暂不支持多控制器
	 */
	return irq_default_domain;
}

unsigned int init_one_hwirq(struct irq_configure *config)
{
	struct irq_desc *irq_desc;
	struct irq_mapping *map;
	unsigned int hw_irq;
	unsigned int type = HWIRQ_TYPE_NONE;
	int virq;
	int ret;

	map = config->np ? get_node_map(config->np) : irq_default_domain;
	if (!map) {
		pr_warn("no irq map found for %s !\n",
			of_node_full_name(config->np));
		return 0;
	}

	ASSERT(map->ops->extract != NULL);
	/**
	 * 调用控制器驱动的回调函数
	 * 解析硬件中断号及其类型
	 */
	if (map->ops->extract(map, config->np, config->args,
				config->args_count, &hw_irq, &type))
		return 0;

	/**
	 * be care
	 */
	virq = hw_irq;

	if (hw_irq >= map->irq_count)
		return -EINVAL;

	irq_desc = get_irq_desc(virq);
	if (!irq_desc || irq_desc->map)
		return -EINVAL;

	mutex_lock(&map_mutex);

	irq_desc->hw_irq = hw_irq;
	irq_desc->map = map;
	/**
	 * 默认值没有初始化链表，这里需要强制初始化一下
	 */
	list_init(&irq_desc->isr_list);
	if (map->ops->init) {
		ret = map->ops->init(map, virq, hw_irq);
		if (ret ) {
			irq_desc->map = NULL;
			irq_desc->hw_irq = 0;
			mutex_unlock(&map_mutex);
			return ret;
		}
	}

	if (!map->name && irq_desc->controller)
		map->name = irq_desc->controller->name;

	/**
	 * 建立虚实映射
	 */
	map->linear_map[hw_irq] = virq;

	mutex_unlock(&map_mutex);

	irq_desc->hwflag &= ~HWIRQ_DISALBE_ISR;
	
	if (type != HWIRQ_TYPE_NONE && type != get_irq_trigger_type(virq))
		set_irq_trigger_type(virq, type);

	return virq;
}
