#ifndef __ASM_TIMEX_H
#define __ASM_TIMEX_H

#include <asm/arch_timer.h>

/**
 * 获取时间戳
 */
#define get_cycles()	arch_counter_get_cntvct()

#include <asm-generic/timex.h>

#endif
