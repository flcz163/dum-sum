#define __KLIBC__
#include <linux/compiler.h>
#include <dim-sum/capability.h>
#include <dim-sum/errno.h>
#include <dim-sum/types.h>
#include <dim-sum/uaccess.h>
#include <stdlib.h>
#include <asm/unistd.h>
#include <dim-sum/irqflags.h>
#include <asm/asm-offsets.h>
#include <asm-generic/current.h>
#include <dim-sum/poll.h>
#include <dim-sum/sched.h>
#include <dim-sum/string.h>
#include <asm-generic/siginfo.h>
#include <kapi/sched.h>
#include <kapi/unistd.h>
#include <dim-sum/syscall.h>

/**
 * 桩函数
 */
int verify_area(int type, const void * addr, unsigned long size)
{
	return 0;
}

#if 0
#define SAVE_REGS(size)	\
	asm("sub	sp, sp, %0@ Allocate frame size in one go" : : "i" (size));	\
	asm("stmia	sp, {r0 - lr}			@ Save XXX r0 - lr");
#define RESTORE_REGS(size)	\
	asm("add	sp, sp, %0@ Allocate frame size in one go" : : "i" (size));		
#else
#define SAVE_REGS(size)	
#define RESTORE_REGS(size)	
#endif

static inline void enter_syscall(void)
{
	disable_irq();
	SAVE_REGS(S_FRAME_SIZE);
	barrier();
	if (!current->in_syscall)
	{
		register struct exception_spot *regs asm ("sp");
		current->user_regs = regs;
		current->in_syscall = 1;
	}
	enable_irq();
}

extern void exception_tail(struct exception_spot *regs);

static inline void exit_syscall(void)
{
	disable_irq();
	
	current->in_syscall = 0;
	exception_tail(current->user_regs);
	current->user_regs = NULL;
	barrier();
	RESTORE_REGS(S_FRAME_SIZE);
	
	enable_irq();
}

#define sys_call_and_return(type, func)	\
	do {	\
		type ret;				\
		enter_syscall();	\
		ret = func;			\
		exit_syscall();		\
		return ret;			\
	}while (0)

/**
 * 从用户空间复制任意大小的块。
 * 对dim-sum来说，直接复制即可。
 */
unsigned long
copy_from_user(void *to, const void *from, unsigned long n)
{
	might_sleep();
	BUG_ON((long) n < 0);

	memcpy(to, from, (size_t)n);
	n = 0;

	return n;
}

unsigned long
copy_to_user(void __user *to, const void *from, unsigned long n)
{
	BUG_ON((long) n < 0);

	memcpy(to, from, n);

	return 0;
}

/*
 * Process-related syscalls
 */
__noreturn void _exit(int a)
{
	sys_exit(a);
}

pid_t __clone(int _f, void *_sp)
{
	//sys_call_and_return sys_clone(_f, _sp);
	return -ENOSYS;
}

pid_t getpid(void)
{
	sys_call_and_return(pid_t, sys_getpid());
}

uid_t getuid(void)
{
	sys_call_and_return(pid_t, sys_getuid());
}

int setpgid(pid_t a, pid_t b)
{
	//sys_call_and_return sys_setpgid(a, b);
	return -ENOSYS;
}

pid_t getpgid(pid_t a)
{
	//sys_call_and_return sys_getpgid(a);
	return -ENOSYS;
}

pid_t getppid(void)
{
	//sys_call_and_return sys_getppid();
	return -ENOSYS;
}

pid_t setsid(void)
{
	//sys_call_and_return sys_setsid();
	return -ENOSYS;
}

pid_t getsid(pid_t a)
{
	//sys_call_and_return sys_getsid(a);
	return -ENOSYS;
}

pid_t wait4(pid_t a, int *b, int c, struct rusage *d)
{
	//sys_call_and_return(pid_t, sys_wait4(a, b, c, d));
	return -ENOSYS;
}

int execve(const char *a, char * const *b, char * const *c)
{
	//sys_call_and_return sys_execve(a, b, c);
	return -ENOSYS;
}

int nice(int a)
{
	//sys_call_and_return sys_nice(a);
	return -ENOSYS;
}

int __getpriority(int a, int b)
{
	//sys_call_and_return sys_getpriority(a, b);
	return -ENOSYS;
}

int setpriority(int a, int b, int c)
{
	//sys_call_and_return sys_getpriority(a, b, c);
	return -ENOSYS;
}

int getrusage(int a, struct rusage *b)
{
	//sys_call_and_return sys_getrusage(a, b);
	return -ENOSYS;
}

int sched_setscheduler(pid_t a, int b, const struct sched_param *c)
{
	//sys_call_and_return sys_sched_setscheduler(a, b, c);
	return -ENOSYS;
}

int sched_setaffinity(pid_t a, unsigned int b, unsigned long *c)
{
	//sys_call_and_return sys_sched_setaffinity(a, b, c);
	return -ENOSYS;
}

int sched_getaffinity(pid_t a, unsigned int b, unsigned long *c)
{
	//sys_call_and_return sys_sched_setaffinity(a, b, c);
	return -ENOSYS;
}

int sched_yield(void)
{
	//sys_call_and_return sys_sched_yield();
	return -ENOSYS;
}

int prctl(int a, unsigned long b, unsigned long c, unsigned long d, unsigned long e)
{
	//sys_call_and_return sys_prctl(a, b, c, d, e);
	return -ENOSYS;
}

/*
 * User and group IDs
 */
int setuid(uid_t a)
{
	//sys_call_and_return sys_setuid(a);
	return -ENOSYS;
}

int setgid(gid_t a)
{
	//sys_call_and_return sys_setgid(a);
}

gid_t getgid(void)
{
	//sys_call_and_return sys_getgid();
	return -ENOSYS;
}

uid_t geteuid(void)
{
	//sys_call_and_return sys_geteuid();
	return -ENOSYS;
}

gid_t getegid(void)
{
	//sys_call_and_return sys_getegid();
	return -ENOSYS;
}

int getgroups(int a, gid_t *b)
{
	//sys_call_and_return sys_getgroups(a, b);
	return -ENOSYS;
}

int setgroups(size_t a, const gid_t *b)
{
	//sys_call_and_return sys_setgroups(a, b);
	return -ENOSYS;
}

int setreuid(uid_t a, uid_t b)
{
	//sys_call_and_return sys_setreuid(a, b);
	return -ENOSYS;
}

int setregid(gid_t a, gid_t b)
{
	//sys_call_and_return sys_setregid(a, b);
	return -ENOSYS;
}

int setfsuid(uid_t a)
{
	//sys_call_and_return sys_setregid(a);
	return -ENOSYS;
}

int setfsgid(gid_t a)
{
	//sys_call_and_return sys_setfsgid(a);
	return -ENOSYS;
}

int setresuid(uid_t a, uid_t b, uid_t c)
{
	//sys_call_and_return sys_setresuid(a, b, c);
	return -ENOSYS;
}

/*
 * POSIX Capabilities
 */
int capget(cap_user_header_t a, cap_user_data_t b)
{
	//sys_call_and_return sys_capget(a, b);
	return -ENOSYS;
}

int capset(cap_user_header_t a, cap_user_data_t b)
{
	//sys_call_and_return sys_capset(a, b);
	return -ENOSYS;
}

/*
 * Filesystem-related system calls
 */
int mount(const char __user *dev_name, const char __user *dir_name,
		   const char __user *type, unsigned long flags, const void __user *data)
{
	sys_call_and_return(int, sys_mount(dev_name, dir_name, type, flags, data));
}

int umount2(const char *name, int flags)
{
	sys_call_and_return(int, sys_umount(name, flags));
}

int pivot_root(const char *a, const char *b)
{
	//sys_call_and_return sys_prvot_root(a, b);
	return -ENOSYS;
}

int sync(void)
{
	sys_call_and_return(int, sys_sync());
}

int statfs(const char *a, struct statfs *b)
{
	//sys_call_and_return(int, sys_statfs(a, b));
	return -ENOSYS;
}

int fstatfs(int a, struct statfs *b)
{
	//sys_call_and_return(int, sys_fstatfs(a, b));
	return -ENOSYS;
}

int swapon(const char *a, int b)
{
	//sys_call_and_return sys_swapon(a, b);
	return -ENOSYS;
}

int swapoff(const char *a)
{
	//sys_call_and_return sys_swapoff(a);
	return -ENOSYS;
}

/*
 * Inode-related system calls
 */
int access(const char *a, int b)
{
	//sys_call_and_return(int, sys_access(a, b));
	return -ENOSYS;
}

int faccessat(int a, const char *b, int c, int d)
{
	//sys_call_and_return sys_faccessat(a, b, c, d);
	return -ENOSYS;
}

int link(const char *oldpath, const char *newpath)
{
	sys_call_and_return(int, sys_link(oldpath, newpath));
}

int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath)
{
	sys_call_and_return(int, sys_linkat(olddirfd, oldpath, newdirfd, newpath));
}

int unlink(const char * pathname)
{
	sys_call_and_return(int, sys_unlink(pathname));
}

int unlinkat(int dirfd, const char *pathname, int flags)
{
	sys_call_and_return(int, sys_unlinkat(dirfd, pathname, flags));
}

int chdir(const char *filename)
{
	sys_call_and_return(int, sys_chdir(filename));
}

int fchdir(int a)
{
	//sys_call_and_return(int, sys_fchdir(a));
	return -ENOSYS;
}

int rename(const char *a, const char *b)
{
	sys_call_and_return(int, sys_rename(a, b));
}

int renameat(int a, const char *b, int c, const char *d)
{
	//sys_call_and_return sys_renameat(a, b, c, d);
	return -ENOSYS;
}

int mknod(const char *a, mode_t b, devno_t c)
{
	//sys_call_and_return(int, sys_mknod(a, b, c));
	return -ENOSYS;
}

int mknodat(int a, const char *b, const char *c, mode_t d, devno_t e)
{
	//sys_call_and_return sys_mknodat(a, b, c, d, e);
	return -ENOSYS;
}

int chmod(const char *a, mode_t b)
{
	//sys_call_and_return(int, sys_chmod(a, b));
	return -ENOSYS;
}

int fchmod(int a, mode_t b)
{
	//sys_call_and_return(int, sys_fchmod(a, b));
	return -ENOSYS;
}

int fchmodat(int a, const char *b, mode_t c)
{
	//sys_call_and_return sys_fchmodat(a, b, c);
	return -ENOSYS;
}

int mkdir(const char __user * pathname, mode_t mode)
{
	sys_call_and_return(int, sys_mkdir(pathname, mode));
}

int mkdirat(int a, const char *b, const char *c, mode_t d)
{
	//sys_call_and_return sys_mkdirat(a, b, c, d);
	return -ENOSYS;
}

int rmdir(const char *a)
{
	//sys_call_and_return(int, sys_rmdir(a));
	return -ENOSYS;
}

int pipe(int *a)
{
	//sys_call_and_return(int, sys_pipe(a));
	return -ENOSYS;
}

int pipe2(int *a, int b)
{
	//sys_call_and_return sys_pipe2(a, b);
	return -ENOSYS;
}

mode_t umask(mode_t a)
{
	//sys_call_and_return(mode_t, sys_umask(a));
	return -ENOSYS;
}


int chroot(const char *a)
{
	//sys_call_and_return(int, sys_chroot(a));
	return -ENOSYS;
}

int symlink(const char *a, const char *b)
{
	//sys_call_and_return(int, sys_symlink(a, b));
	return -ENOSYS;
}

int symlinkat(const char __user * oldname,
			      int newdfd, const char __user * newname)
{
	//sys_call_and_return sys_symlinkat(a, b, c);
	return -ENOSYS;
}

int readlink(const char *a, char *b, size_t c)
{
	//sys_call_and_return(int, sys_readlink(a, b, c));
	return -ENOSYS;
}

int readlinkat(int dfd, const char __user *path, char __user *buf,
			       int bufsiz)
{
	//sys_call_and_return sys_readlinkat(a, b, c, d);
	return -ENOSYS;
}

int stat(const char * filename, struct stat * statbuf)
{
	sys_call_and_return(int, sys_stat(filename, statbuf));
}

int lstat(const char * filename, struct stat * statbuf)
{
	sys_call_and_return(int, sys_lstat(filename, statbuf));
}

int fstat(int a, struct stat *b)
{
	//sys_call_and_return(int, sys_fstat(a, b));
	return -ENOSYS;
}

/* XXX: Is this right?! */
int fstatat(int a, const char *b, struct stat *c, int d)
{
	//sys_call_and_return sys_fstatat(a, b, c, d);
	return -ENOSYS;
}

int getdents(unsigned int fd, struct dirent64 * dirent, unsigned int count)
{
	sys_call_and_return(int, sys_getdents64(fd, dirent, count));
}


int chown(const char *a, uid_t b, gid_t c)
{
	//sys_call_and_return(int, sys_chown(a, b, c));
	return -ENOSYS;
}

int fchown(int a, uid_t b, gid_t c)
{
	//sys_call_and_return(int, sys_fchown(a, b, c));
	return -ENOSYS;
}

int fchownat(int a, const char *b, uid_t c, gid_t d, int e)
{
	//sys_call_and_return sys_fchownat(a, b, c, d, e);
	return -ENOSYS;
}

int lchown(const char * a, uid_t b, gid_t c)
{
	//sys_call_and_return sys_lchown(a, b, c);
	return -ENOSYS;
}

int __getcwd(char *a, size_t b)
{
	//return sys_getcwd(a, b);
	return -ENOSYS;
}

int utime(const char *a, const struct utimbuf *b)
{
	//sys_call_and_return(int, sys_utime(a, b));
	return -ENOSYS;
}

int utimes(const char *a, const struct timeval *b)
{
	//sys_call_and_return sys_utimes(a, b);
	return -ENOSYS;
}

int futimesat(int a, const char *b, const struct timeval *c)
{
	//sys_call_and_return sys_futimesat(a, b, c);
	return -ENOSYS;
}

int inotify_init(void)
{
	//sys_call_and_return sys_inotify_init();
	return -ENOSYS;
}

int inotify_add_watch(int a, const char *b, __u32 c)
{
	//sys_call_and_return sys_inotify_add_watch(a, b, c);
	return -ENOSYS;
}

int inotify_rm_watch(int a, __u32 b)
{
	//sys_call_and_return sys_inotify_rm_watch(a, b);
	return -ENOSYS;
}

/*
 * I/O operations
 */
int __open(const char *pathname, int flags, mode_t mode)
{
	sys_call_and_return(int, sys_open(pathname, flags, mode));
}

int __openat(int a, const char *b, int c, mode_t d)
{
	//sys_call_and_return sys_openat(a, b, c, d);
	return -ENOSYS;
}

ssize_t read(unsigned int fd, char __user * buf, size_t count)
{
	sys_call_and_return(size_t, sys_read(fd, buf, count));
}

ssize_t write(unsigned int fd, const char __user * buf, size_t count)
{
	sys_call_and_return(size_t, sys_write(fd, buf, count));
}

int close(int a)
{
	sys_call_and_return(int, sys_close(a));
}

int __llseek(int fd, unsigned long hi, unsigned long lo, loff_t * res,
		    int whence)
{
	sys_call_and_return(int, sys_llseek(fd, hi, lo, res, whence));
}

int dup(int a)
{
	//sys_call_and_return(int, sys_dup(a));
	return -ENOSYS;
}

int dup2(int a, int b)
{
	//sys_call_and_return(int, sys_dup2(a, b));
	return -ENOSYS;
}

int dup3(int a, int b, int c)
{
	//sys_call_and_return sys_dup3(a, b, c);
	return -ENOSYS;
}

int fcntl(unsigned int fd, unsigned int cmd, unsigned long arg)
{
	//sys_call_and_return(int, sys_fcntl(a, b, c));
	return -ENOSYS;
}

int ioctl(int a, int b, void *c)
{
	//sys_call_and_return(int, sys_ioctl(a, b, (unsigned long)c));
	return -ENOSYS;
}

int flock(int a, int b)
{
	//sys_call_and_return sys_flock(a, b);
	return -ENOSYS;
}

int select(int a, fd_set *b, fd_set *c, fd_set *d, struct timeval *e)
{
	//return sys_select(a, b, c, d, e);
	return -ENOSYS;
}

int poll(struct pollfd *a, nfds_t b, long c)
{
	//sys_call_and_return sys_poll(a, b, c);
	return -ENOSYS;
}

int __ppoll(struct pollfd *a, nfds_t b, struct timespec *c, const sigset_t *d, size_t e)
{
	//sys_call_and_return sys_ppoll(a, b, c, d, e);
	return -ENOSYS;
}

int fsync(int fd)
{
	sys_call_and_return(int, sys_fsync(fd));
}

int fdatasync(int fd)
{
	sys_call_and_return(int, sys_fdatasync(fd));
}

int readv(int a, const struct io_segment *b, int c)
{
	//sys_call_and_return sys_readv(a, b, c);
	return -ENOSYS;
}

int writev(int a, const struct io_segment *b, int c)
{
	//sys_call_and_return sys_writev(a, b, c);
	return -ENOSYS;
}

int truncate(char *path, unsigned long length)
{
	sys_call_and_return(int, sys_truncate(path, length));
}

int ftruncate(unsigned int fd, unsigned long length)
{
	sys_call_and_return(int, sys_ftruncate(fd, length));
}

ssize_t pread(int a, void *b, size_t c, off_t d)
{
	//sys_call_and_return sys_pread(a, b, c, d);
	return -ENOSYS;
}

ssize_t pwrite(unsigned int fd, const char __user *buf,
				size_t count, loff_t pos)
{
	//sys_call_and_return sys_pwrite(a, b, c, d);
	return -ENOSYS;
}

int sync_file_range(int a, off_t b, off_t c, unsigned int d)
{
	//sys_call_and_return sys_sync_file_range(a, b, c, d);
	return -ENOSYS;
}

int splice(int a, off_t *b, int c, off_t *d, size_t e, unsigned int f)
{
	//sys_call_and_return sys_splice(a, b, c, d, e, f);
	return -ENOSYS;
}

int tee(int a, int b, size_t c, unsigned int d)
{
	//sys_call_and_return sys_tee(a, b, c, d);
	return -ENOSYS;
}

ssize_t sendfile(int a, int b, off_t *c, size_t d, off_t e)
{
	//sys_call_and_return sys_sendfile(a, b, c, d, e);
	return -ENOSYS;
}

/*
 * Signal operations
 *
 * We really should get rid of the non-rt_* of these, but that takes
 * sanitizing <signal.h> for all architectures, sigh.  See <klibc/config.h>.
 */
int __sigaction(int a, const struct sigaction *b, struct sigaction *c)
{
	//sys_call_and_return sys_sigaction(a, b, c);
	return -ENOSYS;
}


/*
 * There is no single calling convention for the old sigsuspend.
 * If your architecture is not listed here, building klibc shall
 * rather fail than use a broken calling convention.
 * You better switch to RT signals on those architectures:
 * blackfin h8300 microblaze mips.
 *
 * The arguments other than the sigset_t are assumed ignored.
 */
int __sigsuspend_s(sigset_t a)
{
	//sys_call_and_return sys_sigsuspend_s(a);
	return -ENOSYS;
}

int __sigsuspend_xxs(int a, int b, sigset_t c)
{
	//sys_call_and_return sys_sigsuspend(a, b, c);
	return -ENOSYS;
}

unsigned int alarm(unsigned int a)
{
	//sys_call_and_return sys_alarm(a);
	return -ENOSYS;
}

int getitimer(int a, struct itimerval *b)
{
	//sys_call_and_return sys_getitimer(a, b);
	return -ENOSYS;
}

int setitimer(int a, const struct itimerval *b, struct itimerval *c)
{
	//sys_call_and_return sys_setitimer(a, b, c);
	return -ENOSYS;
}

/*
 * Time-related system calls
 */
time_t time(time_t *a)
{
	//sys_call_and_return sys_time(a);
	return -ENOSYS;
}

clock_t times(struct tms *a)
{
	//sys_call_and_return sys_times(a);
	return -ENOSYS;
}

int gettimeofday(struct timeval *a, struct timezone *b)
{
	//sys_call_and_return sys_gettimeofday(a, b);
	return -ENOSYS;
}

int settimeofday(const struct timeval *a, const struct timezone *b)
{
	//sys_call_and_return sys_settimeofday(a, b);
	return -ENOSYS;
}

int __rt_sigqueueinfo(int pid, int sig, siginfo_t *uinfo)
{
	sys_call_and_return(int, sys_rt_sigqueueinfo(pid, sig, uinfo));
}

int __rt_sigsuspend(const sigset_t *a, size_t b)
{
	sys_call_and_return(int, sys_rt_sigsuspend(a, b));
}

int __rt_sigaction(int a, const struct sigaction *b, struct sigaction *c,
			    size_t d)
{
	sys_call_and_return(int, sys_rt_sigaction(a, b, c, d));
}


int __rt_sigpending (sigset_t *a, size_t b)
{
	sys_call_and_return(int, sys_rt_sigpending(a, b));
}

int __rt_sigprocmask(int how, const sigset_t *set, sigset_t *oldset, size_t size)
{
	sys_call_and_return(int, sys_rt_sigprocmask(how, set, oldset, size));
}

int rt_sigtimedwait(const sigset_t *uthese,
		    siginfo_t *uinfo,
		    const struct timespec *uts,
		    size_t sigsetsize)
{
	sys_call_and_return(int, sys_rt_sigtimedwait(uthese, uinfo, uts, sigsetsize));
}


int kill(pid_t a, int b)
{
	return sys_kill(a, b);
}

int nanosleep(const struct timespec *a, struct timespec *b)
{
	sys_call_and_return(int, sys_nanosleep(a, b));
}

int pause(void)
{
	//sys_call_and_return sys_pause();
	return -ENOSYS;
}

/*
 * Memory
 */
void * __brk(void *a)
{
	//sys_call_and_return sys_brk(a);
	return -ENOSYS;
}

int munmap(void *a, size_t b)
{
	//sys_call_and_return sys_munmap(a, b);
	return -ENOSYS;
}

void * mremap(void *a, size_t b, size_t c, unsigned long d)
{
	//sys_call_and_return sys_mremap(a, b, c, d);
	return -ENOSYS;
}

int msync(const void *a, size_t b, int c)
{
	//sys_call_and_return sys_msync(a, b, c);
	return -ENOSYS;
}

int mprotect(const void * a, size_t b, int c)
{
	//sys_call_and_return sys_mprotect(a, b, c);
	return -ENOSYS;
}

void * mmap(void *a, size_t b, int c, int d, int e, long f)
{
	//sys_call_and_return sys_mmap(a, b, c, d, e, f);
	return -ENOSYS;
}

int mlockall(int a)
{
	//sys_call_and_return sys_mlockall(a);
}

int munlockall(void)
{
	//sys_call_and_return sys_munlockall();
	return -ENOSYS;
}

int mlock(const void *a, size_t b)
{
	//sys_call_and_return sys_mlock(a, b);
	return -ENOSYS;
}

int munlock(const void *a, size_t b)
{
	//sys_call_and_return sys_munlock(a, b);
	return -ENOSYS;
}

/*
 * System stuff
 */
int uname(struct utsname *a)
{
	//sys_call_and_return sys_uname(a);
	return -ENOSYS;
}

int setdomainname(const char *a, size_t b)
{
	//sys_call_and_return sys_setdomainname(a, b);
	return -ENOSYS;
}

int sethostname(const char *a, size_t b)
{
	//sys_call_and_return sys_sethostname(a, b);
	return -ENOSYS;
}

long init_module(void *a, unsigned long b, const char *c)
{
	//sys_call_and_return sys_init_module(a, b, c);
	return -ENOSYS;
}

long delete_module(const char *a, unsigned int b)
{
	//sys_call_and_return sys_delete_module(a, b);
	return -ENOSYS;
}

int __reboot(int a, int b, int c, void *d)
{
	//sys_call_and_return sys_reboot(a, b, c, d);
	return -ENOSYS;
}

int klogctl(int a, char *b, int c)
{
	//sys_call_and_return sys_syslog(a, b, c);
	return -ENOSYS;
}

int sysinfo(struct sysinfo *a)
{
	//sys_call_and_return sys_sysinfo(a);
	return -ENOSYS;
}

long kexec_load(void *a, unsigned long b, struct kexec_segment *c, unsigned long d)
{
	//sys_call_and_return sys_kexec_load(a, b, c, d);
	return -ENOSYS;
}

#ifndef CONFIG_LWIP
/*
 * Most architectures have the socket interfaces using regular
 * system calls.
 */
long __socketcall(int a, const unsigned long *b)
{
	//sys_call_and_return sys_socketcall(a, b);
	return -ENOSYS;
}
int socket(int a, int b, int c)
{
	//sys_call_and_return sys_socket(a, b, c);
	return -ENOSYS;
}
int bind(int a, const struct sockaddr *b, int c)
{
	//sys_call_and_return sys_bind(a, b, c);
	return -ENOSYS;
}

int connect(int a, const struct sockaddr *b, socklen_t c)
{
	//sys_call_and_return sys_connect(a, b, c);
	return -ENOSYS;
}

int listen(int a, int b)
{
	//sys_call_and_return sys_listen(a, b);
	return -ENOSYS;
}

int accept(int a, struct sockaddr *b, socklen_t *c)
{
	//sys_call_and_return sys_accept(a, b, c);
	return -ENOSYS;
}

int getsockname(int a, struct sockaddr *b, socklen_t *c)
{
	//sys_call_and_return sys_getsockname(a, b, c);
	return -ENOSYS;
}

int getpeername(int a, struct sockaddr *b, socklen_t *c)
{
	//sys_call_and_return sys_getpeername(a, b, c);
	return -ENOSYS;
}

int socketpair(int a, int b, int c, int *d)
{
	//sys_call_and_return sys_socketpair(a, b, c, d);
	return -ENOSYS;
}

int sendto(int a, const void *b, size_t c, int d, const struct sockaddr *e, socklen_t f)
{
	//sys_call_and_return sys_sendto(a, b, c, d, e, f);
	return -ENOSYS;
}

int recvfrom(int a, void *b, size_t c, unsigned int d, struct sockaddr *e, socklen_t *f)
{
	//sys_call_and_return sys_recvfrom(a, b, c, d, e, f);
	return -ENOSYS;
}

int shutdown(int a, int b)
{
	//sys_call_and_return sys_shutdown(a, b);
	return -ENOSYS;
}

int setsockopt(int a, int b, int c, const void *d, socklen_t e)
{
	//sys_call_and_return sys_setsockopt(a, b, c, d, e);
	return -ENOSYS;
}

int getsockopt(int a, int b, int c, void *d, socklen_t *e)
{
	//sys_call_and_return sys_getsockopt(a, b, c, d, e);
	return -ENOSYS;
}

int sendmsg(int a, const struct msghdr *b, unsigned int c)
{
	//sys_call_and_return sys_sendmsg(a, b, c);
	return -ENOSYS;
}

int recvmsg(int a, struct msghdr *b, unsigned int c)
{
	//sys_call_and_return sys_recvmsg(a, b, c);
	return -ENOSYS;
}

#endif
