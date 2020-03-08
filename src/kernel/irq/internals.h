
enum {
	__IRQS_PENDING = __IRQSTATE_COUNT,
	__IRQS_ONESHOT,
	__IRQS_REPLAY,
	__IRQS_WAITING,
};

#define IRQS_PENDING			(1UL << __IRQS_PENDING)
#define IRQS_ONESHOT			(1UL << __IRQS_ONESHOT)
#define IRQS_REPLAY				(1UL << __IRQS_REPLAY)
#define IRQS_WAITING			(1UL << __IRQS_WAITING)

static inline void irq_controller_lock(struct irq_desc *desc)
{
	if (unlikely(desc->controller->lock))
		desc->controller->lock(desc);
}

static inline void irq_controller_unlock(struct irq_desc *desc)
{
	if (unlikely(desc->controller->unlock))
		desc->controller->unlock(desc);
}

int __set_irq_trigger_type(struct irq_desc *desc, unsigned int irq,
		      unsigned long flags);
int set_irq_trigger_type(unsigned int irq, unsigned int type);

int irq_startup(struct irq_desc *desc);
void irq_shutdown(struct irq_desc *desc);

/**
 * 调用驱动注册的ISR
 */
extern enum isr_result call_isr(struct irq_desc *desc);
