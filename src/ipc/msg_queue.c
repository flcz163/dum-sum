#include <linux/compiler.h>
#include <dim-sum/beehive.h>
#include <dim-sum/double_list.h>
#include <dim-sum/msg_queue.h>
#include <dim-sum/mem.h>
#include <dim-sum/printk.h>
#include <asm/string.h>
#include <asm/current.h>
#include <dim-sum/irq.h>
#include <dim-sum/sched.h>
#include <dim-sum/semaphore.h>
#include <dim-sum/errno.h>

static void free_msgq_data(struct msg_queue *msgq)
{
	kfree(msgq->data);
}

static void free_msgq_nodes(struct msg_queue *msgq)
{
	kfree(msgq->nodes);
}

static void free_msgq_all(struct msg_queue *msgq)
{
	free_msgq_nodes(msgq);
	free_msgq_data(msgq);
}


#define ALIGNNUM 4 
static int msgalign(int size)
{
        return ((size+ALIGNNUM-1) & (~(ALIGNNUM-1)));
}

int  msg_queue_init(struct msg_queue *msgq, int max_msgs, int msglen, int opt )
{
	int size;
	struct msg_queue_node *node;
	char *data;
	int i;
	
	if ((max_msgs <= 0) || (msglen <=0))
		return -EINVAL;

	if(in_interrupt()) {
		printk("can't create MSGQ in_interrupt!\n");
		return -EINVAL;
	}

	memset(msgq, 0, sizeof(*msgq));

	size = msgalign(msglen) * max_msgs;
	msgq->data = kmalloc(size, PAF_KERNEL);
	if (msgq->data == NULL)
	{
		printk("Can't get mem for MSGQ");
		goto out2;
	}
	memset(msgq->data, 0, size);

	size = sizeof(struct msg_queue_node) * max_msgs;
	msgq->nodes = kmalloc(size, PAF_KERNEL);
	if (msgq->nodes == NULL)
	{
		printk("Can't get mem for MSGQ");
		goto out1;
	}
	memset(msgq->nodes, 0, size);
	
	msgq->option = opt;
	msgq->magic  = MSGQ_MAGIC;
	msgq->msg_size = msgalign(msglen);
	msgq->msg_num = max_msgs;
	msgq->msg_count = 0;

	smp_lock_init(&msgq->lock);
	list_init(&(msgq->msg_list));
	list_init(&(msgq->free_list));
	list_init(&(msgq->wait_recv_list));
	list_init(&(msgq->wait_send_list));

	node = (struct msg_queue_node*)(msgq->nodes);
	for (data = msgq->data, i = max_msgs-1; i >= 0; i--) {
		node->data = data;
		list_insert_behind(&node->list, &msgq->free_list);
		data += msgq->msg_size;
		node++;
	}

	accurate_set(&(msgq->refcount), 1);

	return 0;

out1:
	free_msgq_data(msgq);
out2:
	free_msgq_nodes(msgq);
	return -ENOMEM;
}

void msg_queue_destroy(struct msg_queue *msgq)
{
	if (msgq->magic != MSGQ_MAGIC)
		return;

	if (in_interrupt())
		return;
	
	msgq->magic = 0;

	if (accurate_dec_and_test_zero(&msgq->refcount))
	{
		free_msgq_all(msgq);
	}
}

int  msg_queue_send( struct msg_queue * msgq, char *msg, unsigned int msglen,
                           int wait, int pri )
{
	int ret = 0;
	unsigned long flags;
	struct double_list *list;
	struct msg_queue_node *node;
	struct double_list *list_wait;
	struct msg_queue_send_item wait_item;

	if ((msg == NULL)
		|| (msglen == 0))
	{
		return -1;
	}
		
	if (msgq == NULL)
	{
		return ERROR_MSGQID_INVALID;
	}

	if (msgq->magic != MSGQ_MAGIC)
	{
		return ERROR_MSGQID_INVALID;    
	}

	if (msgq->msg_size < msglen)
	{
		return ERROR_MSG_LENS_OVER;
	}

	if (in_interrupt() && wait)  /* 中断里面不能等待 */
	{
		return ERROR_MSGQ_IN_INTR;
	}

RETRY:
	smp_lock_irqsave(&msgq->lock, flags);
	if (!list_is_empty(&msgq->free_list))
	{
		list = msgq->free_list.next;
		node = list_container(list, struct msg_queue_node, list);
		list_del(list);
		memcpy(node->data, msg, msglen);
		node->size = msglen;
		msgq->msg_count++;
		
		list_insert_behind(list, &msgq->msg_list);

		if (!list_is_empty(&msgq->wait_recv_list))
		{
			struct msg_queue_recv_item *wait;
			struct task_desc *tsk;
			list_wait = msgq->wait_recv_list.next;
			wait = list_container(list_wait, struct msg_queue_recv_item, list);
			list_del(list_wait);
			list_wait->next = NULL;
			wait->flag |= MSGQ_TRY_AGAIN;

			tsk = wait->task;
			wake_up_process(tsk);
		}

		smp_unlock_irqrestore(&msgq->lock, flags);
	}
	else
	{
		if (in_interrupt())
		{
			smp_unlock_irqrestore(&msgq->lock, flags);

			return ERROR_MSGQ_IN_INTR;
		}
		else
		{
			if (wait)
			{
				current->state = TASK_INTERRUPTIBLE;
				wait_item.flag = MSGQ_WAIT;
				wait_item.task = current;
				wait_item.wait_msgq = msgq;

				//list_insert_behind(&wait_item.list, &msgq->wait_send_list);
				if (msgq->option & MSGQ_Q_PRIOITY)
				{
					struct double_list *list;
				    struct msg_queue_send_item *item;
				    int prio = current->sched_prio;

				    for(list = msgq->wait_send_list.prev; list != &(msgq->wait_send_list); list = list->prev)
				    {
				        item = list_container(list, struct msg_queue_send_item, list);
				        if(item->task->sched_prio <= prio)
				        {
				            list_insert_front(&wait_item.list, list);
				            goto out_list;
				        }
				    }
					list_insert_front(&wait_item.list, &msgq->wait_send_list);
				}
				else
				{
			    	list_insert_behind(&wait_item.list, &msgq->wait_send_list);
				}

out_list:
				accurate_inc(&msgq->refcount);
				smp_unlock_irqrestore(&msgq->lock, flags);
				wait = schedule_timeout(wait);

				smp_lock_irqsave(&msgq->lock, flags);

				if (accurate_dec_and_test_zero(&msgq->refcount))
				{
					smp_unlock_irqrestore(&msgq->lock, flags);
					
					free_msgq_all(msgq);

					return ERROR_MSGQ_IS_UNREFED;
				}
				else 
				if (wait_item.flag & MSGQ_TRY_AGAIN)
				{
					smp_unlock_irqrestore(&msgq->lock, flags);
					goto RETRY;
				}
				else if (wait_item.flag & MSGQ_DELETED)
				{
					smp_unlock_irqrestore(&msgq->lock, flags);

					return ERROR_MSGQ_IS_DELETED;
				}
				else if (wait)
				{
					list_del(&wait_item.list);
					smp_unlock_irqrestore(&msgq->lock, flags);

					goto RETRY;
				}
				else
				{
					list_del(&wait_item.list);
					smp_unlock_irqrestore(&msgq->lock, flags);

					return ERROR_MSGQ_TIMED_OUT;
				}
			}
			else
			{
				smp_unlock_irqrestore(&msgq->lock, flags);

				return ERROR_MSGQ_FULL;
			}
		}
	}

	return ret;
}

int  msg_queue_receive( struct msg_queue * msgq, char *msgbuf, unsigned int buflen,
                              int wait )
{
	int ret = 0;
	unsigned long flags;
	struct double_list *list;
	struct msg_queue_node *node;
	struct double_list *list_wait;
	struct msg_queue_recv_item wait_item;

	//hal_printf("func %s, line %d, time %d\n", __FUNCTION__, __LINE__, wait);
	if (msgbuf == NULL)
	{
		return -1;
	}
	
	if (msgq == NULL)
	{
		return ERROR_MSGQID_INVALID;
	}

    if (msgq->magic != MSGQ_MAGIC)
    {
        return ERROR_MSGQID_INVALID;    
    }

	if (buflen <= 0)
	{
		return ERROR_BUF_LENS_UNDER;
	}

	if (in_interrupt() && wait)  /* 中断里面不能等待 */
	{
		return ERROR_MSGQ_IN_INTR;
	}
	
RETRY:
	smp_lock_irqsave(&msgq->lock, flags);

	if (!list_is_empty(&msgq->msg_list))
	{
		int copy_size = buflen;
		list = msgq->msg_list.next;
		node = list_container(list, struct msg_queue_node, list);

		if (node->size < buflen)
			copy_size = node->size;
		
		list_del(list);
    	//list->next = NULL;

		memcpy(msgbuf, node->data, copy_size);
		
		list_insert_behind(list, &msgq->free_list);

		msgq->msg_count--;

		if (!list_is_empty(&msgq->wait_send_list) && !list_is_empty(&msgq->msg_list))
		{
			struct msg_queue_send_item *wait;
			struct task_desc *tsk;
			list_wait = msgq->wait_send_list.next;
			wait = list_container(list_wait, struct msg_queue_send_item, list);
			list_del(list_wait);
    		list_wait->next = NULL;
			wait->flag |= MSGQ_TRY_AGAIN;

			tsk = wait->task;
			wake_up_process(tsk);
		}

		ret = copy_size;
		smp_unlock_irqrestore(&msgq->lock, flags);
	}
	else
	{
		if (in_interrupt())
		{
			smp_unlock_irqrestore(&msgq->lock, flags);

			return ERROR_MSGQ_IN_INTR;
		}
		else
		{
			if (wait)
			{
				current->state = TASK_INTERRUPTIBLE;
				wait_item.flag = MSGQ_WAIT;
				wait_item.task = current;
				wait_item.wait_msgq = msgq;
				//list_insert_behind(&wait_item.list, &msgq->wait_recv_list);
				if (msgq->option & MSGQ_Q_PRIOITY)
				{
					struct double_list *list;
				    struct msg_queue_recv_item *item;
				    int prio = current->sched_prio;

				    for(list = msgq->wait_recv_list.prev; list != &(msgq->wait_recv_list); list = list->prev)
				    {
				        item = list_container(list, struct msg_queue_recv_item, list);
				        if(item->task->sched_prio <= prio)
				        {
				            list_insert_front(&wait_item.list, list);
				            goto out_list;
				        }
				    }
					list_insert_front(&wait_item.list, &msgq->wait_recv_list);
				}
				else
				{
			    	list_insert_behind(&wait_item.list, &msgq->wait_recv_list);
				}

out_list:
				accurate_inc(&msgq->refcount);
				smp_unlock_irqrestore(&msgq->lock, flags);
				wait = schedule_timeout(wait);

				smp_lock_irqsave(&msgq->lock, flags);

				if (accurate_dec_and_test_zero(&msgq->refcount))
				{
					smp_unlock_irqrestore(&msgq->lock, flags);
					free_msgq_all(msgq);
					return ERROR_MSGQ_IS_UNREFED;
				}
				else 
				if (wait_item.flag & MSGQ_TRY_AGAIN)
				{
					smp_unlock_irqrestore(&msgq->lock, flags);
					goto RETRY;
				}
				else if (wait_item.flag & MSGQ_DELETED)
				{
					smp_unlock_irqrestore(&msgq->lock, flags);

					return ERROR_MSGQ_IS_DELETED;
				}
				else if (wait)
				{
					list_del(&wait_item.list);
					smp_unlock_irqrestore(&msgq->lock, flags);
					goto RETRY;
				}
				else
				{
					list_del(&wait_item.list);
					smp_unlock_irqrestore(&msgq->lock, flags);

					return ERROR_MSGQ_TIMED_OUT;
				}
			}
			else
			{
				smp_unlock_irqrestore(&msgq->lock, flags);
				return ERROR_MSGQ_EMPTY;
			}
		}	
	}
	return ret;
}

int msg_queue_count(struct msg_queue * msgq)
{
	if (msgq == NULL)
	{
		return -1;
	}

	if (msgq->magic != MSGQ_MAGIC)
	{
		return ERROR_MSGQID_INVALID;    
	}

	return msgq->msg_count;
}
