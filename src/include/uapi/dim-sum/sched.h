#ifndef _UAPI_SCHED_H
#define _UAPI_SCHED_H

/**
 * 调度策略
 *	1、SCHED_FIFO:实时FIFO调度
 *	2、SCHED_NORMAL:非实时普通任务
 *	3、SCHED_SERVICE:后台服务类任务
 */
#define SCHED_FIFO		0
#define SCHED_NORMAL	1
#define SCHED_SERVICE	2

struct sched_param {
	int sched_priority;
};

#define TASK_NAME_LEN 32
struct task_create_param{
	char name[TASK_NAME_LEN];
	int	prio;
	void *func;
	void *data;
};

/**
 * 进程状态
 */
enum {
	/**
	 * 正在初始化
	 */
	__TASK_INIT,
	/**
	 * 正在运行，但是可能还在调度队列中
	 */
	__TASK_RUNNING,
	/**
	 * 可中断的等待状态。
	 */
	__TASK_INTERRUPTIBLE,
	/**
	 * 不可中断的等待状态。
	 * 这种情况很少，但是有时也有用
	 * 比如进程打开一个设备文件，其相应的驱动程序在探测硬件设备时
	 * 就是这种状态。
	 * 在探测完成前，设备驱动程序如果被中断
	 * 那么硬件设备的状态可能会处于不可预知状态。
	 * 获取信号量也是这种状态
	 */
	__TASK_UNINTERRUPTIBLE,
	/**
	 * 不可被中断，也不可被kill
	 */
	__TASK_UNKILLABLE,
	/**
	 * 暂停状态
	 * 当收到SIGSTOP,SIGTSTP,SIGTTIN或者SIGTTOU信号后，会进入此状态。
	 */
	__TASK_STOPPED,
	/**
	 * 被跟踪状态。
	 * 当进程被另外一个进程监控时，任何信号都可以把这个置于该状态
	 */
	__TASK_TRACED,
	/**
	 * 任务不知道做了什么事情，被手动挂起
	 * 不参与调度了。
	 */
	__TASK_SUSPEND,
	/**
	 * 线程正在被kill
	 */
	__TASK_KILLING,
	/**
	 * 已经被kill，等待父进程调用wait，以完全释放任务描述符
	 * 僵死状态。进程的执行被终止，但是，父进程还没有调用完wait4和waitpid来返回有关
	 * 死亡进程的信息。在此时，内核不能释放相关数据结构，因为父进程可能还需要它。
	 */
	__TASK_ZOMBIE,
};

#define TASK_INIT 			(1UL << __TASK_INIT)
#define TASK_RUNNING		(1UL << __TASK_RUNNING)
#define TASK_INTERRUPTIBLE		(1UL << __TASK_INTERRUPTIBLE)
#define TASK_UNINTERRUPTIBLE	(1UL << __TASK_UNINTERRUPTIBLE)
#define TASK_STOPPED			(1UL << __TASK_STOPPED)
#define TASK_TRACED		(1UL << __TASK_TRACED)
#define TASK_SUSPEND	(1UL << __TASK_SUSPEND)
#define TASK_KILLING			(1UL << __TASK_KILLING)
#define TASK_ZOMBIE		(1UL << __TASK_ZOMBIE)

#define TASK_KILLABLE \
	(TASK_KILLING | TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)
#define TASK_NORMAL             (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)

/**
 * 最大的实时任务优先级
 */
#define MAX_RT_PRIO 128

struct task_desc * create_task(struct task_create_param *param);
extern int suspend_task(struct task_desc *tsk);
extern int resume_task(struct task_desc *tsk);
extern void change_task_prio(struct task_desc *tsk, int prio);
extern void dump_task_stack(struct task_desc *task, unsigned long *sp);

#endif /* _UAPI_SCHED_H */
