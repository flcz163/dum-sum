#ifndef __DIM_SUM_MSG_QUEUE_H
#define __DIM_SUM_MSG_QUEUE_H

#include <dim-sum/magic.h>
#include <dim-sum/smp_lock.h>

#define MSGQ_TRY_AGAIN 1
#define MSGQ_WAIT 2
#define MSGQ_DELETED 3

#define MSGQ_Q_FIFO 0
#define MSGQ_Q_PRIOITY 1

#define ERROR_MSGQID_INVALID -1
#define ERROR_MSGQ_IN_INTR    -2
#define ERROR_MSGQ_IS_UNREFED -3
#define ERROR_MSGQ_IS_DELETED -4
#define ERROR_MSGQ_TIMED_OUT  -5
#define ERROR_MSG_LENS_OVER  -6
#define ERROR_BUF_LENS_UNDER  -7
#define ERROR_MSGQ_FULL       -8
#define ERROR_MSGQ_EMPTY      -9

struct msg_queue;
struct msg_queue_recv_item{
	unsigned int flag;
    struct msg_queue  *wait_msgq;             /* 正在等待的SEM  */
    struct task_desc  *task;
    struct double_list list;     /* 挂入信号量的等待队列  */
};
struct msg_queue_send_item{
	unsigned int flag;
    struct msg_queue  *wait_msgq;             /* 正在等待的SEM  */
    struct task_desc  *task;
    struct double_list list;     /* 挂入信号量的等待队列  */
};
struct msg_queue_node {
	struct double_list list;
	char	*data;
	int		size;
};
struct msg_queue {
	int option;
	u32  magic;
	int msg_size;
	int msg_num;
	char *data;
	char *nodes;
	int	msg_count;
	struct double_list msg_list;
	struct double_list free_list;
	struct smp_lock lock;
	struct double_list wait_recv_list;
	struct double_list wait_send_list;
	struct accurate_counter refcount;
	struct double_list all_list;
};

int  msg_queue_init(struct msg_queue *msgq, int max_msgs, int msglen, int opt);
void msg_queue_destroy(struct msg_queue *msgq);
int  msg_queue_receive( struct msg_queue * queue, char *msgbuf, unsigned int buflen,
                              int wait );
int  msg_queue_send( struct msg_queue * queue, char *msg, unsigned int msglen,
                           int wait, int pri );
int msg_queue_count(struct msg_queue * queue);

#endif /* __DIM_SUM_MSG_QUEUE_H */
