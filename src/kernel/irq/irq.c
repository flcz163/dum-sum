#include <dim-sum/beehive.h>
#include <dim-sum/irq.h>
#include <dim-sum/irq_mapping.h>
#include <dim-sum/sched.h>
#include <dim-sum/smp.h>

#include "internals.h"

/**
 * 全局中断描述符表
 */
aligned_cacheline_in_smp
struct irq_desc all_irq_desc[NR_IRQS] = {
	[0 ... NR_IRQS-1] = {
		.handle	= handle_bad_irq,
		.lock		= SMP_LOCK_UNLOCKED(irq_desc->lock),
	}
};

/**
 * 中断/异常尾声处理
 */
void exception_tail(struct exception_spot *regs)
{
	/**
	 * 先处理抢占
	 */
	if (need_resched()) {
		unsigned long flags;

		local_irq_save(flags);
		preempt_in_irq();
		local_irq_restore(flags);
	}

	/**
	 * 再处理信号
	 */
	if (test_process_flag(PROCFLAG_SIGPENDING)
		 && (!current->in_syscall)) {/* 内核态OS!! */
		process_signal(regs);
	}

	if ((hardirq_count() == 0)
		 && (!current->in_syscall)) {
		current->user_regs = NULL;
	}
}

/**
 * 中断序言
 */
void irq_preface(int irq)
{
	add_preempt_count(HARDIRQ_OFFSET);
}

/**
 * 中断尾声
 */
void irq_tail(int irq, struct exception_spot *regs)
{
	disable_irq();
	/**
	 * 这里直接减计数，前面已经关中断了。
	 */
	sub_preempt_count(HARDIRQ_OFFSET);
	/**
	 * 取消软中断，改由高优先级工作队列处理
	 * 这里直接入理尾声工作了
	 */
	exception_tail(regs);
}

/**
 * 在确定了硬件中断的虚拟号以后
 * 调用此函数做软件方面的准备工作
 */
void prepare_one_irq(struct irq_mapping *map, bool is_percpu,
				unsigned int virq, unsigned int hw_irq, struct irq_controller *controller,
				void *controller_priv, irq_handler_f handler,
				void *handler_data, const char *handler_name)
{
	struct irq_desc *irq_desc = get_irq_desc(virq);
	unsigned long flags;

	if (is_percpu) {
		irq_desc->percpu_enabled =
			kzalloc(sizeof(*irq_desc->percpu_enabled), PAF_KERNEL);
		irq_desc->hwflag |= HWIRQ_PER_CPU;
	}

	irq_desc->hw_irq = hw_irq;
	irq_desc->controller = controller;
	irq_desc->controller_priv = controller_priv;

	if (!handler)
		handler = handle_bad_irq;

	/**
	 * 为什么要加锁??猜猜看^_^
	 */
	irq_controller_lock(irq_desc);
	smp_lock_irqsave(&irq_desc->lock, flags);

	irq_desc->handle = handler;
	irq_desc->name = handler_name;

	smp_unlock_irqrestore(&irq_desc->lock, flags);
	irq_controller_unlock(irq_desc);
}

/**
 * 同步等待一个中断的结束
 */
void synchronize_irq(unsigned int irq)
{
	struct irq_desc *desc = get_irq_desc(irq);
	bool inprogress;

	if (desc)
		do {
			unsigned long flags;

			/*
			 * 死循环判断，直到没有INPROGRESS标志
			 */
			while (desc->state & IRQSTATE_IRQ_INPROGRESS)
				/**
				 * why?!
				 */
				cpu_relax();

			/**
			 * 在前面的循环中，用屏障指令也无事无补
			 * 必须要在这里用锁这个**大杀器**进行二次检测
			 */
			smp_lock_irqsave(&desc->lock, flags);
			inprogress = desc->state & IRQSTATE_IRQ_INPROGRESS;
			smp_unlock_irqrestore(&desc->lock, flags);
		/**
		 * 如果在锁的保护下，发现中断也没有被执行
		 * 那么我们可以放心的说，这个中断真的没有执行了。
		 */
		} while (inprogress);
}

enum isr_result call_isr(struct irq_desc *desc)
{
	enum isr_result ret = ISR_SKIP;
	unsigned int irq = desc->virt_irq;
	struct double_list *entry;

	//在打开锁之前，需要标记这两个标志
	desc->state &= ~IRQS_PENDING;
	irq_state_set(desc, IRQSTATE_IRQ_INPROGRESS);
	//打开中断描述符的锁
	smp_unlock(&desc->lock);

	/**
	 * 遍历所有ISR
	 */
	list_for_each(entry, &desc->isr_list) {
		enum isr_result res;
		struct irq_callback* cb = container_of(entry, struct irq_callback, list);

		/**
		 * 调用注册的中断处理程序
		 */
		res = cb->handler(irq, cb->dev_id);

		/**
		 * 有问题的ISR，意外的改变了中断禁止标志
		 * 应当警告一下
		 */
		if (!irqs_disabled())
			disable_irq();

		ret |= res;
	}

	/**
	 * **再次**获得锁
	 * 并清除IRQD_IRQ_INPROGRESS标志
	 */
	smp_lock(&desc->lock);
	irq_state_clear(desc, IRQSTATE_IRQ_INPROGRESS);

	return ret;
}

unsigned long irq_err_count;
int do_hard_irq(struct irq_mapping *map, unsigned int hw_irq,
					struct exception_spot *regs)
{
	unsigned int irq;
	struct irq_desc *desc;
	int ret = 0;

	irq = get_virt_irq(map, hw_irq);

	/**
	 * 处理抢占计数，rcu等等
	 */
	irq_preface(irq);

	/*
	 * 错误的硬件号
	 */
	if (unlikely(irq < 0 || irq >= NR_IRQS)) {
		irq_err_count++;
		ret = -EINVAL;
	} else {
		/**
		 * 进行通常的中断处理
		 */
		desc = get_irq_desc(irq);
		/**
		 * 调用描述符中的回调，例如handle_fasteoi_irq.
		 * 通过__irq_set_handler设置回调函数
		 * 在这里面再调用ISR
		 */
		desc->handle(irq, desc);
	}

	/**
	 * 处理中断尾声
	 */
	irq_tail(irq, regs);

	return ret;
}
