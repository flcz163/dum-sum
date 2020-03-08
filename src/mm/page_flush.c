#include <dim-sum/delay.h>
#include <dim-sum/fs.h>
#include <dim-sum/writeback.h>

/**
 * 回写任务描述符
 */
struct writeback_work {
	/**
	 * 执行回写的任务描述符
	 */
	struct task_desc *task;
	/**
	 * 回写任务的回调函数。
	 */
	void (*func)(unsigned long);	
	/**
	 * 给回调函数的参数。
	 */
	unsigned long arg0;
	/**
	 * 通过此字段将任务链接到全局链表work_list中
	 */
	struct double_list list;
};

static struct double_list free_list = LIST_HEAD_INITIALIZER(free_list);
static struct smp_lock work_lock = SMP_LOCK_UNLOCKED(work_lock);

/**
 * 激活空闲的回写线程。执行给定的回写任务
 */
int __kick_writeback_task(void (*func)(unsigned long), unsigned long arg0)
{
	unsigned long flags;
	int ret = 0;

	ASSERT(func != NULL);

	smp_lock_irqsave(&work_lock, flags);
	/**
	 * 没有空闲任务
	 */
	if (list_is_empty(&free_list)) {
		smp_unlock_irqrestore(&work_lock, flags);
		ret = -1;
	} else {
		struct writeback_work *work;

		/**
		 * 从空闲链表中取出第一个空闲线程。
		 */
		work = list_first_container(&free_list, struct writeback_work, list);
		list_del_init(&work->list);

		/**
		 * 设置内核线程的回调函数。
		 */
		work->func = func;
		work->arg0 = arg0;
		/**
		 * 唤醒空闲线程。
		 */
		wake_up_process(work->task);

		smp_unlock_irqrestore(&work_lock, flags);
	}

	return ret;
}

/**
 * 临时性回写任务
 */
static int writeback_task(void *dummy)
{
	struct writeback_work work;

	current->flags |= TASKFLAG_FLUSHER;
	work.func = NULL;
	work.task = current;
	list_init(&work.list);

	smp_lock_irq(&work_lock);
	while (1) {
		/**
		 *  将自己插入空闲链表，并开始睡眠。
		 */
		set_current_state(TASK_INTERRUPTIBLE);
		list_move_to_front(&work.list, &free_list);
		smp_unlock_irq(&work_lock);

		schedule();

		ASSERT(list_is_empty(&work.list));
		ASSERT(work.func != NULL);

		/**
		 * 执行回写任务
		 */
		work.func(work.arg0);

		smp_lock_irq(&work_lock);
		work.func = NULL;
	}

	smp_unlock_irq(&work_lock);

	return 0;
}

/**
 * 周期性回写任务
 */
static int writeback_damon(void *dummy)
{
	u64	last;

	while (1) {
		last = get_jiffies_64();

		writeback_period();
		msleep(2000 - (get_jiffies_64() - last) * 1000 / HZ);
	}

	return 0;
}

int __init pdflush_init(void)
{
	int i;

	for (i = 0; i < 4; i++)
		create_process(writeback_task,
				NULL,
				"writeback_task",
				10
			);

	create_process(writeback_damon,
			NULL,
			"writeback_damon",
			10
		);

	return 0;
}
