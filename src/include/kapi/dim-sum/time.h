#ifndef __KAPI_DIM_SUM_TIME_H
#define __KAPI_DIM_SUM_TIME_H

#include <linux/types.h>

extern unsigned long long get_jiffies_64(void);

#ifndef _STRUCT_TIMESPEC
#define _STRUCT_TIMESPEC
struct timespec {
	__kernel_time_t	tv_sec;			/* seconds */
	long		tv_nsec;		/* nanoseconds */
};
#endif

#endif /* __KAPI_DIM_SUM_TIME_H */
