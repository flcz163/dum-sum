#ifndef __DIM_SUM_DELAY_H
#define __DIM_SUM_DELAY_H

#include <dim-sum/kernel.h>

extern unsigned long loops_per_second;

#include <asm/delay.h>

#ifndef mdelay
#define mdelay(n) \
	({unsigned long __ms=(n); while (__ms--) udelay(1000);})
#endif

#ifndef ndelay
static inline void ndelay(unsigned long x)
{
	udelay(DIV_ROUND_UP(x, 1000));
}
#define ndelay(x) ndelay(x)
#endif

void msleep(unsigned int msecs);
unsigned long msleep_interruptible(unsigned int msecs);

static inline void ssleep(unsigned int seconds)
{
	msleep(seconds * 1000);
}

#endif /* __DIM_SUM_DELAY_H */
