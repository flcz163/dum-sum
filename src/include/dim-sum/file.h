#ifndef __DIM_SUM_FILE_H
#define __DIM_SUM_FILE_H

#include <dim-sum/accurate_counter.h>
#include <linux/posix_types.h>
#include <linux/compiler.h>
#include <dim-sum/smp_lock.h>

struct file;

/**
 * 进程当前打开的文件。
 */
struct task_file_handles {
	/**
	 * 共享该表的进程数目。
	 */
        struct accurate_counter count;
	/**
	 * 保护该表的自旋锁。
	 */
        struct smp_lock file_lock;
	/**
	 * 文件对象的当前最大编号。
	 */
        int max_handle;
	/**
	 * 最小的可用描述符
	 */
        int first_free;
	/**
	 * 打开文件描述符的指针。
	 */
        fd_set open_fds;
	/**
	 * 文件对象指针的初始化数组。
	 */
        struct file * fd_array[__FD_SETSIZE];
};

extern void release_file(struct file *);
extern void loosen_file(struct file *);
extern int file_alloc_handle(void);
extern void file_free_handle(unsigned int fd);
extern void file_attach_handle(unsigned int fd, struct file * file);

#endif /* __DIM_SUM_FILE_H */
