#include <dim-sum/delay.h>
#include <dim-sum/smp.h>
#include <dim-sum/sched.h>
#include <dim-sum/timer.h>
#include <dim-sum/timer_device.h>

/**
 * 每个CPU上的定时器队列
 */
static struct cpu_timer_queue cpu_timers[MAX_CPUS];
struct cpu_timer_queue dummy_timer_queue;

static struct cpu_timer_queue *
lock_timer_queue(struct timer *timer, unsigned long *flags)
{
	struct cpu_timer_queue *queue;

	for (;;) {
		queue = timer->queue;
		if (likely(queue != NULL)) {
			smp_lock_irqsave(&queue->lock, *flags);
			if (likely(queue == timer->queue))
				return queue;

			smp_unlock_irqrestore(&queue->lock, *flags);
		}
		cpu_relax();
	}
}

static void unlock_timer_queue(struct timer *timer, unsigned long flags)
{
	smp_unlock_irqrestore(&timer->queue->lock, flags);
}

static void __timer_insert(struct cpu_timer_queue *queue, struct timer *timer)
{
	struct rb_node **link = &queue->rbroot.rb_node;
	struct rb_node *parent = NULL, *rbprev = NULL;
	struct timer *entry;

	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct timer, rbnode);

		if (timer->expire < entry->expire)
			link = &(*link)->rb_left;
		else {
			rbprev = parent;
			link = &(*link)->rb_right;
		}
	}
	rb_link_node(&timer->rbnode, parent, link);
	rb_insert_color(&timer->rbnode, &queue->rbroot);

	if (rbprev) {
		entry = rb_entry(rbprev, struct timer, rbnode);
		list_insert_front(&timer->list, &entry->list);
	} else
		list_insert_front(&timer->list, &queue->timers);

	timer->flag |= TIMER_INQUEUE;
}

static void __timer_dequeue(struct timer *timer)
{
	rb_erase(&timer->rbnode, &timer->queue->rbroot);
	list_del(&timer->list);
	timer->flag &= ~TIMER_INQUEUE;
}

int timer_init(struct timer *timer)
{
	memset(timer, 0, sizeof(*timer));
	timer->queue = &dummy_timer_queue;
	timer->flag = TIMER_FREE;

	return 0;
}

void timer_add(struct timer * timer)
{
	unsigned long flag;
	int cpu = smp_processor_id();
	struct cpu_timer_queue *queue = &cpu_timers[cpu];

	smp_lock_irqsave(&queue->lock, flag);
	timer->queue = queue;
	__timer_insert(queue, timer);
	timer->flag &= ~TIMER_FREE;
	smp_unlock_irqrestore(&queue->lock, flag);
}

int timer_rejoin(struct timer *timer, u64 expire)
{
	unsigned long flag;

	timer->expire = expire;
	lock_timer_queue(timer, &flag);
	if (timer->flag & TIMER_INQUEUE)
		__timer_dequeue(timer);
	unlock_timer_queue(timer, flag);

	timer_add(timer);

	return 0;
}

int timer_remove(struct timer *timer)
{
	unsigned long flag;

	lock_timer_queue(timer, &flag);
	if (unlikely(timer->flag & TIMER_FREE))
		goto out;
	if (timer->flag & TIMER_INQUEUE)
		__timer_dequeue(timer);
	timer->flag |= TIMER_FREE;

out:
	unlock_timer_queue(timer, flag);
	return 0;
}

int synchronize_timer_del(struct timer *timer)
{
	timer_remove(timer);

	while (timer->flag & TIMER_RUNING) {
		cpu_relax();
		preempt_check_resched();
	}

	return 0;
}

/**
 * 在时钟中断中调用，运行当前CPU上的定时器
 */
static void run_local_timer(void)
{
	unsigned long flag;
	int cpu = smp_processor_id();
	struct cpu_timer_queue *queue = &cpu_timers[cpu];
	u64 now = get_jiffies_64();
	struct timer *timer;

again:
	smp_lock_irqsave(&queue->lock, flag);
	while (likely(!list_is_empty(&queue->timers))) {
		timer = list_container(queue->timers.next, struct timer, list);

		if (timer->expire > now)
			break;
	
		__timer_dequeue(timer);
		timer->flag |= TIMER_RUNING;

		smp_unlock_irqrestore(&queue->lock, flag);
		if (timer->flag & TIMER_OPEN_IRQ) {
			enable_irq();
			timer->handle(timer->data);
			disable_irq();
		} else
			timer->handle(timer->data);
		smp_lock_irqsave(&queue->lock, flag);

		if (!(timer->flag & TIMER_INQUEUE)) {
			if (!(timer->flag & TIMER_FREE)) {
				if (timer->flag & TIMER_PERIODIC) {
					timer->expire += timer->period;
					__timer_insert(queue, timer);
				} else
					timer->flag |= TIMER_FREE;
			}
		}
		timer->flag &= ~TIMER_RUNING;
	}

	/**
	 * 在运行过程中，可能有新的定时器到期
	 */
	if (likely(!list_is_empty(&queue->timers))) {
		timer = list_container(queue->timers.next, struct timer, list);

		now = get_jiffies_64();
		if (timer->expire <= now) {
			smp_unlock_irqrestore(&queue->lock, flag);
			goto again;
		}
	}

	smp_unlock_irqrestore(&queue->lock, flag);
}

/**
 * 高精度定时器的中断处理函数
 */
void hrtimer_interrupt(struct timer_device *dev)
{
	unsigned long counter;

	if (smp_processor_id()  == 0)
	{
		add_jiffies_64(1);
	}

	run_local_timer();

	counter = ns_to_timer_counter(dev, NSEC_PER_SEC / HZ);
	dev->trigger_timer(counter, dev);
}

void init_timer(void)
{
	int i;

	for (i = 0; i < MAX_CPUS; i++) {
		smp_lock_init(&cpu_timers[i].lock);
		list_init(&cpu_timers[i].timers);
	}

	smp_lock_init(&dummy_timer_queue.lock);
	list_init(&dummy_timer_queue.timers);
}
