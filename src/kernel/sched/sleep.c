#include <dim-sum/delay.h>
#include <dim-sum/errno.h>
#include <dim-sum/sched.h>
#include <dim-sum/stacktrace.h>
#include <dim-sum/syscall.h>
#include <dim-sum/timer.h>
#include <dim-sum/uaccess.h>

static int process_timeout(void *data)
{
	struct task_desc *p = (struct task_desc *)data;

	wake_up_process(p);

	return 0;
}

signed long __sched schedule_timeout(signed long timeout)
{
	struct timer timer;
	unsigned long long start = 0;

	might_sleep();

	switch (timeout)
	{
	case MAX_SCHEDULE_TIMEOUT:
		schedule();
		goto out;
	/**
	 * 为了适合不等待获取信号量、消息队列的情况
	 */
	case 0:
		current->state = TASK_RUNNING;
		return 0;
	default:
		if (timeout < 0) {
			current->state = TASK_RUNNING;
			goto out;
		}
	}

	start = jiffies;
	timer_init(&timer);
	timer.data = (void*)current;
	timer.handle = &process_timeout;

	timer_rejoin(&timer, (unsigned long long)start + timeout);

	schedule();

	timer_remove(&timer);

	timeout = timeout - (signed long)(jiffies - start);

	return timeout < 1 ? 0 : timeout;

 out:
	return MAX_SCHEDULE_TIMEOUT;
}

long __sched io_schedule_timeout(long timeout)
{
	long ret;

	ret = schedule_timeout(timeout);

	return ret;
}

void __might_sleep(const char *file, const char* func, int line)
{
	if (preempt_count()) {
		WARN(preempt_count(), "try sleep at [%s/%s] line %d.\n",
			file, func, line);
		dump_stack();
	}
}

void msleep(unsigned int msecs)
{
	unsigned long timeout = msecs_to_jiffies(msecs) + 1;

	set_current_state(TASK_UNINTERRUPTIBLE);
	timeout = schedule_timeout(timeout);

	if (timeout)
		WARN("killed.\n");
}

unsigned long msleep_interruptible(unsigned int msecs)
{
	unsigned long timeout = msecs_to_jiffies(msecs) + 1;

	while (timeout && !signal_pending(current)) {
		set_current_state(TASK_INTERRUPTIBLE);
		timeout = schedule_timeout(timeout);
	}

	return jiffies_to_msecs(timeout);
}

/**
 * nanosleep系统调用的服务例程。
 * 将进程挂起直到指定的时间间隔用完。
 */
asmlinkage long sys_nanosleep(struct timespec __user *time, struct timespec __user *rmtp)
{
	struct timespec t;
	unsigned long expire;

	/**
	 * 首先调用copy_frome_user
	 * 将包含在timerspec结构中的值复制到局部变量t中。
	 */
	if (copy_from_user(&t, time, sizeof(t)))
		return -EFAULT;

	/**
	 * 假定是一个有效的延迟。
	 */
	if ((t.tv_nsec >= 1000000000L) || (t.tv_nsec < 0) || (t.tv_sec < 0))
		return -EINVAL;

	/**
	 * timespec_to_jiffies将t中的时间间隔转换成节拍数。
	 * 再加上t.tv_sec || t.tv_nsec，是为了保险起见。
	 * 即，计算出来的节拍数始终会被加一。
	 */
	expire = timespec_to_jiffies(&t) + (t.tv_sec || t.tv_nsec);
	/**
	 * schedule_timeout会调用动态定时器。实现进程的延时。
	 */
	current->state = TASK_INTERRUPTIBLE;
	/**
	 * 可能会返回一个剩余节拍数。
	 */
	expire = schedule_timeout(expire);

	return expire;
}
