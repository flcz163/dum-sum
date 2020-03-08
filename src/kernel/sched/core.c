#include <dim-sum/beehive.h>
#include <dim-sum/boot_allotter.h>
#include <dim-sum/cpu.h>
#include <dim-sum/fs.h>
#include <dim-sum/idle.h>
#include <dim-sum/irq.h>
#include <dim-sum/mem.h>
#include <dim-sum/sched.h>
#include <dim-sum/smp.h>
#include <dim-sum/string.h>
#include <dim-sum/stacktrace.h>
#include <dim-sum/syscall.h>
#include <dim-sum/timer.h>
#include <dim-sum/wait.h>
#include <kapi/dim-sum/task.h>

#include <asm/asm-offsets.h>
#include <asm/signal.h>

#include "internal.h"

/**
 * 主CPU的idle线程堆栈
 * 使用静态分配的目的，是为了提供给CPU初始化过程使用
 */
__attribute__((aligned(PROCESS_STACK_SIZE))) 
union process_union master_idle_stack;

/**
 * 注意，是MAX_RT_PRIO + BITS_PER_LONG
 * 而不是MAX_RT_PRIO + BITS_PER_LONG **-1**
 * 这样最后总有一位是0
 */
#define SCHED_MASK_SIZE ((MAX_RT_PRIO + BITS_PER_LONG) / BITS_PER_LONG)
unsigned long sched_runqueue_mask[SCHED_MASK_SIZE] = { 0 };
struct double_list sched_runqueue_list[MAX_RT_PRIO+1];

union process_union *idle_proc_stacks[MAX_CPUS];
struct task_desc *idle_task_desc[MAX_CPUS];
#define INIT_SP(tsk)		((unsigned long)tsk + THREAD_START_SP)

struct smp_lock lock_all_task_list = SMP_LOCK_UNLOCKED(lock_all_task_list);
struct double_list sched_all_task_list = LIST_HEAD_INITIALIZER(sched_all_task_list);

/**
 * 分配/释放任务堆栈
 * 后期可以考虑使用对象池
 */
static void* alloc_proc_stack(void)
{
	return (void *)alloc_pages_memory(PAF_KERNEL, 
					get_order(PROCESS_STACK_SIZE));
}

static void free_proc_stack(void* addr)
{
	free_pages_memory((unsigned long)addr, get_order(PROCESS_STACK_SIZE));
}

/**
 * 释放任务结构
 * 在任务的引用计数为0时调用
 */
void __release_task_desc(struct ref_count *ref)
{
	struct task_desc *tsk = container_of(ref, struct task_desc, ref);

	list_del_init(&tsk->all_list);
	free_proc_stack(tsk->stack);
}

static inline void add_to_runqueue(struct task_desc * p)
{
 	int pri = p->sched_prio;

	if (p->in_run_queue)
		return;

	list_insert_behind(&p->run_list,&(sched_runqueue_list[pri])); 
	p->in_run_queue = 1;
	if (p->sched_prio < current->sched_prio)
		set_task_need_resched(current);

	atomic_set_bit(pri, ( long unsigned int *)sched_runqueue_mask);  
}

static inline void del_from_runqueue(struct task_desc * p)
{
	int pri = p->sched_prio;

	if (!p->in_run_queue)
		return;

	list_del(&p->run_list);
	list_init(&p->run_list);

	if(list_is_empty(&(sched_runqueue_list[pri])))
	    atomic_clear_bit(pri, ( long unsigned int *)sched_runqueue_mask);

	p->in_run_queue = 0;
}

asmlinkage void __sched preempt_schedule(void)
{
	if (likely(preempt_count() || irqs_disabled()))
		return;
	
need_resched:
	/**
	 * 如果不添加PREEMPT_ACTIVE标志，会出现什么情况?
	 */
	add_preempt_count(PREEMPT_ACTIVE);
	schedule();
	sub_preempt_count(PREEMPT_ACTIVE);

	barrier();
	if (unlikely(test_process_flag(PROCFLAG_NEED_RESCHED)))
		goto need_resched;
}

asmlinkage void __sched preempt_in_irq(void)
{
	if (likely(preempt_count() || !irqs_disabled()))
		return;

need_resched:
	add_preempt_count(PREEMPT_ACTIVE);
	/**
	 * 这里不需要加内存屏障
	 */
	enable_irq();
	schedule();
	/**
	 * 在退回到中断汇编处理前，这里应当关闭中断
	 * 否则可能将堆栈击穿
	 */
	disable_irq();
	sub_preempt_count(PREEMPT_ACTIVE);

	barrier();
	if (unlikely(test_process_flag(PROCFLAG_NEED_RESCHED)))
		goto need_resched;
}

static struct task_desc *__switch_to(struct process_desc *prev,
	struct process_desc *new)
{
	struct task_desc *last;

	last = __switch_cpu_context(prev->task, new->task);

	return last;
}

asmlinkage void schedule(void)
{
	struct task_desc *prev, *next;
	unsigned long flags;
	int idx;

	if (irqs_disabled() || (preempt_count() & ~PREEMPT_ACTIVE)) {
		printk("cannt switch task, preempt count is %lx, irq %s.\n",
			preempt_count(), irqs_disabled() ? "disabled" : "enabled");
		BUG();
	}

need_resched:
	/**
	 * 这里必须手动增加抢占计数
	 * 以避免在打开锁的时候执行调度，那样就乱套了
	 */
	preempt_disable();
	smp_lock_irqsave(&lock_all_task_list, flags);

	prev = current;
	/**
	 * 很微妙的两个标志，请特别小心
	 */
	if (!(preempt_count() & PREEMPT_ACTIVE) && !(prev->state & TASK_RUNNING))
		del_from_runqueue(prev);

	idx = find_first_bit(sched_runqueue_mask, MAX_RT_PRIO + 1);
	/**
	 * 没有任务可运行
	 */
	if (idx > MAX_RT_PRIO)
		/**
		 * 选择本CPU上的IDLE任务来运行
		 */
		next = idle_task_desc[smp_processor_id()];
	else
		next = list_first_container(&sched_runqueue_list[idx],
							struct task_desc, run_list);

	/**
	 * 什么情况下，二者会相等??
	 */
	if (unlikely(prev == next)) {
		clear_task_need_resched(prev);
		smp_unlock_irq(&lock_all_task_list);
		preempt_enable_no_resched();
		goto same_process;
	}
	clear_task_need_resched(prev);

	next->prev_sched = prev;
	task_process_info(next)->cpu = task_process_info(prev)->cpu;
	prev = __switch_to(task_process_info(prev), task_process_info(next)); 
	barrier();
	/**
	 * 只能在切换后，释放上一个进程的结构。
	 * 待wait系统完成后，这里再修改
	 */
	if (current->prev_sched && (current->prev_sched->state & TASK_ZOMBIE)) {
		loosen_task_desc(current->prev_sched);
		current->prev_sched = NULL;
	}

	smp_unlock_irq(&lock_all_task_list);
	preempt_enable_no_resched();

same_process:
	if (unlikely(test_process_flag(PROCFLAG_NEED_RESCHED)))
		goto need_resched;

	return;
}

int __sched wake_up_process_special(struct task_desc *tsk, unsigned int state, int sync)
{
	unsigned long flags;
	int ret;

	smp_lock_irqsave(&lock_all_task_list, flags);
	if (tsk->state & state) {
		add_to_runqueue(tsk);
		tsk->state = TASK_RUNNING;
		ret = 0;
	} else
		ret = -1;
	smp_unlock_irqrestore(&lock_all_task_list, flags);

	return ret;
}

int __sched wake_up_process(struct task_desc *tsk)
{
	return wake_up_process_special(tsk, TASK_STOPPED | TASK_TRACED |
				 TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE, 0);
}

void change_task_prio(struct task_desc *tsk, int prio)
{
	unsigned long flags;
	
	if (prio == tsk->sched_prio)
		return;

	smp_lock_irqsave(&lock_all_task_list, flags);

	/**
	 * 此处需要用in_list进行判断，而不能判断进程状态是否为TASK_RUNNING
	 */
	if (tsk->in_run_queue) {
		del_from_runqueue(tsk);
		tsk->sched_prio = prio;
		add_to_runqueue(tsk);
	} else
		tsk->sched_prio = prio;

	smp_unlock_irqrestore(&lock_all_task_list, flags);
}

static void __noreturn do_exit_task(int code)
{
	struct task_desc *tsk = current;

	wake_up_interruptible(&tsk->wait_child_exit);

	set_current_state(TASK_ZOMBIE);
	/**
	 * 这里一旦切换出去以后，无法再回来
	 * 因此清理工作是在schedule中做的
	 * 虽然有点奇怪，但是以后会调整
	 */
	schedule();

	BUG();
}

/**
 * 任务最初的入口
 */
static void task_entry(void)
{
	struct task_desc *tsk = current;
	int (*func)(void *) = tsk->task_main;
	int ret;

	/** 
	 * 稍微有点费解
	 * 这里是与schedule函数前半部分对应
	 */
	smp_unlock(&lock_all_task_list);
	enable_irq();
	preempt_enable();

	/**
	 * 执行任务主函数
	 */
	ret = func(tsk->main_data);

	/**
	 * 任务执行完毕后，进行清理工作
	 */
	do_exit_task(ret);
}

asmlinkage __noreturn void sys_exit(int error_code)
{
	do_exit_task((error_code & 0xff)<<8);
}

struct task_desc *create_task(struct task_create_param *param)
{
	struct task_desc *ret = NULL;
	union process_union *stack = NULL;
	unsigned long flags;
	struct task_desc *tsk;

	if (param->prio < 0 || param->prio >= MAX_RT_PRIO)
		goto out;

	tsk = kmalloc(sizeof(struct task_desc), PAF_KERNEL);
	if (tsk == NULL)
		goto out;
	memset(tsk, 0, sizeof(struct task_desc));

	stack = (union process_union *)alloc_proc_stack();
	if (stack == NULL)
		goto out;

	hold_task_desc(tsk);
	stack->process_desc.task = tsk;
	tsk->stack = stack;
	strncpy(tsk->name, param->name, TASK_NAME_LEN);
      	tsk->magic = TASK_MAGIC; 
	tsk->state = TASK_INIT;
	tsk->sched_prio = param->prio;
	tsk->prio = param->prio;
	tsk->flags = 0;
	tsk->exit_code = 0;
	tsk->task_main = param->func;
	tsk->main_data = param->data;
	tsk->in_run_queue = 0;
	tsk->pid = (pid_t)tsk;
	
	stack->process_desc.preempt_count = 2;
#ifdef  CONFIG_ARM
	tsk->process_desc->cpu_context.fp = 0;
	tsk->process_desc->cpu_context.sp = (unsigned long)stack + THREAD_START_SP;
	tsk->process_desc->cpu_context.pc = (unsigned long)(&task_entry);  
#endif
#ifdef  CONFIG_ARM64
	tsk->task_spot.cpu_context.fp = 0;
	tsk->task_spot.cpu_context.sp = (unsigned long)stack + THREAD_START_SP;
	tsk->task_spot.cpu_context.pc = (unsigned long)(&task_entry);  
#endif
	tsk->prev_sched = NULL;
	init_task_fs(current, tsk);

	hold_task_desc(tsk);
	list_init(&tsk->run_list);
	list_init(&tsk->all_list);
	init_waitqueue(&tsk->wait_child_exit);
	smp_lock_irqsave(&lock_all_task_list, flags);
	tsk->state = TASK_RUNNING;
	add_to_runqueue(tsk);
	list_insert_behind(&tsk->all_list, &sched_all_task_list);
	smp_unlock_irqrestore(&lock_all_task_list, flags);

	return tsk;
out:
	if (tsk)
		kfree(tsk);

	if (stack != NULL)
		free_proc_stack(stack);
	
	return ret;
}

/**
 * 暂停任务运行
 * 在SMP下面应当重新实现本函数
 */
int suspend_task(struct task_desc *tsk)
{
	int ret = -1;
	unsigned long flags;

	if (!tsk)
		return -EINVAL;

	tsk->state |= TASK_STOPPED;
	smp_lock_irqsave(&lock_all_task_list, flags);
	del_from_runqueue(tsk);
	smp_unlock_irqrestore(&lock_all_task_list, flags);

	if (tsk == current)
		schedule();

	ret = 0;
	return ret;
}

/**
 * 恢复任务运行
 */
int resume_task(struct task_desc *tsk)
{
	int ret = -1;
	unsigned long flags;

	if (!tsk)
		return -EINVAL;

	smp_lock_irqsave(&lock_all_task_list, flags);
	tsk->state &= ~TASK_STOPPED;
	if (!tsk->state) {
		add_to_runqueue(tsk);
		tsk->state = TASK_RUNNING;
	}
	smp_unlock_irqrestore(&lock_all_task_list, flags);

	ret = 0;
	return ret;
}

static void init_idle_process(union process_union *stack,
		struct task_desc *proc, int cpu)
{
	if (stack == NULL || proc == NULL)
		BUG();
	
	memset(proc, 0, sizeof(*proc));
	stack->process_desc.task = proc;
	stack->process_desc.cpu = cpu;
	proc->stack = stack->stack;

	snprintf(proc->name, TASK_NAME_LEN, "idle%d", cpu);
	proc->magic = TASK_MAGIC; 
	proc->state = TASK_INIT;
	proc->sched_prio = MAX_RT_PRIO;
	proc->prio = MAX_RT_PRIO;
	proc->flags = 0;
	proc->exit_code = 0;
	proc->task_main = &cpu_idle;
	proc->main_data = NULL;
	proc->in_run_queue = 0;
	proc->prev_sched = NULL;

	init_task_fs(NULL, proc);

	stack->process_desc.preempt_count = 0;

	list_init(&proc->run_list);
	list_init(&proc->all_list);
	init_waitqueue(&proc->wait_child_exit);
	hold_task_desc(proc);
}

/**
 * 调度模块早期初始化
 * 主要是初始化所有核上面的idle堆栈
 */
void init_sched_early(void)
{
	union process_union *stack;
	struct task_desc *proc;
	int i;

	for (i = 0; i < nr_existent_cpus; i++)
	{
		if (i == 0)
		{
			idle_proc_stacks[i] = &master_idle_stack;
		}
		else
		{
			idle_proc_stacks[i] = alloc_boot_mem_permanent(PROCESS_STACK_SIZE,
									PROCESS_STACK_SIZE);
		}
		idle_task_desc[i] = alloc_boot_mem_permanent(sizeof(struct task_desc),
									cache_line_size());
		
		stack = idle_proc_stacks[i];
		proc = idle_task_desc[i];
		init_idle_process(stack, proc, i);
	}
}

/**
 * 调度初始化函数
 */
void init_sched(void)
{
	int i;

	list_init(&sched_all_task_list);
	
	if (!irqs_disabled()) {
		printk("O_O, disable irq, please!");
		disable_irq();
	}
	
	for(i = 0; i < ARRAY_SIZE(sched_runqueue_list); i++)
		list_init(&(sched_runqueue_list[i]));

	memset(sched_runqueue_mask, 0, sizeof(sched_runqueue_mask));
}
