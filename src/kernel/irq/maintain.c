#include <dim-sum/beehive.h>
#include <dim-sum/irq.h>
#include <dim-sum/sched.h>
#include <dim-sum/smp.h>

#include "internals.h"

/**
 * 将中断回调函数插入到中断描述符链表
 * 并执行一些硬件相关的初始化
 */
static int __register_irq_callback(unsigned int irq,
	struct irq_desc *desc, struct irq_callback *new)
{
	int ret, shared = 0;

	if (!desc || !desc->controller)
		return -EINVAL;

	if (list_is_empty(&desc->isr_list)) {
		desc->share_state = (new->flags & IRQFLAG_SHARED) ?
			IRQ_SHARE_ALLOWED : IRQ_SHARE_DISALLOWED;
		desc->share_flag = new->flags;
	} else {	/* 存在旧的ISR */
		/* 新旧ISR没有打开共享 */
		if (!((desc->share_flag & new->flags) & IRQFLAG_SHARED) ||
			/* 中断类型不一样 */
		    ((desc->share_flag ^ new->flags) & IRQFLAG_TRIGGER_MASK))
			goto mismatch;

		if ((desc->share_flag & IRQFLAG_PERCPU) !=
		    (new->flags & IRQFLAG_PERCPU))
			goto mismatch;

		shared = 1;
	}

	/**
	 * 第一个注册的ISR，做一些必要的初始化
	 */
	if (!shared) {
		/**
		 * 设置触发器类型
		 */
		if (new->flags & IRQFLAG_TRIGGER_MASK) {
			/**
			 * __!
			 */
			ret = __set_irq_trigger_type(desc, irq,
						new->flags & IRQFLAG_TRIGGER_MASK);
			if (ret)
				return ret;
		}

		if (new->flags & IRQFLAG_PERCPU)
			desc->hwflag |= HWIRQ_PER_CPU;

		ret = irq_startup(desc);
		if (ret)
			return ret;
	}

	/**
	 * 将ISR链接到链表尾部
	 * 应当放在后面
	 * 并且在其他初始化完成后再插入链表
	 */
	list_insert_behind(&new->list, &desc->isr_list);

	return 0;

mismatch:
	pr_err("Flags mismatch irq %d. %08x (%s) vs. %08x\n",
		       irq, new->flags, new->name, desc->share_flag);
	ret = -EBUSY;
	return ret;
}

int register_percpu_irq_handle(unsigned int irq, isr_handler_f handler,
		       const char *devname, void __percpu *dev_id)
{
	struct irq_desc *desc = get_irq_desc(irq);
	struct irq_callback *cb;
	unsigned long flags;
	int retval;

	if (!dev_id || !desc)
		return -EINVAL;

	cb = kzalloc(sizeof(struct irq_callback), PAF_KERNEL);
	if (!cb)
		return -ENOMEM;

	cb->handler = handler;
	cb->flags = IRQFLAG_PERCPU;
	cb->name = devname;
	cb->percpu_dev_id = dev_id;

	/**
	 * 获得控制器和中断描述符的锁
	 */
	irq_controller_lock(desc);
	smp_lock_irqsave(&desc->lock, flags);
	/**
	 * 执行真正的初始化操作，无锁!!
	 */
	retval = __register_irq_callback(irq, desc, cb);
	smp_unlock_irqrestore(&desc->lock, flags);
	irq_controller_unlock(desc);

	if (retval)
		kfree(cb);

	return retval;
}

void unregister_percpu_irq_handle(unsigned int irq, void __percpu *dev_id)
{
	/* TO-DO */
}

/**
 * 注册ISR
 */
int register_isr_handle(unsigned int irq, isr_handler_f handler,
	unsigned long irqflags, const char *devname, void *dev_id)
{
	struct irq_desc *desc = get_irq_desc(irq);
	struct irq_callback *cb;
	unsigned long flags;
	int retval;

	/**
	 * 对于共享中断来说，需要dev_id来进行区分
	 */
	if ((irqflags & IRQFLAG_SHARED) && !dev_id)
		return -EINVAL;

	if (!handler || !desc || (desc->hwflag & HWIRQ_DISALBE_ISR))
		return -EINVAL;

	cb = kzalloc(sizeof(struct irq_callback), PAF_KERNEL);
	if (!cb)
		return -ENOMEM;

	cb->handler = handler;
	cb->flags = irqflags;
	cb->name = devname;
	cb->dev_id = dev_id;

	irq_controller_lock(desc);
	smp_lock_irqsave(&desc->lock, flags);
	retval = __register_irq_callback(irq, desc, cb);
	smp_unlock_irqrestore(&desc->lock, flags);
	irq_controller_unlock(desc);

	if (retval)
		kfree(cb);

	return retval;
}

/**
 * 取消注册ISR
 */
void unregister_isr_handle(unsigned int irq, void *dev_id)
{
	struct irq_desc *desc = get_irq_desc(irq);
	unsigned long flags;
	struct double_list * entry;
	struct irq_callback* cb_found = NULL;

	/**
	 * 安全检查
	 *	1、描述符不能为 NULL
	 	2、不能是percpu中断
	 */
	if (!desc || (desc->hwflag & HWIRQ_PER_CPU))
		return;

	/**
	 * 1、在中断上下文调用此函数会死锁
	 * 	    此时中断描述符的锁可能已经被持有了
	 * 2、并且控制器的锁一般是睡眠锁
	 */
	if (in_interrupt()) {
		WARN("Trying to free IRQ %d from IRQ context!\n", irq);
		return;
	}

	/**
	 * 先锁住控制器!!!
	 */
	irq_controller_lock(desc);
	smp_lock_irqsave(&desc->lock, flags);

	/**
	 * 在ISR链表中遍历，找到要删除的项
	 */
	list_for_each(entry, &desc->isr_list) {
		struct irq_callback* cb = container_of(entry, struct irq_callback, list);

		if (cb->dev_id == dev_id) {
			cb_found = cb;
			list_del(&cb->list);
			break;
		}
	}

	if (!cb_found) {
		WARN(1, "Free IRQ %d repeatedly\n", irq);
		smp_unlock_irqrestore(&desc->lock, flags);
		irq_controller_unlock(desc);

		return;
	}

	if (list_is_empty(&desc->isr_list)) {
		desc->share_state = IRQ_SHARE_EMPTY;
		desc->share_flag = 0;
		irq_shutdown(desc);
	}

	/**
	 * 运行至此，已经从链表上将ISR摘除
	 * 但是ISR可能还在多核上并发运行
	 */
	smp_unlock_irqrestore(&desc->lock, flags);

	/**
	 * 确保在多核下面，ISR真的执行完毕了。
	 */
	synchronize_irq(irq);

	/**
	 * 释放控制器的锁
	 */
	irq_controller_unlock(desc);

	/**
	 * 现在可以完全释放ISR对象了
	 */
	kfree(cb_found);
}
