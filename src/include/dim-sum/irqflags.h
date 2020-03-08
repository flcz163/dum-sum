#ifndef __DIM_SUM_IRQFLAGS_H
#define __DIM_SUM_IRQFLAGS_H

#include <dim-sum/typecheck.h>
#include <asm/irqflags.h>

static inline int arch_irqs_disabled(void)
{
	return arch_irqs_disabled_flags(arch_local_save_flags());
}

#define disable_irq()		arch_local_irq_disable()
#define enable_irq()		arch_local_irq_enable()
#define local_irq_save(flags)			\
	do {						\
		typecheck(unsigned long, flags);	\
		flags = arch_local_irq_save();		\
	} while (0)
#define local_irq_restore(flags)			\
	do {						\
		typecheck(unsigned long, flags);	\
		arch_local_irq_restore(flags);		\
	} while (0)
#define local_save_flags(flags)			\
	do {						\
		typecheck(unsigned long, flags);	\
		flags = arch_local_save_flags();	\
	} while (0)
#define irqs_disabled_flags(flags)			\
	({						\
		typecheck(unsigned long, flags);	\
		arch_irqs_disabled_flags(flags);	\
	})
#define irqs_disabled()		(arch_irqs_disabled())

#endif /* __DIM_SUM_IRQFLAGS_H */
