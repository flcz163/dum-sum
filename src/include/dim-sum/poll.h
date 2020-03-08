#ifndef __DIM_SUM_POLL_H
#define __DIM_SUM_POLL_H

#include <dim-sum/wait.h>
#include <uapi/linux/poll.h>

struct poll_table_struct;
struct file;

/* 
 * structures and helpers for file_ops->poll implementations
 */
typedef void (*poll_queue_proc)(struct file *, struct wait_queue *, struct poll_table_struct *);

typedef struct poll_table_struct {
	poll_queue_proc qproc;
} poll_table;

/* These are specified by iBCS2 */
#define POLLIN		0x0001
#define POLLPRI		0x0002
#define POLLOUT		0x0004
#define POLLERR		0x0008
#define POLLHUP		0x0010
#define POLLNVAL	0x0020

/* The rest seem to be more-or-less nonstandard. Check them! */
#define POLLRDNORM	0x0040
#define POLLRDBAND	0x0080
#define POLLWRNORM	0x0100
#define POLLWRBAND	0x0200
#define POLLMSG		0x0400
#define POLLREMOVE	0x1000

struct pollfd {
	int fd;
	short events;
	short revents;
};

#endif /* __DIM_SUM_POLL_H */
