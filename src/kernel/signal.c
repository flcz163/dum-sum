#include <dim-sum/types.h>
#include <dim-sum/errno.h>
#include <dim-sum/linkage.h>
#include <dim-sum/time.h>
#include <dim-sum/sched.h>
#include <dim-sum/signal.h>
#include <asm/siginfo.h>
#include <asm/ptrace.h>
#include <asm/signal.h>

int process_signal(struct exception_spot *regs)
{
	return 0;
}


int post_signal(unsigned long sig,struct task_desc * p,int priv)
{
	return 0;
}

/*
 * post_signal_to_procgroup() sends a signal to a process group: this is what the tty
 * control characters do (^C, ^Z etc)
 */
int post_signal_to_procgroup(int pgrp, int sig, int priv)
{
	struct task_desc *p;
	int err,retval = -ESRCH;
	int found = 0;

	if (sig<0 || sig>32 || pgrp<=0)
		return -EINVAL;
	for_each_task(p) {
		if (p->pgrp == pgrp) {
			if ((err = post_signal(sig,p,priv)) != 0)
				retval = err;
			else
				found++;
		}
	}
	return(found ? 0 : retval);
}

int post_signal_to_proc(int pid, int sig, int priv)
{
 	struct task_desc *p;

	if (sig<0 || sig>32)
		return -EINVAL;
	for_each_task(p) {
		if (p && p->pid == pid)
			return post_signal(sig,p,priv);
	}
	return(-ESRCH);
}

int __rt_sigsuspend(const sigset_t *a, size_t b);

asmlinkage int sys_rt_sigsuspend(const sigset_t *mask, size_t size)
{
	return -ENOSYS;
}

asmlinkage long sys_rt_sigaction(int sig,
		 const struct sigaction *act,
		 struct sigaction *oact,
		 size_t sigsetsize)
{
	return -ENOSYS;
}

asmlinkage long sys_rt_sigprocmask(int how, const sigset_t __user *set, sigset_t __user *oset, size_t sigsetsize)
{
	return -ENOSYS;
}

asmlinkage long sys_rt_sigpending(sigset_t __user *set, size_t sigsetsize)
{
	return -ENOSYS;
}

/**
 * Kill系统调用处理函数
 * pid>0 表示把sig信号发送到其PID==pid的进程所属的线程组。
 * pid==0表示把信号发送到与调用进程同组的进程所有线程组。
 * pid==-1表示把信号发送到所有进程。除了swapper。init和current
 * pid < -1把信号发送到进程组-pid中进程的所有线程线。
 * 虽然kill能够发送编号在32-64之间的实时信号。但是它不能确保把一个新的元素加入到目标进程的挂起信号队列。
 * 因此，发送实时信号需要通过rt_sigqueueinfo系统调用进行。
 */
asmlinkage long
sys_kill(int pid, int sig)
{
	return -ENOSYS;
}

/**
 * Rt_sigqueueinfo的系统调用处理函数
 */
asmlinkage long
sys_rt_sigqueueinfo(int pid, int sig, siginfo_t *uinfo)
{
	return -ENOSYS;
}

int rt_sigtimedwait(const sigset_t *uthese,
		    siginfo_t *uinfo,
		    const struct timespec *uts,
		    size_t sigsetsize);

asmlinkage long
sys_rt_sigtimedwait(const sigset_t __user *uthese,
		    siginfo_t __user *uinfo,
		    const struct timespec __user *uts,
		    size_t sigsetsize)
{
	return -ENOSYS;
}


