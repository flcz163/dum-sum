#ifndef __DIM_SUM_SMP_H
#define __DIM_SUM_SMP_H

#include <dim-sum/errno.h>
#include <dim-sum/init.h>
#include <dim-sum/types.h>
#include <dim-sum/double_list.h>
#include <dim-sum/cpumask.h>
#include <dim-sum/preempt.h>
#include <dim-sum/kernel.h>
#include <dim-sum/process.h>
#include <asm/smp.h>

/**
 * IPI命令类型
 */
enum ipi_cmd_type {
	/**
	 * 要求其他核触发一次调度
	 * 可能是需要产生一次静止状态
	 * 也可能是有高优先级线程需要抢占
	 */
	IPI_RESCHEDULE,
	/**
	 * 要求其他核执行一些特定操作
	 * 例如刷新TLB
	 */
	IPI_CALL_FUNC,
};

struct smp_call_data {
	struct smp_lock lock;
	void (*func) (void *info);
	void *priv;
	u16 flags;
};

int smp_call_function(void(*func)(void *info), void *info, int wait);
int smp_call_for_all(void (*func) (void *info), void *info, int wait);

#define get_cpu()		({ preempt_disable(); smp_processor_id(); })
#define put_cpu()		preempt_enable()

extern void slave_cpu_entry(void);

extern void __init launch_slave(void);

#endif /* __DIM_SUM_SMP_H */
