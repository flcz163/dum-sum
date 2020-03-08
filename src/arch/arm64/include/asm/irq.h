#ifndef __ASM_ARM_IRQ_H
#define __ASM_ARM_IRQ_H

#include <asm/exception.h>

extern void set_chip_irq_handle(void (*handle_irq)(struct exception_spot *));

#endif