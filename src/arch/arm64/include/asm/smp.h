#ifndef __ASM_SMP_H

#define __ASM_SMP_H

#ifndef __ASSEMBLY__
#include <dim-sum/process.h>

struct cpumask;
enum ipi_cmd_type;

#define smp_processor_id() (current_proc_info()->cpu)

/**
 * 用于触发IPI的函数
 * 由硬件驱动设置
 */
extern void (*__smp_raise_ipi)(const struct cpumask *, unsigned int);
typedef void (*raise_ipi_call_fn)(const struct cpumask *, unsigned int);
/**
 * 硬件调用此函数，设置触IPI的函数指针
 */
extern void smp_set_raise_ipi_call(raise_ipi_call_fn fn);

/**
 * 向其他核发送IPI中断
 * 体系架构相关
 */
extern void arch_raise_ipi(const struct cpumask *mask, enum ipi_cmd_type cmd_type);

/**
 * IPI中断处理函数
 */
extern void do_IPI(int ipinr, struct exception_spot *regs);

#endif /* __ASSEMBLY__ */

#endif /* __ASM_SMP_H */
