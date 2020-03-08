#ifndef __DIM_SUM_WAIT_H
#define __DIM_SUM_WAIT_H

#include <dim-sum/double_list.h>
#include <dim-sum/smp_lock.h>
#include <dim-sum/stddef.h>
#include <dim-sum/time.h>
#include <dim-sum/timer.h>
#include <uapi/linux/wait.h>

#include <asm/current.h>

struct wait_task_desc;
struct task_desc;
struct wait_queue;

typedef int (*wakeup_task_func_f)(struct wait_task_desc *wait,
	unsigned mode, int flags, void *data);
int wakeup_one_task_simple(struct wait_task_desc *wait, unsigned mode, int flags, void *data);

#define WQ_FLAG_EXCLUSIVE	0x01

struct wait_task_desc {
	struct task_desc	*task;
	unsigned int		flags;
	wakeup_task_func_f	wakeup;
	struct double_list	list;
};

struct wait_queue {
	struct smp_lock		lock;
	struct double_list	awaited;
};

#define __WAIT_TASK_INITIALIZER(name, tsk) {				\
	.task	= tsk,						\
	.wakeup		= wakeup_one_task_simple,			\
	.list	= LIST_HEAD_INITIALIZER((name).list) }

#define __WAIT_QUEUE_INITIALIZER(name) {				\
	.lock		= SMP_LOCK_UNLOCKED(name.lock),		\
	.awaited	= LIST_HEAD_INITIALIZER((name).awaited) }

#define init_wait_task(wait)							\
	do {								\
		(wait)->task = current;				\
		(wait)->wakeup = wakeup_one_task;		\
		list_init(&(wait)->list);			\
		(wait)->flags = 0;					\
	} while (0)

extern void __init_waitqueue(struct wait_queue *q, const char *name);

#define init_waitqueue(q)				\
	do {						\
		__init_waitqueue((q), #q);	\
	} while (0)

static inline int waitqueue_active(struct wait_queue *q)
{
	return !list_is_empty(&q->awaited);
}

extern void add_to_wait_queue(struct wait_queue *q, struct wait_task_desc *wait);
extern void del_from_wait_queue(struct wait_queue *q, struct wait_task_desc *wait);

static inline void __add_to_wait_queue(struct wait_queue *head, struct wait_task_desc *new)
{
	list_insert_front(&new->list, &head->awaited);
}

static inline void __add_to_wait_queue_tail(struct wait_queue *head,
					 struct wait_task_desc *new)
{
	list_insert_behind(&new->list, &head->awaited);
}

static inline void
__del_from_wait_queue(struct wait_queue *head, struct wait_task_desc *old)
{
	list_del(&old->list);
}

void __wake_up(struct wait_queue *q, unsigned int mode, int nr, void *key);

#define wake_up(x)			__wake_up(x, TASK_NORMAL, 1, NULL)
#define wake_up_interruptible(x)	__wake_up(x, TASK_INTERRUPTIBLE, 1, NULL)

#define DEFINE_WAIT(name) 				\
	struct wait_task_desc name = {						\
		.task	= current,				\
		.wakeup		= wakeup_one_task,				\
		.list	= LIST_HEAD_INITIALIZER((name).list),	\
	}

#define cond_wait(wq, condition)			\
do {									\
	DEFINE_WAIT(__wait);			\
	if (condition)	 						\
		break;							\
	for (;;) {							\
		prepare_to_wait(&wq, &__wait, TASK_UNINTERRUPTIBLE);	\
		if (condition)						\
			break;						\
		schedule();						\
	}								\
	finish_wait(&wq, &__wait);					\
} while (0)

#define cond_wait_interruptible(wq, condition)				\
({									\
	int __ret = 0;							\
	DEFINE_WAIT(__wait);						\
										\
	if (!(condition)) {						\
		for (;;) {							\
			prepare_to_wait(&wq, &__wait, TASK_INTERRUPTIBLE);	\
			if (condition)						\
				break;						\
			if (!signal_pending(current)) {				\
				schedule();					\
				continue;					\
			}							\
			__ret = -ERESTARTSYS;					\
			break;							\
		}								\
		finish_wait(&wq, &__wait);					\
	}								\
									\
	__ret;								\
})

#define cond_wait_timeout(wq, condition, timeout)			\
({									\
	DEFINE_WAIT(__wait);						\
	long __ret = timeout;						\
	if (!(condition)) { 						\
		for (;;) {							\
			prepare_to_wait(&wq, &__wait, TASK_UNINTERRUPTIBLE);	\
			if (condition)						\
				break;						\
			__ret = schedule_timeout(__ret);				\
			if (!__ret)						\
				break;						\
		}								\
		finish_wait(&wq, &__wait);					\
	}								\
	__ret;								\
})

#define cond_wait_interruptible_timeout(wq, condition, timeout)	\
({									\
	long __ret = timeout;						\
	DEFINE_WAIT(__wait);						\
									\
	if (!(condition)) {						\
		for (;;) {							\
			prepare_to_wait(&wq, &__wait, TASK_INTERRUPTIBLE);	\
			if (condition)						\
				break;						\
			if (!signal_pending(current)) {				\
				__ret = schedule_timeout(__ret);			\
				if (!__ret)					\
					break;					\
				continue;					\
			}							\
			__ret = -ERESTARTSYS;					\
			break;							\
		}								\
		finish_wait(&wq, &__wait);					\
	}								\
									\
	__ret;								\
})

#define cond_wait_interruptible_exclusive(wq, condition)		\
({									\
	int __ret = 0;							\
	DEFINE_WAIT(__wait);						\
									\
	if (!(condition)) {						\
		for (;;) {							\
			prepare_to_wait_exclusive(&wq, &__wait,			\
						TASK_INTERRUPTIBLE);		\
			if (condition)						\
				break;						\
			if (!signal_pending(current)) {				\
				schedule();					\
				continue;					\
			}							\
			__ret = -ERESTARTSYS;					\
			break;							\
		}								\
		finish_wait(&wq, &__wait);					\
	}									\
	__ret;								\
})

void prepare_to_wait(struct wait_queue *q, struct wait_task_desc *wait, int state);
void prepare_to_wait_exclusive(struct wait_queue *q, struct wait_task_desc *wait, int state);
void finish_wait(struct wait_queue *q, struct wait_task_desc *wait);
void abort_exclusive_wait(struct wait_queue *q, struct wait_task_desc *wait, unsigned int mode, void *key);
int wakeup_one_task(struct wait_task_desc *wait, unsigned mode, int sync, void *key);

struct wait_bit_key {
	void *addr;
	int bit_nr;
};

struct bit_wait_task_desc {
	struct wait_task_desc wait;
	unsigned long timeout;
	void *addr;
	int bit_nr;
};

typedef int wait_bit_schedule_f(struct bit_wait_task_desc *);
void __wake_up_locked_key(struct wait_queue *q, unsigned int mode, void *key);
void __wake_up_bit(struct wait_queue *, void *, int);
int __wait_on_bit(struct wait_queue *, struct bit_wait_task_desc *, wait_bit_schedule_f *, unsigned);
int __wait_on_bit_lock(struct wait_queue *, struct bit_wait_task_desc *, wait_bit_schedule_f *, unsigned);
void wake_up_bit(void *, int);
struct wait_queue *bit_waitqueue(void *, int);
int wakeup_one_task_bit(struct wait_task_desc *wait, unsigned mode, int sync, void *key);


#define __WAIT_BIT_KEY_INITIALIZER(word, bit)				\
	{ .addr = word, .bit_nr = bit, }

#define DEFINE_WAIT_BIT(name, word, bit)				\
	struct bit_wait_task_desc name = {					\
		.wait	= {						\
			.task	= current,			\
			.wakeup		= wakeup_one_task_bit,		\
			.list	=				\
				LIST_HEAD_INITIALIZER((name).wait.list),	\
		},							\
		.addr = word,				\
		.bit_nr = bit,							\
	}

extern int sched_bit_wait(struct bit_wait_task_desc *);
extern int iosched_bit_wait(struct bit_wait_task_desc *);
extern int sched_timeout_bit_wait(struct bit_wait_task_desc *);
extern int bit_wait_io_timeout(struct bit_wait_task_desc *);

/**
 * 通常的位图等待函数
 */
static inline int
wait_on_bit(unsigned long *word, int bit, unsigned mode)
{
	struct wait_queue *wq = bit_waitqueue(word, bit);
	DEFINE_WAIT_BIT(wait, word, bit);

	might_sleep();

	/**
	 * 很幸运，位图竟然可用
	 */
	if (!test_bit(bit, word))
		return 0;


	return __wait_on_bit(wq, &wait, sched_bit_wait, mode);
}

/**
 * 超时等待位图
 */
static inline int
wait_on_bit_timeout(void *word, int bit, unsigned mode, unsigned long timeout)
{
	struct wait_queue *wq = bit_waitqueue(word, bit);
	DEFINE_WAIT_BIT(wait, word, bit);

	might_sleep();

	if (!test_bit(bit, word))
		return 0;

	wait.timeout = jiffies + timeout;
	return __wait_on_bit(wq, &wait, sched_timeout_bit_wait, mode);
}

/**
 * 等待位图可用，并锁住
 */
static inline int
wait_on_bit_lock(void *word, int bit, unsigned mode)
{
	struct wait_queue *wq = bit_waitqueue(word, bit);
	DEFINE_WAIT_BIT(wait, word, bit);

	might_sleep();

	if (!atomic_test_and_set_bit(bit, word))
		return 0;

	return __wait_on_bit_lock(wq, &wait, sched_bit_wait, mode);
}

/**
 * 由于IO的原因，等待位图
 */
static inline int
wait_on_bit_io(void *word, int bit, unsigned mode)
{
	struct wait_queue *wq = bit_waitqueue(word, bit);
	DEFINE_WAIT_BIT(wait, word, bit);

	might_sleep();

	if (!test_bit(bit, word))
		return 0;

	return __wait_on_bit(wq, &wait, iosched_bit_wait, mode);
}

/**
 * 由于IO的原因，等待位图并锁住
 */
static inline int
wait_on_bit_lock_io(void *word, int bit, unsigned mode)
{
	struct wait_queue *wq = bit_waitqueue(word, bit);
	DEFINE_WAIT_BIT(wait, word, bit);

	might_sleep();
	if (!atomic_test_and_set_bit(bit, word))
		return 0;

	return __wait_on_bit_lock(wq, &wait, iosched_bit_wait, mode);
}

#endif /* __DIM_SUM_WAIT_H */
