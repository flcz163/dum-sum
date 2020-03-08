#ifndef __ASM_THREAD_INFO_H
#define __ASM_THREAD_INFO_H

#ifdef __KERNEL__

#define PROCESS_STACK_SIZE 8192

#ifndef __ASSEMBLY__

#include <linux/compiler.h>

struct process_desc;

/**
 * 体系结构特定的，任务描述符
 * 将被放入通用结构process_desc中
 */
struct arch_process_desc {
};

static inline struct process_desc *current_proc_info(void)
{
        register unsigned long sp asm ("sp");
        return (struct process_desc *)(sp & ~(PROCESS_STACK_SIZE - 1));
}

register unsigned long current_stack_pointer asm ("sp");

#define thread_saved_pc(tsk)	\
	((unsigned long)(tsk->task_spot.cpu_context.pc))
#define thread_saved_sp(tsk)	\
	((unsigned long)(tsk->task_spot.cpu_context.sp))
#define thread_saved_fp(tsk)	\
	((unsigned long)(tsk->task_spot.cpu_context.fp))

#endif /* __ASSEMBLY__ */
#endif /* __KERNEL__ */
#endif /* __ASM_THREAD_INFO_H */
