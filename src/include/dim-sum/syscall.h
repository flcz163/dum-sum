#ifndef _KAPI_DIM_SUM_SYSCALL_H
#define _KAPI_DIM_SUM_SYSCALL_H
#include <dim-sum/linkage.h>
#include <dim-sum/dirent.h>
#include <dim-sum/stat.h>
#include <dim-sum/types.h>
#include <dim-sum/poll.h>
#include <linux/capability.h>
#include <linux/utime.h>
#include <linux/time.h>
#include <linux/resource.h>
#include <asm/signal.h>
#include <asm/statfs.h>
#include <asm/siginfo.h>

typedef int socklen_t;
typedef unsigned int nfds_t;
struct sockaddr;
struct tms;
struct utsname;
struct kexec_segment;
struct msghdr;
struct sched_param;
struct io_segment;

extern asmlinkage long sys_getpid(void);
asmlinkage long sys_getuid(void);
asmlinkage long
sys_rt_sigqueueinfo(int pid, int sig, siginfo_t *uinfo);
asmlinkage long
sys_rt_sigpending(sigset_t __user *set, size_t sigsetsize);
asmlinkage long
sys_rt_sigprocmask(int how, const sigset_t __user *set, sigset_t __user *oset, size_t sigsetsize);

asmlinkage long
sys_rt_sigtimedwait(const sigset_t __user *uthese,
		    siginfo_t __user *uinfo,
		    const struct timespec __user *uts,
		    size_t sigsetsize);

extern asmlinkage int sys_setup(void);
asmlinkage __noreturn void sys_exit(int error_code);

extern asmlinkage pid_t sys_clone(unsigned long a, void *b);

extern asmlinkage pid_t sys_fork(void);
extern asmlinkage pid_t sys_vfork(void);
extern asmlinkage int sys_setpgid(pid_t a, pid_t b);
extern asmlinkage pid_t sys_getpgid(pid_t a);
extern asmlinkage pid_t sys_getppid(void);

extern asmlinkage pid_t sys_setsid(void);

extern asmlinkage pid_t sys_getsid(pid_t a);

//extern asmlinkage pid_t sys_wait4(pid_t a, int *b, int c, struct rusage *d);

extern asmlinkage int sys_execve(const char *a, char * const *b, char * const *c);

extern asmlinkage int sys_nice(int a);
extern asmlinkage int sys_getpriority(int a, int b);

extern asmlinkage int sys_setpriority(int a, int b, int c);

extern asmlinkage int sys_getrusage(int a, struct rusage *b);

extern asmlinkage int sys_sched_setscheduler(pid_t a, int b, const struct sched_param *c);

extern asmlinkage int sys_sched_setaffinity(pid_t a, unsigned int b, unsigned long *c);

extern asmlinkage int sys_sched_getaffinity(pid_t a, unsigned int b, unsigned long *c);

extern asmlinkage int sys_sched_yield(void);

extern asmlinkage int sys_prctl(int a, unsigned long b, unsigned long c, unsigned long d, unsigned long e);

/*
 * User and group IDs
 */
extern asmlinkage int sys_setuid(uid_t a);

extern asmlinkage int sys_setgid(gid_t a);

extern asmlinkage gid_t sys_getgid(void);

extern asmlinkage uid_t sys_geteuid(void);

extern asmlinkage gid_t sys_getegid(void);

extern asmlinkage int sys_getgroups(int a, gid_t *b);

extern asmlinkage int sys_setgroups(size_t a, const gid_t *b);

extern asmlinkage int sys_setreuid(uid_t a, uid_t b);

extern asmlinkage int sys_setregid(gid_t a, gid_t b);

extern asmlinkage int sys_setfsuid(uid_t a);
extern asmlinkage int sys_setfsgid(gid_t a);

extern asmlinkage int sys_setresuid(int a, uid_t b, uid_t c, uid_t d);

/*
 * POSIX Capabilities
 */
extern asmlinkage int sys_capget(cap_user_header_t a, cap_user_data_t b);
extern asmlinkage int sys_capset(cap_user_header_t a, cap_user_data_t b);

/*
 * Filesystem-related system calls
 */
extern asmlinkage int sys_mount(const char __user * dev_name, const char __user * dir_name,
			  const char __user * type, unsigned long flags,
			  const void __user * data);
extern asmlinkage int sys_umount(const char * name, int flags);

extern asmlinkage int sys_umount2(const char *a, int b);

extern asmlinkage int sys_pivot_root(const char *a, const char *b);

extern asmlinkage int sys_sync(void);

extern asmlinkage int sys_statfs(const char *a, struct statfs *b);

extern asmlinkage int sys_fstatfs(unsigned int fd, struct statfs * buf);

extern asmlinkage int sys_swapon(const char *a, int b);

extern asmlinkage int sys_swapoff(const char *a);

/*
 * Inode-related system calls
 */
extern asmlinkage int sys_access(const char *a, int b);

extern asmlinkage int sys_faccessat(int a, const char *b, int c, int d);

extern asmlinkage int sys_link(const char * a, const char *b);

extern asmlinkage int
sys_linkat(int olddirfd, const char *oldpath,
	int newdirfd, const char *newpath);

extern asmlinkage int sys_unlink(const char *a);

extern asmlinkage int sys_unlinkat(int dirfd, const char *pathname, int flags);

extern asmlinkage int sys_chdir(const char *filename);
extern asmlinkage int sys_fchdir(unsigned int fd);
extern int
sys_rename(const char *oldname, const char *newname);
asmlinkage long sys_getdents64(unsigned int fd, struct dirent64 * dirent, unsigned int count);
asmlinkage long sys_getdents(unsigned int fd, struct dirent * dirent, unsigned int count);

extern asmlinkage int sys_renameat(int a, const char *b, int c, const char *d);

extern asmlinkage int sys_mknod(const char * filename, int mode, devno_t dev);

extern asmlinkage int sys_mknodat(int a, const char *b, const char *c, mode_t d, devno_t e);

extern asmlinkage int sys_chmod(const char *a, mode_t b);
extern asmlinkage int sys_fchmod(unsigned int fd, mode_t mode);

extern asmlinkage int sys_fchmodat(int a, const char *b, mode_t c);

extern asmlinkage long sys_mkdir(const char __user * pathname, int mode);

extern asmlinkage int sys_mkdirat(int a, const char *b, const char *c, mode_t d);

extern asmlinkage int sys_rmdir(const char *a);

extern asmlinkage int sys_pipe(int *a);

extern asmlinkage int sys_pipe2(int *a, int b);

extern asmlinkage mode_t sys_umask(mode_t a);


extern asmlinkage int sys_chroot(const char *a);

extern asmlinkage int sys_symlink(const char *a, const char *b);

extern asmlinkage int sys_symlinkat(const char *a, int b, const char *c);

extern asmlinkage int sys_readlink(const char * path, char * buf, int bufsiz);

extern asmlinkage int sys_readlinkat(int a, const char *b, char *c, int d);

extern asmlinkage int sys_stat(const char * filename, struct stat * statbuf);

extern asmlinkage int sys_lstat(const char * filename, struct stat * statbuf);

extern asmlinkage int sys_fstat(unsigned int fd, struct stat * statbuf);

/* XXX: Is this right?! */
extern asmlinkage int sys_fstatat(int a, const char *b, struct stat *c, int d);

//extern asmlinkage int sys_getdents(unsigned int a, struct dirent *b, unsigned int c);


extern asmlinkage int sys_chown(const char *a, uid_t b, gid_t c);
extern asmlinkage int sys_fchown(unsigned int fd, uid_t user, gid_t group);

extern asmlinkage int sys_fchownat(int a, const char *b, uid_t c, gid_t d, int e);

extern asmlinkage int sys_lchown(const char * a, uid_t b, gid_t c);

extern asmlinkage int sys_getcwd(char *a, size_t b);

extern asmlinkage int sys_utime(const char * filename, const struct utimbuf * times);

extern asmlinkage int sys_utimes(const char *a, const struct timeval *b);

extern asmlinkage int sys_futimesat(int a, const char *b, const struct timeval *c);

extern asmlinkage int sys_inotify_init(void);

extern asmlinkage int sys_inotify_add_watch(int a, const char *b, __u32 c);

extern asmlinkage int sys_inotify_rm_watch(int a, __u32 b);

/*
 * I/O operations
 */
extern asmlinkage int sys_open(const char * filename,int flags,int mode);

extern asmlinkage int sys_openat(int a, const char *b, int c, mode_t d);

extern asmlinkage ssize_t sys_read(unsigned int fd, char __user * buf, size_t count);

extern asmlinkage ssize_t sys_write(unsigned int fd, const char __user * buf, size_t count);

extern asmlinkage long sys_close(unsigned int fd);

extern asmlinkage long sys_llseek(unsigned int fd, unsigned long offset_high,
			   unsigned long offset_low, loff_t * result,
			   unsigned int origin);
extern asmlinkage int sys_dup(unsigned int fildes);
extern asmlinkage int sys_dup2(unsigned int oldfd, unsigned int newfd);

extern asmlinkage int sys_dup3(int a, int b, int c);

extern asmlinkage int sys_fcntl(unsigned int fd, unsigned int cmd, unsigned long arg);

extern asmlinkage int sys_ioctl(unsigned int fd, unsigned int cmd, unsigned long arg);

extern asmlinkage int sys_flock(int a, int b);

extern asmlinkage int sys_select( unsigned long *buffer );

extern asmlinkage int sys_poll(struct pollfd *a, nfds_t b, long c);

extern asmlinkage int sys_ppoll(struct pollfd *a, nfds_t b, struct timespec *c, const sigset_t *d, size_t e);

extern asmlinkage int sys_fsync(unsigned int a);

extern asmlinkage int sys_fdatasync(unsigned int fd);

extern asmlinkage int sys_readv(int a, const struct io_segment *b, int c);

extern asmlinkage int sys_writev(int a, const struct io_segment *b, int c);

extern asmlinkage int sys_ftruncate(unsigned int fd, unsigned int length);

extern asmlinkage int sys_truncate(const char *path, unsigned long length);

extern asmlinkage ssize_t sys_pread(int a, void *b, size_t c, off_t d);

extern asmlinkage ssize_t sys_pwrite(int a, void *b, size_t c, off_t d);

extern asmlinkage int sys_sync_file_range(int a, off_t b, off_t c, unsigned int d);

extern asmlinkage int sys_splice(int a, off_t *b, int c, off_t *d, size_t e, unsigned int f);
extern asmlinkage int sys_tee(int a, int b, size_t c, unsigned int d);
extern asmlinkage ssize_t sys_sendfile(int a, int b, off_t *c, size_t d, off_t e);

/*
 * Signal operations
 *
 * We really should get rid of the non-rt_* of these, but that takes
 * sanitizing <signal.h> for all architectures, sigh.  See <klibc/config.h>.
 */
extern asmlinkage int sys_sigaction(int a, const struct sigaction *b, struct sigaction *c);
extern asmlinkage int sys_sigpending(sigset_t *a);
extern asmlinkage int sys_sigprocmask(int a, const sigset_t *b, sigset_t *c);

/*
 * There is no single calling convention for the old sigsuspend.
 * If your architecture is not listed here, building klibc shall
 * rather fail than use a broken calling convention.
 * You better switch to RT signals on those architectures:
 * blackfin h8300 microblaze mips.
 *
 * The arguments other than the sigset_t are assumed ignored.
 */
extern asmlinkage int sys_sigsuspend_s(sigset_t a);

extern asmlinkage int sys_sigsuspend_xxs(int a, int b, sigset_t c);

extern asmlinkage int sys_kill(pid_t a, int b);

extern asmlinkage unsigned int sys_alarm(unsigned int a);

extern asmlinkage int sys_getitimer(int a, struct itimerval *b);

extern asmlinkage int sys_setitimer(int a, const struct itimerval *b, struct itimerval *c);
/*
 * Time-related system calls
 */
extern asmlinkage time_t sys_time(time_t *a);

extern asmlinkage clock_t sys_times(struct tms *a);

extern asmlinkage int sys_gettimeofday(struct timeval *a, struct timezone *b);

extern asmlinkage int sys_settimeofday(const struct timeval *a, const struct timezone *b);

extern asmlinkage long sys_nanosleep(struct timespec __user *rqtp,
			struct timespec __user *rmtp);

extern asmlinkage int sys_rt_sigsuspend(const sigset_t *, size_t);
extern asmlinkage long sys_rt_sigaction(int sig,
		 const struct sigaction *act,
		 struct sigaction *oact,
		 size_t sigsetsize);

extern asmlinkage int sys_pause(void);

/*
 * Memory
 */
extern asmlinkage void * sys_brk(void *a);

extern asmlinkage int sys_munmap(void *a, size_t b);

extern asmlinkage void * sys_mremap(void *a, size_t b, size_t c, unsigned long d);

extern asmlinkage int sys_msync(const void *a, size_t b, int c);

extern asmlinkage int sys_mprotect(const void * a, size_t b, int c);

extern asmlinkage void * sys_mmap(void *a, size_t b, int c, int d, int e, long f);

extern asmlinkage int sys_mlockall(int a);

extern asmlinkage int sys_munlockall(void);

extern asmlinkage int sys_mlock(const void *a, size_t b);

extern asmlinkage int sys_munlock(const void *a, size_t b);

/*
 * System stuff
 */
extern asmlinkage int sys_uname(struct utsname *a);

extern asmlinkage int sys_setdomainname(const char *a, size_t b);

extern asmlinkage int sys_sethostname(const char *a, size_t b);

extern asmlinkage long sys_init_module(void *a, unsigned long b, const char *c);

extern asmlinkage long sys_delete_module(const char *a, unsigned int b);

extern asmlinkage int sys_reboot(int a, int b, int c, void *d);

extern asmlinkage int sys_klogctl(int a, char *b, int c);

extern asmlinkage int sys_sysinfo(struct sysinfo *a);

extern asmlinkage long sys_kexec_load(void *a, unsigned long b, struct kexec_segment *c, unsigned long d);

/*
 * Most architectures have the socket interfaces using regular
 * system calls.
 */
extern asmlinkage long sys_socketcall(int a, const unsigned long *b);
extern asmlinkage int sys_socket(int a, int b, int c);
extern asmlinkage int sys_bind(int a, const struct sockaddr *b, int c);

extern asmlinkage int sys_connect(int a, const struct sockaddr *b, socklen_t c);
extern asmlinkage int sys_listen(int a, int b);
extern asmlinkage int sys_accept(int a, struct sockaddr *b, socklen_t *c);
extern asmlinkage int sys_getsockname(int a, struct sockaddr *b, socklen_t *c);
extern asmlinkage int sys_getpeername(int a, struct sockaddr *b, socklen_t *c);
extern asmlinkage int sys_socketpair(int a, int b, int c, int *d);
extern asmlinkage int sys_sendto(int a, const void *b, size_t c, int d, const struct sockaddr *e, socklen_t f);
extern asmlinkage int sys_recvfrom(int a, void *b, size_t c, unsigned int d, struct sockaddr *e, socklen_t *f);
extern asmlinkage int sys_shutdown(int a, int b);
extern asmlinkage int sys_setsockopt(int a, int b, int c, const void *d, socklen_t e);
extern asmlinkage int sys_getsockopt(int a, int b, int c, void *d, socklen_t *e);
extern asmlinkage int sys_sendmsg(int a, const struct msghdr *b, unsigned int c);
extern asmlinkage int sys_recvmsg(int a, struct msghdr *b, unsigned int c);

#endif /* _KAPI_DIM_SUM_SYSCALL_H */
