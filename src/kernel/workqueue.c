#include <dim-sum/beehive.h>
#include <dim-sum/workqueue.h>
#include <dim-sum/smp_lock.h>
#include <dim-sum/wait.h>
#include <dim-sum/cpumask.h>
#include <dim-sum/sched.h>
#include <dim-sum/err.h>
#include <dim-sum/cpu.h>
#include <dim-sum/smp.h>

/**
 * 每个CPU的工作队列描述符
 * 工作队列与可延迟函数的主要区别在于：工作队列运行在进程上下文，而可延迟函数运行在中断上下文。
 */
struct cpu_workqueue_struct {
	/**
	 * 保护该工作队列的自旋锁
	 * 虽然每个CPU都有自己的工作队列，但是有时候也需要访问其他CPU的工作队列
	 * 所以也需要自旋锁	 
	 */
	struct smp_lock lock;

	/**
	 * flush_workqueue使用的计数
	 */
	long remove_sequence;	/* Least-recently added (next to run) */
	long insert_sequence;	/* Next to add */

	/**
	 * 挂起链表的头结点
	 */
	struct double_list worklist;
	/**
	 * 等待队列，其中的工作者线程因为等待更多的工作而处于睡眠状态
	 */
	struct wait_queue more_work;
	/**
	 * 等待队列，其中的进程由于等待工作队列被刷新而处于睡眠状态
	 */
	struct wait_queue work_done;

	/**
	 * 指向workqueue_struct的指针，workqueue_struct中包含了本描述符
	 */
	struct workqueue_struct *wq;
	/**
	 * 指向工作者线程的进程描述符指针
	 */
	struct task_desc *thread;

	/**
	 * 当前的执行深度（当工作队列链表中的函数阻塞时，这个字段的值会比1大）
	 */
	int run_depth;		/* Detect run_workqueue() recursion depth */
} aligned_cacheline;

/**
 * 工作队列描述符，它包含有一个NR_CPUS个元素的数组。
 * 分多个队列的主要目的是每个CPU都有自己的工作队列，避免了多个CPU访问全局数据时，造成TLB不停刷新
 */
struct workqueue_struct {
	struct cpu_workqueue_struct cpu_wq[MAX_CPUS];
	const char *name;
	struct double_list list; 	/* Empty if single thread */
};

/* All the per-cpu workqueues on the system, for hotplug cpu to add/remove
   threads to each one as cpus come/go. */
static struct smp_lock workqueue_lock = 
			SMP_LOCK_UNLOCKED(workqueue_lock);

static struct double_list workqueues = LIST_HEAD_INITIALIZER(workqueues);

/* If it's single threaded, it isn't in the list of workqueues. */
static inline int is_single_threaded(struct workqueue_struct *wq)
{
	return list_is_empty(&wq->list);
}

/* Preempt must be disabled. */
static void __queue_work(struct cpu_workqueue_struct *cwq,
			 struct work_struct *work)
{
	unsigned long flags;

	smp_lock_irqsave(&cwq->lock, flags);
	work->wq_data = cwq;
	list_insert_behind(&work->entry, &cwq->worklist);
	cwq->insert_sequence++;
	wake_up(&cwq->more_work);
	smp_unlock_irqrestore(&cwq->lock, flags);
}

/*
 * Queue work on a workqueue. Return non-zero if it was successfully
 * added.
 *
 * We queue the work to the CPU it was submitted, but there is no
 * guarantee that it will be processed by that CPU.
 */
/**
 * queue_work把函数插入工作队列
 * wq-被插入的workqueue_struct描述符。
 * work-要插入的work_struct
 */
int queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	int ret = 0, cpu = get_cpu();

	/**
	 * test_and_set_bit判断work->pending是否为1
	 * 如果为1，表示函数已经插入到工作队列中，直接退出
	 * 否则，执行插入过程，并将标志设置为1。
	 */
	if (!atomic_test_and_set_bit(0, &work->pending)) {
		if (unlikely(is_single_threaded(wq)))
			cpu = 0;
		BUG_ON(!list_is_empty(&work->entry));
		/**
		 * 调用__queue_work将函数插入到工作队列中。
		 * 如果工作者线程在本地CPU的cpu_workqueue_struct的more_work上睡眠，则唤醒线程。
		 */
		__queue_work(wq->cpu_wq + cpu, work);
		ret = 1;
	}
	put_cpu();
	return ret;
}

static int delayed_work_timer_fn(void *__data)
{
	struct work_struct *work = (struct work_struct *)__data;
	struct workqueue_struct *wq = work->wq_data;
	int cpu = smp_processor_id();

	if (unlikely(is_single_threaded(wq)))
		cpu = 0;

	__queue_work(wq->cpu_wq + cpu, work);

	return 0;
}

int queue_delayed_work(struct workqueue_struct *wq,
			struct work_struct *work, unsigned long delay)
{
	int ret = 0;
	struct timer *timer = &work->timer;

	if (!atomic_test_and_set_bit(0, &work->pending)) {
		//BUG_ON(timer_pending(timer));
		BUG_ON(!list_is_empty(&work->entry));

		/* This stores wq for the moment, for the timer_fn */
		work->wq_data = wq;
		timer->expire = jiffies + delay;
		timer->data = work;
		timer->handle = delayed_work_timer_fn;
		timer_add(timer);
		ret = 1;
	}
	return ret;
}

static inline void run_workqueue(struct cpu_workqueue_struct *cwq)
{
	unsigned long flags;

	/*
	 * Keep taking off work from the queue until
	 * done.
	 */
	smp_lock_irqsave(&cwq->lock, flags);
	cwq->run_depth++;
	if (cwq->run_depth > 3) {
		/* morton gets to eat his hat */
		printk("%s: recursion depth exceeded: %d\n",
			__FUNCTION__, cwq->run_depth);
		//dump_stack();
	}
	while (!list_is_empty(&cwq->worklist)) {
		struct work_struct *work = list_container(cwq->worklist.next,
						struct work_struct, entry);
		void (*f) (void *) = work->func;
		void *data = work->data;

		list_del_init(cwq->worklist.next);
		smp_unlock_irqrestore(&cwq->lock, flags);

		BUG_ON(work->wq_data != cwq);
		atomic_clear_bit(0, &work->pending);
		f(data);

		smp_lock_irqsave(&cwq->lock, flags);
		cwq->remove_sequence++;
		wake_up(&cwq->work_done);
	}
	cwq->run_depth--;
	smp_unlock_irqrestore(&cwq->lock, flags);
}

static int worker_thread(void *__cwq)
{
	struct wait_task_desc wait = __WAIT_TASK_INITIALIZER(wait, current);
	struct cpu_workqueue_struct *cwq = __cwq;

	current->flags |= TASKFLAG_NOFREEZE;

	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		add_to_wait_queue(&cwq->more_work, &wait);
		if (list_is_empty(&cwq->worklist))
			schedule();
		else
			__set_current_state(TASK_RUNNING);
		del_from_wait_queue(&cwq->more_work, &wait);

		if (!list_is_empty(&cwq->worklist))
			run_workqueue(cwq);
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static void flush_cpu_workqueue(struct cpu_workqueue_struct *cwq)
{
	if (cwq->thread == current) {
		/*
		 * Probably keventd trying to flush its own queue. So simply run
		 * it by hand rather than deadlocking.
		 */
		run_workqueue(cwq);
	} else {
		DEFINE_WAIT(wait);
		long sequence_needed;

		smp_lock_irq(&cwq->lock);
		sequence_needed = cwq->insert_sequence;

		while (sequence_needed - cwq->remove_sequence > 0) {
			prepare_to_wait(&cwq->work_done, &wait,
					TASK_UNINTERRUPTIBLE);
			smp_unlock_irq(&cwq->lock);
			schedule();
			smp_lock_irq(&cwq->lock);
		}
		finish_wait(&cwq->work_done, &wait);
		smp_unlock_irq(&cwq->lock);
	}
}

/*
 * flush_workqueue - ensure that any scheduled work has run to completion.
 *
 * Forces execution of the workqueue and blocks until its completion.
 * This is typically used in driver shutdown handlers.
 *
 * This function will sample each workqueue's current insert_sequence number and
 * will sleep until the head sequence is greater than or equal to that.  This
 * means that we sleep until all works which were queued on entry have been
 * handled, but we are not livelocked by new incoming ones.
 *
 * This function used to run the workqueues itself.  Now we just wait for the
 * helper threads to do it.
 */
void fastcall flush_workqueue(struct workqueue_struct *wq)
{
	might_sleep();

	if (is_single_threaded(wq)) {
		/* Always use cpu 0's area. */
		flush_cpu_workqueue(wq->cpu_wq + 0);
	} else {
		int cpu;

		lock_cpu_hotplug();
		for_each_online_cpu(cpu)
			flush_cpu_workqueue(wq->cpu_wq + cpu);
		unlock_cpu_hotplug();
	}
}
	
static struct task_desc *create_workqueue_thread(struct workqueue_struct *wq,
						   int cpu)
{
	struct cpu_workqueue_struct *cwq = wq->cpu_wq + cpu;
	struct task_desc *p;

	smp_lock_init(&cwq->lock);
	cwq->wq = wq;
	cwq->thread = NULL;
	cwq->insert_sequence = 0;
	cwq->remove_sequence = 0;
	list_init(&cwq->worklist);
	init_waitqueue(&cwq->more_work);
	init_waitqueue(&cwq->work_done);

	if (is_single_threaded(wq))
		p = kthread_create(worker_thread, cwq, 30, "%s", wq->name);
	else
		p = kthread_create(worker_thread, cwq, 30, "%s/%d", wq->name, cpu);
	if (IS_ERR(p))
		return NULL;
	cwq->thread = p;
	return p;
}

struct workqueue_struct *__create_workqueue(const char *name,
					    int singlethread)
{
	int cpu, destroy = 0;
	struct workqueue_struct *wq;
	struct task_desc *p;

	BUG_ON(strlen(name) > 10);

	wq = kmalloc(sizeof(*wq), PAF_KERNEL);
	if (!wq)
		return NULL;
	memset(wq, 0, sizeof(*wq));

	wq->name = name;
	/* We don't need the distraction of CPUs appearing and vanishing. */
	lock_cpu_hotplug();
	if (singlethread) {
		list_init(&wq->list);
		p = create_workqueue_thread(wq, 0);
		if (!p)
			destroy = 1;
		else
			wake_up_process(p);
	} else {
		smp_lock(&workqueue_lock);
		list_insert_front(&wq->list, &workqueues);
		smp_unlock(&workqueue_lock);
		for_each_online_cpu(cpu) {
			p = create_workqueue_thread(wq, cpu);
			if (p) {
				kthread_bind(p, cpu);
				wake_up_process(p);
			} else
				destroy = 1;
		}
	}
	unlock_cpu_hotplug();

	/*
	 * Was there any error during startup? If yes then clean up:
	 */
	if (destroy) {
		destroy_workqueue(wq);
		wq = NULL;
	}
	return wq;
}

static void cleanup_workqueue_thread(struct workqueue_struct *wq, int cpu)
{
	struct cpu_workqueue_struct *cwq;
	unsigned long flags;
	struct task_desc *p;

	cwq = wq->cpu_wq + cpu;
	smp_lock_irqsave(&cwq->lock, flags);
	p = cwq->thread;
	cwq->thread = NULL;
	smp_unlock_irqrestore(&cwq->lock, flags);
	if (p)
		kthread_stop(p);
}

/**
 * 撤消工作队列
 */
void destroy_workqueue(struct workqueue_struct *wq)
{
	int cpu;

	flush_workqueue(wq);

	/* We don't need the distraction of CPUs appearing and vanishing. */
	lock_cpu_hotplug();
	if (is_single_threaded(wq)) {
		cleanup_workqueue_thread(wq, 0);
	} else {
		for_each_online_cpu(cpu) {
			cleanup_workqueue_thread(wq, cpu);
		}
		smp_lock(&workqueue_lock);
		list_del(&wq->list);
		smp_unlock(&workqueue_lock);
	}
	unlock_cpu_hotplug();
	kfree(wq);
}

static struct workqueue_struct *keventd_wq;

int fastcall schedule_work(struct work_struct *work)
{
	return queue_work(keventd_wq, work);
}

int fastcall schedule_delayed_work(struct work_struct *work, unsigned long delay)
{
	return queue_delayed_work(keventd_wq, work, delay);
}

int schedule_delayed_work_on(int cpu,
			struct work_struct *work, unsigned long delay)
{
	int ret = 0;
	struct timer *timer = &work->timer;

	if (!atomic_test_and_set_bit(0, &work->pending)) {
		//BUG_ON(timer_pending(timer));
		BUG_ON(!list_is_empty(&work->entry));
		/* This stores keventd_wq for the moment, for the timer_fn */
		work->wq_data = keventd_wq;
		timer->expire = jiffies + delay;
		timer->data = (void *)work;
		timer->handle = delayed_work_timer_fn;
		timer_add(timer);
		ret = 1;
	}
	return ret;
}

void flush_scheduled_work(void)
{
	flush_workqueue(keventd_wq);
}

int keventd_up(void)
{
	return keventd_wq != NULL;
}

int current_is_keventd(void)
{
	struct cpu_workqueue_struct *cwq;
	int cpu = smp_processor_id();	/* preempt-safe: keventd is per-cpu */
	int ret = 0;

	BUG_ON(!keventd_wq);

	cwq = keventd_wq->cpu_wq + cpu;
	if (current == cwq->thread)
		ret = 1;

	return ret;

}

void init_sleep_works(void)
{
	//hotcpu_notifier(workqueue_cpu_callback, 0);
	keventd_wq = create_workqueue("events");
	BUG_ON(!keventd_wq);
}
