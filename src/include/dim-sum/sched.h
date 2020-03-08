#ifndef __DIM_SUM_SCHED_H
#define __DIM_SUM_SCHED_H

#ifdef __KERNEL__
#include <kapi/dim-sum/task.h>
#include <uapi/dim-sum/sched.h>
#include <dim-sum/linkage.h>
#include <dim-sum/process.h>
#include <dim-sum/ref.h>
#include <dim-sum/time.h>
#include <dim-sum/wait.h>

#include <asm/current.h>
#include <asm/processor.h>

struct tty_struct;
struct wait_queue;
struct task_fs_context;
struct task_file_handles;
struct blkdev_infrast;

/**
 * 任务标志
 */
enum {
	/**
	 * 不要冻结此任务
	 */
	__TASKFLAG_NOFREEZE,
	/**
	 * 任务正在被冷冻
	 */
	__TASKFLAG_FREEZING,
	/**
	 * 任务已经被冷冻
	 */
	__TASKFLAG_FROZEN,
	/**
	 * 任务正在执行内存回收
	 * 也就是可以突破内存水线
	 */
	__TASKFLAG_RECLAIM,
	/**
	 * 正在执行同步写，例如在调用fsync
	 */
	__TASKFLAG_SYNCWRITE,
	/**
	 * 当前任务是后台回写任务
	 */
	__TASKFLAG_FLUSHER,
};

#define TASKFLAG_NOFREEZE		(1UL << __TASKFLAG_NOFREEZE)
#define TASKFLAG_FREEZING		(1UL << __TASKFLAG_FREEZING)
#define TASKFLAG_FROZEN		(1UL << __TASKFLAG_FROZEN)
#define TASKFLAG_RECLAIM		(1UL << __TASKFLAG_RECLAIM)
#define TASKFLAG_SYNCWRITE	(1UL << __TASKFLAG_SYNCWRITE)
#define TASKFLAG_FLUSHER		(1UL << __TASKFLAG_FLUSHER)

/**
 * 任务描述符
 */
struct task_desc {
	/**
	 * 魔法值
	 */
	int 			magic;
	/**
	 * 任务标志
	 */
	unsigned int 	flags;
	/**
	 * 任务当前状态，如TASK_INTERRUPTIBLE
	 */
	volatile long 	state;
	/**
	 * 任务堆栈，即process_desc描述符
	 */
	void			*stack;
	/**
	 * 任务调度优先级，当前按此优先级进行调度
	 * 可能比创建时的优先级有所提高或者降低
	 */
	int 			sched_prio;
	/**
	 * 任务优先级，创建时的优先级
	 */
	int			prio;
	/**
	 * 是否在运行队列中
	 */
	bool			in_run_queue;
	/**
	 * 通过此字段将任务放进运行队列 
	 */
	struct double_list 		run_list;
	/**
	 * 通过此字段将任务放进全局链表
	 */	
	struct double_list 		all_list;
	/**
	 * 任务所属的用户id、组id
	 * 目前未用，以后用会
	 */
	uid_t uid;
	uid_t euid;
	gid_t egid;
	/**
	 * 组id
	 * 目前未用
	 */
	gid_t gid;
	gid_t pgrp;
	uid_t fsuid;
	gid_t fsgid;
	/**
	 * 名称
	 * 有点奇怪吧，这几个字段竟然没有在最前面
	 */
	char 		name[TASK_NAME_LEN];
	/**
	 * 任务的引用计数
	 */
	struct ref_count	ref;
	/**
	 * 任务ID
	 */
	pid_t pid;
	/**
	 * 用户态寄存器现场
	 * 目前未用
	 */
	struct exception_spot *user_regs;
	/**
	 * 是否处于系统调用中
	 * 为了在内核态支持系统调用
	 */
	int		in_syscall;
	/**
	 * 临时变量，有点奇怪
	 * 用于释放描述符，以后会废弃
	 */
	struct task_desc 		*prev_sched;
	/**
	 * 线程主函数
	 */
	void 		*task_main;
	/**
	 * 传给线程主函数的参数
	 */
	void 		*main_data;
	/**
	 * 任务退出代码
	 */
	int			exit_code;
	/**
	 * 与当前任务相关的文件句柄结构
	 * 目前使用全局数组。
	 */
	struct task_file_handles	*files;
	/**
	 * 与文件系统相关的信息。如当前目录。
	 */
	struct task_fs_context *fs_context;
	/**
	 * 文件系统在查找路径时使用
	 * 避免符号链接查找深度过深，导致死循环。
	 */
	struct {
		/**
		 * 递归链接查找的层次。
		 * link_count调用链接查找的总次数。
		 */
		int nested_count;
		/**
		 * 调用链接查找的总次数。
		 */
		int link_count;
	} fs_search;
	/**
	 * 任务当前使用的tty
	 */
	struct tty_struct *tty;
	/**
	 * 当前活动日志操作处理的地址。
	 * 正在使用的原子操作对象。
	 */
	void *journal_info;
	/**
	 * 在系统调用wait4中睡眠的进程的等待队列。
	 */
	struct wait_queue		wait_child_exit;
	/**
	 * 任务调度\恢复的寄存器现场
	 */
	struct task_spot		task_spot;
};

#define __sched		__attribute__((__section__(".sched.text")))

/**
 * 全局静态的任务堆栈，用于boot cpu的idle进程
 * 避免在系统启动阶段分配内存
 */
extern union process_union master_idle_stack;

extern struct task_desc *idle_task_desc[];
static inline struct task_desc *idle_task(int cpu)
{
	return idle_task_desc[cpu];
}

static inline void
__set_task_state(struct task_desc *tsk, unsigned long state)
{
	tsk->state = state;
}

/**
 * 设置进程状态。同时会加上内存屏障。
 */
static inline void
set_task_state(struct task_desc *tsk, unsigned long state)
{
	tsk->state = state;
	smp_mb();
}

/**
 * 设置当前进程状态。无内存屏障。
 */
static inline void
__set_current_state(unsigned long state)
{
	current->state = state;
}

/**
 * 设置当前进程状态。同时会加上内存屏障。
 */
static inline void
set_current_state(unsigned long state)
{
	set_task_state(current, state);
}

#define task_process_info(task)	((struct process_desc *)(task)->stack)
#define task_stack_bottom(task)	((task)->stack)

static inline int test_task_proc_flag(struct task_desc *tsk, int flag)
{
	return __test_process_flag(task_process_info(tsk), flag);
}

static inline void set_task_proc_flag(struct task_desc *tsk, int flag)
{
	__set_process_flag(task_process_info(tsk), flag);
}

static inline void set_task_need_resched(struct task_desc *tsk)
{
	set_task_proc_flag(tsk, PROCFLAG_NEED_RESCHED);
}

static inline void clear_task_need_resched(struct task_desc *tsk)
{
	__clear_process_flag(task_process_info(tsk), PROCFLAG_NEED_RESCHED);
}

static inline int need_resched(void)
{
	return unlikely(test_process_flag(PROCFLAG_NEED_RESCHED));
}

static inline void set_need_resched(void)
{
	set_process_flag(PROCFLAG_NEED_RESCHED);
}

static inline void clear_need_resched(void)
{
	clear_process_flag(PROCFLAG_NEED_RESCHED);
}

static inline int signal_pending(struct task_desc *p)
{
	return unlikely(test_task_proc_flag(p, PROCFLAG_SIGPENDING));
}

/**
 * 任务描述符引用计数
 */
#define hold_task_desc(tsk)	ref_count_hold(&(tsk)->ref)
extern void __release_task_desc(struct ref_count *ref);
#define loosen_task_desc(tsk)	ref_count_loosen(&(tsk)->ref, __release_task_desc)

/**
 * 全局任务链表
 */
extern struct double_list sched_all_task_list;

static inline struct task_desc *next_task(struct task_desc *p)
{
	struct task_desc *next;

	if (p->all_list.next == &sched_all_task_list)
		return NULL;

	next = list_container(p->all_list.next, struct task_desc, all_list);
	return next;
}

static inline struct task_desc *first_task(void)
{
	if (list_is_empty(&sched_all_task_list))
		return NULL;
	else	
		return list_container(sched_all_task_list.next, struct task_desc, all_list);
}

/**
 * 遍历所有任务
 */
#define for_each_task(p) \
	list_for_each_entry(p, &(sched_all_task_list), all_list)

#define rt_task(p) 1

#define MAX_SCHEDULE_TIMEOUT ((unsigned long)(~0UL>>1))
signed long schedule_timeout(signed long timeout);

extern long io_schedule_timeout(long timeout);
static inline void io_schedule(void)
{
	io_schedule_timeout(MAX_SCHEDULE_TIMEOUT);
}

extern int capable(int cap);
extern int in_group_p(gid_t grp);
int is_orphaned_pg(int pgrp);
int init_task_fs(struct task_desc *parent, struct task_desc *new);

extern asmlinkage void schedule(void);
extern asmlinkage void preempt_in_irq(void);
int wake_up_process_special(struct task_desc *tsk,
	unsigned int state, int sync);
int wake_up_process(struct task_desc *tsk);
extern struct task_desc * __switch_cpu_context(struct task_desc *prev,
	struct task_desc *next);

extern int process_signal(struct exception_spot *regs);
extern int post_signal(unsigned long sig,struct task_desc * p,int priv);
extern int post_signal_to_proc(int pid, int sig, int priv);
extern int post_signal_to_procgroup(int pgrp, int sig, int priv);

static inline int kthread_stop(struct task_desc *k)
{
	/* TO-DO */
	return 0;
}

static inline int kthread_should_stop(void)
{
	/* TO-DO */
	return 0;
}

static inline void kthread_bind(struct task_desc *k, unsigned int cpu)
{
	/* TO-DO */
}

struct task_desc *kthread_create(int (*threadfn)(void *data),
			void *data, int prio, const char namefmt[], ...);

/**
 * 模块初始化函数
 *	init_sched_early:早期初始化，只能使用bootmem分配
 *	init_sched:初始化
 */
extern void init_sched_early(void);
extern void init_sched(void);

#endif /* __KERNEL__ */

#endif /* __DIM_SUM_SCHED_H */
