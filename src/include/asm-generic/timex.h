#ifndef __ASM_GENERIC_TIMEX_H
#define __ASM_GENERIC_TIMEX_H

typedef unsigned long cycles_t;
#ifndef get_cycles
#warning "have not define get_cycles yet, are you sure?"
static inline cycles_t get_cycles(void)
{
	return 0;
}
#endif

#endif /* __ASM_GENERIC_TIMEX_H */
