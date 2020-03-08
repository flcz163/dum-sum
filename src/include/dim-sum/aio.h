#ifndef __LINUX__AIO_H
#define __LINUX__AIO_H

#include <dim-sum/double_list.h>
#include <dim-sum/wait.h>
#include <dim-sum/accurate_counter.h>
/**
 * 同步和异步Io操作的完成状态。
 */
struct async_io_desc {
	/**
	 * 描述符的引用计数器。
	 */
	int			users;
	/**
	 * 异步IO操作标识符。同步IO操作标识符为0xffffffff
	 */
	unsigned		key;		/* id of this request */

	/**
	 * IO操作相关的文件对象指针
	 */
	struct file		*file;
	/**
	 * 对于同步操作，这旨指向发出该操作的进程描述符的指针。
	 * 对于异步操作，它是指向用户态数据结构iocb的指针。
	 */
	union {
		void __user		*user;
		struct task_desc	*tsk;
	} obj;
	/**
	 * 给用户态进程返回的值。
	 */
	__u64			user_data;	/* user's data for completion */
	/**
	 * 正在进行IO操作的当前文件位置。
	 */
	loff_t			pos;
	/**
	 * 异步IO操作等待队列。
	 */
	struct wait_task_desc		wait;
	/**
	 * 由文件系统层自由使用。
	 */
	void			*private;
};


#define KIOCB_SYNC_KEY		(~0U)

/**
 * 文件读写控制块是否为同步读写。
 * 如果是，那么，即使在异步读写函数(如aio_read)中
 * 也必须等待读写任务完成。
 */
#define is_sync_kiocb(aio)	((aio)->key == KIOCB_SYNC_KEY)
#define init_async_io(x, filp)			\
	do {						\
		struct task_desc *tsk = current;	\
		(x)->users = 1;			\
		(x)->key = KIOCB_SYNC_KEY;		\
		(x)->file = (filp);			\
		(x)->obj.tsk = tsk;			\
		(x)->user_data = 0;                  \
		init_wait_task((&(x)->wait));             \
	} while (0)

extern ssize_t wait_on_async_io(struct async_io_desc *aio);

#endif
