#include <dim-sum/smp_seq_lock.h>
#include <dim-sum/string.h>
#include <dim-sum/time.h>

#include <asm/div64.h>

u64 jiffies_64 = 0;

/**
 * 存放当前时间和日期
 */
struct timespec cur_time;
static struct smp_seq_lock time_lock = SMP_SEQ_LOCK_UNLOCKED(time_lock);

static inline unsigned int jiffies_to_usecs(const unsigned long j)
{
	return (1000000 / HZ) * j;
}

struct timespec timespec_trunc(struct timespec ts, unsigned presision)
{
	if (presision <= jiffies_to_usecs(1) * 1000) {
		/* nothing */
	} else if (presision == NSEC_PER_SEC) {
		ts.tv_nsec = 0;
	} else {
		ts.tv_nsec -= ts.tv_nsec % presision;
	}

	return ts;
}

void add_jiffies_64(unsigned long ticks)
{
	unsigned long flags;

	local_irq_save(flags);
	smp_seq_write_lock(&time_lock);
	jiffies_64 += ticks;
	smp_seq_write_unlock(&time_lock);
	local_irq_restore(flags);
}

u64 get_jiffies_64(void)
{
	unsigned long seq;
	u64 ret;

	do {
		seq = smp_seq_read_begin(&time_lock);
		ret = jiffies_64;
	} while (smp_seq_read_retry(&time_lock, seq));

	return ret;
}

u64 uptime(void)
{
	return get_jiffies_64() * NSEC_PER_SEC / HZ;
}

inline struct timespec current_kernel_time(void)
{
        struct timespec now;
        unsigned long seq;

	do {
		seq = smp_seq_read_begin(&time_lock);
		now = cur_time;
	} while (smp_seq_read_retry(&time_lock, seq));

	return now; 
}

unsigned long timespec_to_jiffies(const struct timespec *value)
{
	unsigned long sec = value->tv_sec;
	long nsec = value->tv_nsec + TICK_NSEC - 1;

	return sec * 1000 / HZ + nsec / TICK_NSEC;
}

void jiffies_to_timespec(const unsigned long jiffies, struct timespec *value)
{
	u64 nsec = (u64)jiffies * TICK_NSEC;
	value->tv_sec = div_long_long_rem(nsec, NSEC_PER_SEC,
		&value->tv_nsec);
}
