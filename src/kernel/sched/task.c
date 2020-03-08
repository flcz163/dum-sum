#include <dim-sum/printk.h>
#include <dim-sum/sched.h>
#include <dim-sum/string.h>
#include <dim-sum/syscall.h>

#include <kapi/dim-sum/task.h>

/**
 * 根据POSIX定义，孤立的进程组不受终端终止信号影响
 * 但是受到SIGHUP和SIGCONT信号的影响
 */
int is_orphaned_pg(int pgrp)
{
	return 0;
}

int capable(int cap)
{
	return 1;
}

int in_group_p(gid_t grp)
{
	return 1;
}

TaskId create_process(int (*func)(void *data),
			void *data,
			char *name,
			int prio)
{
 	struct task_create_param param;
	TaskId ret = (TaskId)NULL;
	int len;

	if (!func) {
		printk("Create task error: func is NULL\n");
		return ret;
	}

	if (!name) {
		printk("Create task error: name is NULL\n");
		return ret;
	}

	if (prio < 0 || prio >= MAX_RT_PRIO) {
		printk("Create task error: prio is error\n");
		return ret;
	}

	len = strlen(name);
	if (len >= sizeof(param.name))
		len = sizeof(param.name) - 1;
		
	strncpy(param.name, name, len);
	param.name[len] = 0;

	param.prio  = prio;
	param.func = func;
	param.data = data;
	ret = (TaskId)create_task(&param);

	WARN_ON(!ret, "create task [%s] error\n", param.name);

	return ret;
}

struct task_desc *kthread_create(int (*threadfn)(void *data),
				   void *data,
				   int prio,
				   const char namefmt[],
				   ...)
{
	char name[TASK_NAME_LEN];
	struct task_desc *task;
	va_list args;

	va_start(args, namefmt);
	vsnprintf(name, TASK_NAME_LEN, namefmt, args);
	va_end(args);

	task = (struct task_desc *)create_process(threadfn, data, name, prio);

	return task;
}

asmlinkage long sys_getpid(void)
{
	return current->pid;
}

asmlinkage long sys_getuid(void)
{
	return current->uid;
}
