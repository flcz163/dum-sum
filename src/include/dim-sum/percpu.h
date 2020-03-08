#ifndef __DIM_SUM_PERCPU_H
#define __DIM_SUM_PERCPU_H

#include <dim-sum/cpumask.h>

#include <dim-sum/preempt.h>
#include <dim-sum/mm.h>
#include <dim-sum/smp.h>
#include <dim-sum/init.h>

#define __PCPU_ATTRS(sec)						\
	__percpu __attribute__((section(PER_CPU_BASE_SECTION sec)))	\
	PER_CPU_ATTRIBUTES

#define DECLARE_PER_CPU_SECTION(type, name, sec)			\
	extern __PCPU_ATTRS(sec) __typeof__(type) name

#define DEFINE_PER_CPU_SECTION(type, name, sec)				\
	__PCPU_ATTRS(sec) PER_CPU_DEF_ATTRIBUTES			\
	__typeof__(type) name

#define DECLARE_PER_CPU(type, name)					\
	DECLARE_PER_CPU_SECTION(type, name, "")

#define DEFINE_PER_CPU(type, name)					\
	DEFINE_PER_CPU_SECTION(type, name, "")

#define __verify_pcpu_ptr(ptr)	do {					\
	const void __percpu *__vpp_verify = (typeof((ptr) + 0))NULL;	\
	(void)__vpp_verify;						\
} while (0)

#include <asm/percpu.h>

/**
 * 先禁用内核抢占，然后在每CPU数组name中，为本地CPU选择元素。
 */
#define hold_percpu_var(var) (*({ 				\
	preempt_disable(); 					\
	&__get_cpu_var(var); }))

/**
 * 启用内核抢占（name参数无用）
 */
#define loosen_percpu_var(var) do {				\
	(void)&(var);					\
	preempt_enable();				\
} while (0)

struct dynamic_percpu {
	void *ptrs[MAX_CPUS];
	void *data;
};

/**
 * 返回每CPU数组中，与参数cpu对应的cpu元素地址，参数pointer给出数组的地址。
 */
#define __percpu_ptr(ptr, cpu)                \
({                                              \
        struct dynamic_percpu *__p = 		\
        		(struct dynamic_percpu *)~(unsigned long)(ptr); \
        (__typeof__(ptr))__p->ptrs[(cpu)];	\
})

#define hold_percpu_ptr(var) ({				\
	int cpu = get_cpu();				\
	__percpu_ptr(var, cpu); })

#define loosen_percpu_ptr(var) do {				\
	(void)(var);					\
	put_cpu();				\
} while (0)


extern void *__alloc_percpu(size_t size, size_t align);
extern void free_percpu(const void *);

#define alloc_percpu(type)	\
	(typeof(type) __percpu *)__alloc_percpu(sizeof(type), __alignof__(type))

#endif /* __DIM_SUM_PERCPU_H */
