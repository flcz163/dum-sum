#ifndef __LINUX_CPUMASK_H
#define __LINUX_CPUMASK_H

#include <dim-sum/bitmap.h>
#include <dim-sum/kernel.h>
#include <dim-sum/cpu.h>

typedef struct cpumask { DECLARE_BITMAP(bits, MAX_CPUS); } cpumask_t;

#define cpumask_bits(maskp) ((maskp)->bits)

extern const struct cpumask *const cpu_possible_mask;
extern const struct cpumask *const cpu_online_mask;
extern const struct cpumask *const cpu_present_mask;

#define num_online_cpus()	cpumask_weight(cpu_online_mask)
#define num_possible_cpus()	cpumask_weight(cpu_possible_mask)
#define num_present_cpus()	cpumask_weight(cpu_present_mask)
#define cpu_online(cpu)		cpumask_test_cpu((cpu), cpu_online_mask)
#define cpu_possible(cpu)	cpumask_test_cpu((cpu), cpu_possible_mask)
#define cpu_present(cpu)	cpumask_test_cpu((cpu), cpu_present_mask)

static inline unsigned int cpumask_check(unsigned int cpu)
{
	return cpu;
}

static inline unsigned int cpumask_first(const struct cpumask *srcp)
{
	return find_first_bit(cpumask_bits(srcp), MAX_CPUS);
}

static inline unsigned int cpumask_next(int n, const struct cpumask *srcp)
{
	if (n != -1)
		cpumask_check(n);
	return find_next_bit(cpumask_bits(srcp), MAX_CPUS, n+1);
}

#define for_each_cpu(cpu, mask)				\
	for ((cpu) = -1;				\
		(cpu) = cpumask_next((cpu), (mask)),	\
		(cpu) < nr_existent_cpus;)


#define CPU_BITS_NONE						\
{								\
	[0 ... BITS_TO_LONGS(MAX_CPUS)-1] = 0UL			\
}

#define CPU_BITS_CPU0						\
{								\
	[0] =  1UL						\
}

static inline void cpumask_set_cpu(unsigned int cpu, struct cpumask *dstp)
{
	atomic_set_bit(cpumask_check(cpu), cpumask_bits(dstp));
}

static inline void cpumask_clear_cpu(int cpu, struct cpumask *dstp)
{
	atomic_clear_bit(cpumask_check(cpu), cpumask_bits(dstp));
}

#define cpumask_test_cpu(cpu, cpumask) \
	test_bit(cpumask_check(cpu), cpumask_bits((cpumask)))

static inline unsigned int cpumask_weight(const struct cpumask *srcp)
{
	return bitmap_weight(cpumask_bits(srcp), MAX_CPUS);
}

static inline void cpumask_copy(struct cpumask *dstp,
				const struct cpumask *srcp)
{
	bitmap_copy(cpumask_bits(dstp), cpumask_bits(srcp), MAX_CPUS);
}

#define for_each_possible_cpu(cpu) for_each_cpu((cpu), cpu_possible_mask)
#define for_each_online_cpu(cpu)   for_each_cpu((cpu), cpu_online_mask)
#define for_each_present_cpu(cpu)  for_each_cpu((cpu), cpu_present_mask)

void mark_cpu_possible(unsigned int cpu, bool possible);
void mark_cpu_present(unsigned int cpu, bool present);
void mark_cpu_online(unsigned int cpu, bool online);
void init_cpu_present(const struct cpumask *src);
void init_cpu_possible(const struct cpumask *src);
void init_cpu_online(const struct cpumask *src);

#define to_cpumask(bitmap)						\
	((struct cpumask *)(1 ? (bitmap)				\
			    : (void *)sizeof(__check_is_bitmap(bitmap))))

static inline int __check_is_bitmap(const unsigned long *bitmap)
{
	return 1;
}

#define cpu_is_offline(cpu)	unlikely(!cpu_online(cpu))

#if MAX_CPUS <= BITS_PER_LONG
#define CPU_BITS_ALL						\
{								\
	[BITS_TO_LONGS(MAX_CPUS)-1] = CPU_MASK_LAST_WORD	\
}

#else /* MAX_CPUS > BITS_PER_LONG */

#define CPU_BITS_ALL						\
{								\
	[0 ... BITS_TO_LONGS(MAX_CPUS)-2] = ~0UL,		\
	[BITS_TO_LONGS(MAX_CPUS)-1] = CPU_MASK_LAST_WORD		\
}
#endif /* MAX_CPUS > BITS_PER_LONG */

#define cpu_possible_map	(*(cpumask_t *)cpu_possible_mask)
#define cpu_online_map		(*(cpumask_t *)cpu_online_mask)
#define cpu_present_map		(*(cpumask_t *)cpu_present_mask)

#define CPU_MASK_LAST_WORD BITMAP_LAST_WORD_MASK(MAX_CPUS)

#endif /* __LINUX_CPUMASK_H */
