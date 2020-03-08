/*
 * Copyright (C) 2012 Thomas Petazzoni
 *
 * Thomas Petazzoni <thomas.petazzoni@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <dim-sum/init.h>
#include <dim-sum/irq_controller.h>

extern void gic_v3_init(void);


void __init init_irq_controller(void)
{
#if CONFIG_QEMU
	gic_v2_init();
#else
	gic_v3_init();
#endif
}
