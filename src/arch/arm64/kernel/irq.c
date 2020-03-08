#include <dim-sum/init.h>
#include <dim-sum/stddef.h>

#include <asm/irq.h>

void (*handle_arch_irq)(struct exception_spot *) = NULL;

/**
 * 设置中断处理函数
 * 如gic_handle_irq
 */
void __init set_chip_irq_handle(void (*handle_irq)(struct exception_spot *))
{
	if (handle_arch_irq)
		return;

	handle_arch_irq = handle_irq;
}
