#ifndef __DIM_SUM_TIME_H
#define __DIM_SUM_TIME_H

#include <dim-sum/types.h>
#include <uapi/linux/time.h>
#include <kapi/dim-sum/time.h>

typedef u64 stable_time_t;
struct super_block;

#define HZ CONFIG_HZ
extern u64 jiffies_64;
extern u32 jiffies;
extern struct timespec cur_time;
/**
 * jiffies比较，可以处理32位计数溢出。
 */
#define time_after(a,b)		\
	(typecheck(unsigned long, a) && \
	 typecheck(unsigned long, b) && \
	 ((long)(b) - (long)(a) < 0))
#define time_before(a,b)	time_after(b,a)
#define time_after_eq(a,b)	\
	(typecheck(unsigned long, a) && \
	 typecheck(unsigned long, b) && \
	 ((long)(a) - (long)(b) >= 0))
#define time_before_eq(a,b)	time_after_eq(b,a)

extern void add_jiffies_64(unsigned long ticks);
extern struct timespec
timespec_trunc(struct timespec ts, unsigned presision);
extern u64 uptime(void);
struct timespec current_kernel_time(void);

#define CURRENT_TIME (current_kernel_time())
#define CURRENT_TIME_SEC ((struct timespec) { cur_time.tv_sec, 0 })

extern struct timespec current_fs_time(struct super_block *sb);

static __inline__ int timespec_equal(struct timespec *a, struct timespec *b)
{ 
	return (a->tv_sec == b->tv_sec) && (a->tv_nsec == b->tv_nsec);
}

static inline unsigned long get_seconds(void)
{ 
	return cur_time.tv_sec;
}

#define NSEC_PER_USEC (1000L)
#define NSEC_PER_SEC (1000000000L)
#define TICK_NSEC (NSEC_PER_SEC / HZ)

static inline unsigned int jiffies_to_msecs(const unsigned long j)
{
#if HZ <= 1000 && !(1000 % HZ)
	return (1000 / HZ) * j;
#elif HZ > 1000 && !(HZ % 1000)
	return (j + (HZ / 1000) - 1)/(HZ / 1000);
#else
	return (j * 1000) / HZ;
#endif
}

static inline unsigned long msecs_to_jiffies(const unsigned int m)
{
#if HZ <= 1000 && !(1000 % HZ)
	return (m + (1000 / HZ) - 1) / (1000 / HZ);
#elif HZ > 1000 && !(HZ % 1000)
	return m * (HZ / 1000);
#else
	return (m * HZ + 999) / 1000;
#endif
}

unsigned long timespec_to_jiffies(const struct timespec *value);
void jiffies_to_timespec(const unsigned long jiffies, struct timespec *value);

#endif /* __DIM_SUM_TIME_H */
