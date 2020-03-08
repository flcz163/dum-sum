#ifndef __DIM_SUM_TIMER_H
#define __DIM_SUM_TIMER_H

#include <dim-sum/double_list.h>
#include <dim-sum/magic.h>
#include <dim-sum/rbtree.h>
#include <kapi/dim-sum/time.h>

struct timer_device;
struct cpu_timer_queue;

enum {
	/**
	 * 运行时打开中断
	 */
	__TIMER_OPEN_IRQ,
	/**
	 * 一次性定时器，已经被运行了
	 */
	__TIMER_FREE,
	/**
	 * 正在在等待队列中
	 */
	__TIMER_INQUEUE,
	/**
	 * 定时器正在运行
	 */
	__TIMER_RUNING,
	/**
	 * 一次性定时器
	 */
	__TIMER_ONESHOT,
	/**
	 * 周期性定时器
	 */
	__TIMER_PERIODIC,
	
};

/**
 * 定时器标志
 */
#define TIMER_FREE			(1UL << __TIMER_FREE)
#define TIMER_RUNING		(1UL << __TIMER_RUNING)
#define TIMER_ONESHOT		(1UL << __TIMER_ONESHOT)
#define TIMER_PERIODIC		(1UL << __TIMER_PERIODIC)
#define TIMER_INQUEUE		(1UL << __TIMER_INQUEUE)
#define TIMER_OPEN_IRQ		(1UL << __TIMER_OPEN_IRQ)

/**
 * 每个CPU上的定时器队列描述符
 */
struct cpu_timer_queue
{
	/**
	 * 保护本队列的锁
	 */
	struct smp_lock	lock;
	/**
	 * 队列上的定时器链表头
	 */
	struct double_list timers;
	/**
	 * 定时器根节点
	 */
	struct rb_root rbroot;
};

/**
 * 定时器描述符
 */
struct timer
{
	/**
	 * 魔法值，探测其是否被破坏
	 */
	unsigned long magic;
	/**
	 * 标志，如TIMER_RUNING
	 */
	unsigned int flag;
	/**
	 * 所属CPU队列
	 */
	struct cpu_timer_queue *queue;
	/**
	 * 通过此节点，将定时器加入到队列红黑树中
	 */
	struct rb_node rbnode;
	/**
	 * 通过此节点，将定时器加入到队列链表中
	 */
	struct double_list list;
	/**
	 * 定时器到期时间
	 */
	u64	expire;
	/**
	 * 对于周期性定时器来，表示该定时器的周期
	 */
	u64	period;
	/**
	 * 定时器回调函数
	 */
	int (*handle)(void *data);
	/**
	 * 定时器回调参数
	 */
	void *data;
};

extern struct cpu_timer_queue dummy_timer_queue;
#define TIMER_INITIALIZER(_function, _expires, _data) {		\
		.magic = TIMER_MAGIC,				\
		.queue = &dummy_timer_queue,					\
		.expires = (_expires),				\
		.handle = (_function),			\
		.data = (_data),				\
	}

int timer_init(struct timer *timer);
/**
 * 将动态定时器插入到合适的链表中。
 */
void timer_add(struct timer * timer);
int timer_rejoin(struct timer *timer, u64 expire);
int timer_remove(struct timer *timer);
/**
 * 同步等待定时器运行完毕，并从队列中移除
 */
extern int synchronize_timer_del(struct timer *timer);

void hrtimer_interrupt(struct timer_device *dev);

extern void init_timer(void);

#endif /* __DIM_SUM_TIMER_H */
