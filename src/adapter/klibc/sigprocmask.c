/*
 * sigprocmask.c
 */

#include <signal.h>
#include <sys/syscall.h>
#include <klibc/sysconfig.h>

#if _KLIBC_USE_RT_SIG

int sigprocmask(int how, const sigset_t * set, sigset_t * oset)
{
	return __rt_sigprocmask(how, set, oset, sizeof(sigset_t));
}

#endif
