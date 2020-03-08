#include <dim-sum/irq.h>
#include <dim-sum/irq_mapping.h>
#include <dim-sum/percpu.h>

#include "internals.h"

/**
 * 设置中断类型，*无*锁保护
 */
int __set_irq_trigger_type(struct irq_desc *desc, unsigned int irq,
		      unsigned long type)
{
	struct irq_controller *controller = desc->controller;
	int ret;

	if (!controller || !controller->set_trigger_type) {
		return 0;
	}

	/**
	 * 直接调用驱动回调函数进行设置
	 */
	ret = controller->set_trigger_type(desc, type);

	return ret;
}

/**
 * 设置中断类型，*有*锁保护
 */
int set_irq_trigger_type(unsigned int irq, unsigned int type)
{
	struct irq_desc *desc = get_irq_desc(irq);
	unsigned long flags;
	int ret = 0;

	if (!desc)
		return -EINVAL;

	irq_controller_lock(desc);
	smp_lock_irqsave(&desc->lock, flags);

	type &= HWIRQ_TRIGGER_TYPE_MASK;
	ret = __set_irq_trigger_type(desc, irq, type);

	smp_unlock_irqrestore(&desc->lock, flags);
	irq_controller_unlock(desc);

	return ret;
}

void irq_enable(struct irq_desc *desc)
{
	irq_state_clear(desc, IRQSTATE_IRQ_DISABLED);
	if (desc->controller->enable)
		desc->controller->enable(desc);
	else
		desc->controller->unmask(desc);
	irq_state_clear(desc, IRQSTATE_IRQ_MASKED);
}

void irq_disable(struct irq_desc *desc)
{
	irq_state_set(desc, IRQSTATE_IRQ_DISABLED);
	if (desc->controller->disable) {
		desc->controller->disable(desc);
		irq_state_set(desc, IRQSTATE_IRQ_MASKED);
	}
}

int irq_startup(struct irq_desc *desc)
{
	int ret = 0;
	irq_state_clear(desc, IRQSTATE_IRQ_INPROGRESS);
	irq_state_clear(desc, IRQSTATE_IRQ_DISABLED);
	/**
	 * 调用控制器的初始化过程
	 */
	if (desc->controller->setup) {
		ret = desc->controller->setup(desc);
		irq_state_clear(desc, IRQSTATE_IRQ_MASKED);
	} else {
		irq_enable(desc);
	}

	return ret;
}

void irq_shutdown(struct irq_desc *desc)
{
	irq_state_set(desc, IRQSTATE_IRQ_DISABLED);
	if (desc->controller->shutdown)
		desc->controller->shutdown(desc);
	else if (desc->controller->disable)
		desc->controller->disable(desc);
	else
		desc->controller->mask(desc);
	irq_state_set(desc, IRQSTATE_IRQ_MASKED);
}

void enable_percpu_irq(unsigned int irq, unsigned int type)
{
	unsigned int cpu = smp_processor_id();
	unsigned long flags;
	struct irq_desc *desc = get_irq_desc(irq);

	if (!desc)
		return;

	/**
	 * percpu中断不需要锁控制器
	 */
	smp_lock_irqsave(&desc->lock, flags);

	/**
	 * 设置中断触发类型
	 */
	type &= HWIRQ_TRIGGER_TYPE_MASK;
	if (type != HWIRQ_TYPE_NONE) {
		int ret;

		ret = __set_irq_trigger_type(desc, irq, type);

		if (ret) {
			goto out;
		}
	}

	/**
	 * 打开中断
	 */
	if (desc->controller->enable)
		desc->controller->enable(desc);
	else
		desc->controller->unmask(desc);
	cpumask_set_cpu(cpu, desc->percpu_enabled);

out:
	smp_unlock_irqrestore(&desc->lock, flags);
}

void disable_percpu_irq(unsigned int irq)
{
	unsigned int cpu = smp_processor_id();
	unsigned long flags;
	struct irq_desc *desc = get_irq_desc(irq);

	if (!desc)
		return;

	/**
	 * percpu中断不需要锁控制器
	 */
	smp_lock_irqsave(&desc->lock, flags);

	/**
	 * 调用控制器回调关闭中断
	 */
	if (desc->controller->disable)
		desc->controller->disable(desc);
	else
		desc->controller->mask(desc);
	cpumask_clear_cpu(cpu, desc->percpu_enabled);

	smp_unlock_irqrestore(&desc->lock, flags);
}

void handle_bad_irq(unsigned int irq, struct irq_desc *desc)
{
	printk(KERN_CRIT "unexpected IRQ trap at vector %02x\n", irq);
}

/**
 * 处理percpu中断，如timer、IPI中断
 */
void handle_percpu_irq(unsigned int irq, struct irq_desc *desc)
{
	struct irq_controller *controller = desc->controller;
	struct irq_callback *cb;
	void *dev_id;

	BUG_ON(list_is_empty(&desc->isr_list));

	cb = list_first_container(&desc->isr_list, struct irq_callback, list);
	dev_id = this_cpu_var(cb->percpu_dev_id);

	if (controller->ack)
		controller->ack(desc);

	cb->handler(irq, dev_id);

	if (controller->eoi)
		controller->eoi(desc);
}

/**
 * 屏蔽/取消屏蔽中断
 */
static void mask_irq(struct irq_desc *desc)
{
	if (desc->controller->mask) {
		desc->controller->mask(desc);
		irq_state_set(desc, IRQSTATE_IRQ_MASKED);
	}
}

static void unmask_irq(struct irq_desc *desc)
{
	if (desc->controller->unmask) {
		desc->controller->unmask(desc);
		irq_state_clear(desc, IRQSTATE_IRQ_MASKED);
	}
}

/**
 * 中断处理完毕以后
 * 视情况取消屏蔽，并eoi应答
 */
static void cond_unmask_eoi_irq(struct irq_desc *desc, struct irq_controller *controller)
{
	if (!(desc->state & IRQS_ONESHOT)) {
		controller->eoi(desc);
		return;
	}

	if (!(desc->state & IRQSTATE_IRQ_DISABLED) &&
	    (desc->state & IRQSTATE_IRQ_MASKED)) {
		controller->eoi(desc);
		unmask_irq(desc);
	} else
		controller->eoi(desc);
}

void handle_fasteoi_irq(unsigned int irq, struct irq_desc *desc)
{
	struct irq_controller *controller = desc->controller;

	smp_lock(&desc->lock);

	/**
	 * 中断已经在处理中了
	 */
	if (desc->state & IRQSTATE_IRQ_INPROGRESS) {
		/**
		 * 设置挂起标志，等待其他核接着处理
		 * 然后执行eio应答
		 */
		desc->state |= IRQS_PENDING;
		goto out;
	}

	desc->state &= ~(IRQS_REPLAY | IRQS_WAITING);

	/**
	 * 如果没有注册ISR
	 * 或者中断被软件禁止了
	 */
	if (unlikely(list_is_empty(&desc->isr_list)
	    || (desc->state & IRQSTATE_IRQ_DISABLED))) {
	    	/**
	    	 * 设置挂起标志，等待打开中断的时候执行ISR
	    	 */
		desc->state |= IRQS_PENDING;
		mask_irq(desc);
		goto out;
	}

	if (desc->state & IRQS_ONESHOT)
		mask_irq(desc);

	/**
	 * 调用ISR中断处理回调
	 */
	call_isr(desc);

	/**
	 * 应答中断
	 */
	cond_unmask_eoi_irq(desc, controller);

	smp_unlock(&desc->lock);

	return;

out:
	controller->eoi(desc);
	smp_unlock(&desc->lock);
}
