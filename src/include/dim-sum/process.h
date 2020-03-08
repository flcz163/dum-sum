#ifndef __DIM_SUM_THREAD_INFO_H
#define __DIM_SUM_THREAD_INFO_H

#include <dim-sum/bitops.h>
#include <dim-sum/bug.h>

#include <asm/process.h>

#ifdef __KERNEL__

enum {
	/**
	 * 有更高优先级的任务被唤醒
	 * 线程将在合适的时候被抢占
	 */
	PROCFLAG_NEED_RESCHED,
	/**
	 * 由于OOM的原因，任务正在被kill
	 * 此时应当允许任务分配更多的内存
	 */
	PROCFLAG_OOM_KILLED,
	/**
	 * 有信号待处理
	 */
	PROCFLAG_SIGPENDING,
	/**
	 * 是否正在单步调试
	 */
	PROCFLAG_SINGLESTEP,
	/**
	 * 标准的标志数量
	 * 体系结构定义的标志应当从此开始
	 */
	PROCFLAG_COUNT,
};

struct process_desc {
	/**
	 * 线程标志 ，如PROCFLAG_NEED_RESCHED
	 */
        unsigned long           flags;
	/**
	 * 抢占计数
	 */
       int                     preempt_count;
	/**
	 * 线程描述符结构
	 */
	struct task_desc	*task;
	/**
	 * 所在CPU
	 */
	int			cpu;
	/**
	 * 体系结构特定的描述符
	 */
	struct arch_process_desc arch_desc;
};

/**
 * 进程描述符及其堆栈
 */
union process_union {
	struct process_desc process_desc;
	unsigned long stack[PROCESS_STACK_SIZE/sizeof(long)];
};

static inline void
__set_process_flag(struct process_desc *proc, int flag)
{
	atomic_set_bit(flag, &proc->flags);
}

static inline void
__clear_process_flag(struct process_desc *proc, int flag)
{
	atomic_clear_bit(flag, &proc->flags);
}

static inline int
__test_and_set_process_flag(struct process_desc *proc, int flag)
{
	return atomic_test_and_set_bit(flag, &proc->flags);
}

static inline int
__test_and_clear_process_flag(struct process_desc *proc, int flag)
{
	return atomic_test_and_clear_bit(flag, &proc->flags);
}

static inline int
__test_process_flag(struct process_desc *proc, int flag)
{
	return test_bit(flag, &proc->flags);
}

#define set_process_flag(flag) \
	__set_process_flag(current_proc_info(), flag)
#define clear_process_flag(flag) \
	__clear_process_flag(current_proc_info(), flag)
#define test_and_set_process_flag(flag) \
	__test_and_set_process_flag(current_proc_info(), flag)
#define test_and_clear_process_flag(flag) \
	__test_and_clear_process_flag(current_proc_info(), flag)
#define test_process_flag(flag) \
	__test_process_flag(current_proc_info(), flag)

#endif	/* __KERNEL__ */

#endif /* __DIM_SUM_THREAD_INFO_H */
