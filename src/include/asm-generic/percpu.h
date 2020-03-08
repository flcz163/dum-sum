#ifndef __ASM_GENERIC_PERCPU_H_
#define __ASM_GENERIC_PERCPU_H_

#include <linux/compiler.h>
#include <dim-sum/cpu.h>

#ifndef PER_CPU_BASE_SECTION
#define PER_CPU_BASE_SECTION ".data..percpu"
#endif

#ifndef PER_CPU_ATTRIBUTES
#define PER_CPU_ATTRIBUTES
#endif

#ifndef PER_CPU_DEF_ATTRIBUTES
#define PER_CPU_DEF_ATTRIBUTES
#endif

#ifndef SHIFT_PERCPU_PTR
/* Weird cast keeps both GCC and sparse happy. */
#define SHIFT_PERCPU_PTR(__p, __offset)	({				\
	__verify_pcpu_ptr((__p));					\
	RELOC_HIDE((typeof(*(__p)) __kernel __force *)(__p), (__offset)); \
})
#endif

#ifndef arch_raw_cpu_ptr
#define arch_raw_cpu_ptr(ptr) SHIFT_PERCPU_PTR(ptr, this_cpu_offset)
#endif

#define this_cpu_var(ptr)						\
({									\
	__verify_pcpu_ptr(ptr);						\
	arch_raw_cpu_ptr(ptr);						\
})

#define __get_cpu_var(var) (*this_cpu_var(&(var)))

#ifndef per_cpu_var_offsets
/**
 * per-cpu变量在所有CPU上面的偏移值
 */
extern unsigned long per_cpu_var_offsets[MAX_CPUS];
#define per_cpu_offset(x) (per_cpu_var_offsets[x])
#endif

#define per_cpu_var(var, cpu) \
	(*SHIFT_PERCPU_PTR(&(var), per_cpu_offset(cpu)))
	
#endif /* __ASM_GENERIC_PERCPU_H_ */
