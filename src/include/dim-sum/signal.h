#ifndef _DIM_SUM_SIGNAL_H
#define _DIM_SUM_SIGNAL_H

#include <asm/siginfo.h>
#include <asm/signal.h>

struct exception_spot;
struct sigaction;

int process_signal(struct exception_spot *regs);
asmlinkage int sys_rt_sigsuspend(const sigset_t *mask, size_t size);

asmlinkage long sys_rt_sigaction(int sig,
		 const struct sigaction *act,
		 struct sigaction *oact,
		 size_t sigsetsize);

asmlinkage long sys_rt_sigprocmask(int how, const sigset_t __user *set, sigset_t __user *oset, size_t sigsetsize);

asmlinkage long sys_rt_sigpending(sigset_t __user *set, size_t sigsetsize);
asmlinkage long sys_kill(int pid, int sig);
asmlinkage long
sys_rt_sigqueueinfo(int pid, int sig, siginfo_t *uinfo);

asmlinkage long
sys_rt_sigtimedwait(const sigset_t __user *uthese,
		    siginfo_t __user *uinfo,
		    const struct timespec __user *uts,
		    size_t sigsetsize);

#endif /* _DIM_SUM_SIGNAL_H */