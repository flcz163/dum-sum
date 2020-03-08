#include <dim-sum/console.h>
#include <dim-sum/irq.h>
#include <dim-sum/semaphore.h>
#include <dim-sum/smp.h>
#include <dim-sum/sched.h>
#include <dim-sum/wait.h>

/**
 * 默认的打印级别
 */
#define DEFAULT_MESSAGE_LOGLEVEL 4

/**
 * 最大/最小的打印级别
 */
#define MINIMUM_CONSOLE_LOGLEVEL 1
#define DEFAULT_CONSOLE_LOGLEVEL 7

/**
 * 控制台消息级别
 * 可以通过proc文件系统配置
 */
int printk_levels[4] = {
	/**
	 * 控制台打印级别
	 */
	DEFAULT_CONSOLE_LOGLEVEL,
	/**
	 * 默认消息打印级别
	 */
	DEFAULT_MESSAGE_LOGLEVEL,
	/**
	 * 最小的控制台打印级别
	 */
	MINIMUM_CONSOLE_LOGLEVEL,
	/**
	 * 默认控制台日志级别
	 */
	DEFAULT_CONSOLE_LOGLEVEL,
};
extern int printk_levels[];

#define console_loglevel DEFAULT_CONSOLE_LOGLEVEL
#define default_message_loglevel DEFAULT_MESSAGE_LOGLEVEL
#define minimum_console_loglevel MINIMUM_CONSOLE_LOGLEVEL
#define default_console_loglevel DEFAULT_CONSOLE_LOGLEVEL

/**
 * 保护控制台设备的锁
 */
static struct semaphore devices_sem = 
	SEMAPHORE_INITIALIZER(devices_sem, 1);
/**
 * 所有控制台设备的全局链表
 */
static struct double_list console_devices =
	LIST_HEAD_INITIALIZER(console_devices);

/**
 * 保护消息缓冲区的锁
 */
static struct smp_lock buf_lock = 
			SMP_LOCK_UNLOCKED(buf_lock);
#define MSG_BUF_LEN	(1 << CONFIG_LOG_BUF_SHIFT)
#define MSG_BUF_MASK	(MSG_BUF_LEN-1)
#define MSG_BUF(idx) (msg_buf[(idx) & MSG_BUF_MASK])
/**
 * 打印消息缓冲区
 */
static char msg_buf[MSG_BUF_LEN];
/**
 * 保存格式化消息的缓冲区
 */
static char format_buf[1024];

/**
 * 当前输出级别
 */
static int cur_msg_level = -1;
/**
 * 消息在缓冲区中的输出位置
 */
static unsigned long msg_head;
static unsigned long msg_tail;
/**
 * 已经输出到控制台的位置
 */
static unsigned long write_pos;

/**
 * 首选的控制台
 * 通过boot参数解析确定
 */
struct preferred_console
{
	/**
	 * 控制台名称
	 */
	char	name[8];
	/**
	 * 设备编号
	 */
	int	index;
	/**
	 * 指定的参数项，可能会传递给驱动
	 * 目前未用
	 */
	char	*options;			/* Options for the driver   */
};
/**
 * 所有指定的控制台命令行
 * 只有在此列表中的控制台，才能真正生效
 */
#define MAX_PREFERRED_CONSOLES 8
static struct preferred_console preferred_consoles[MAX_PREFERRED_CONSOLES];

/**
 * 获得控制台设备链表的锁
 */
void lock_console_devices(void)
{
	if (in_interrupt())
		BUG();

	down(&devices_sem);
}

static void call_console_drivers(unsigned long start, unsigned long end)
{
	struct console *con;
	struct double_list *list;

	list_for_each(list, &console_devices) {
		con = list_container(list, struct console, list);

		if ((con->flags & CONSOLE_ENABLED) && con->write)
			con->write(con, &MSG_BUF(start), end - start);
	}
}

static void write_line(unsigned long start,
				unsigned long end, int msg_log_level)
{
	if (msg_log_level < console_loglevel &&
			!list_is_empty(&console_devices) && start != end) {
		/**
		 * 缓冲区回绕了，分两次输出
		 */
		if ((start & MSG_BUF_MASK) > (end & MSG_BUF_MASK)) {
			call_console_drivers(start & MSG_BUF_MASK, MSG_BUF_LEN);
			call_console_drivers(0, end & MSG_BUF_MASK);
		} else
			call_console_drivers(start, end);
	}
}

static void write_bulk(unsigned long start, unsigned long end)
{
	unsigned long cur_index, start_print;
	static int msg_level = -1;

	/**
	 * 此时不能判断start > end
	 * 需要考虑整形溢出
	 */
	if (((long)(start - end)) > 0)
		return;

	cur_index = start;
	start_print = start;
	while (cur_index != end) {
		if (msg_level < 0 && ((end - cur_index) > 2)
		     && MSG_BUF(cur_index + 0) == '<'
		     && MSG_BUF(cur_index + 1) >= '0'
		     && MSG_BUF(cur_index + 1) <= '7'
		     && MSG_BUF(cur_index + 2) == '>') {
			msg_level = MSG_BUF(cur_index + 1) - '0';
			cur_index += 3;
			start_print = cur_index;
		}

		while (cur_index != end) {
			char c = MSG_BUF(cur_index);
			cur_index++;

			if (c == '\n') {
				/**
				 * 当缓冲区被截断的时候，可能丢失了消息级别
				 * 此时用默认值
				 */
				if (msg_level < 0)
					msg_level = default_message_loglevel;

				write_line(start_print, cur_index, msg_level);

				/**
				 * 重置消息级别以及打印位置
				 */
				msg_level = -1;
				start_print = cur_index;
				break;
			}
		}
	}

	write_line(start_print, end, msg_level);
}

/**
 * 调用此函数时，必须执行控制台链表的信号量
 * 及缓冲区的锁
 */
static inline void __printk(unsigned long flags)
{
	unsigned long start, end;

	for ( ; ; ) {
		if (write_pos == msg_tail)
			break;
		start = write_pos;
		end = msg_tail;
		write_pos = msg_tail;
		smp_unlock_irqrestore(&buf_lock, flags);
		/**
		 * 调用控制台驱动，输出字符
		 */
		write_bulk(start, end);
		smp_lock_irqsave(&buf_lock, flags);
	}
}

/**
 * 释放控制台链表的锁
 * 在释放锁之前，需要将缓冲区中的内容输出到控制台
 */
void unlock_console_devices(void)
{
	unsigned long flags;

	smp_lock_irqsave(&buf_lock, flags);
	__printk(flags);
	/**
	 * 这里不能遵循通常的AB-BA原则
	 * 否则会形成竞争条件
	 */
	up(&devices_sem);
	smp_unlock_irqrestore(&buf_lock, flags);
}

extern int vsprintf(char *buf, const char *fmt, va_list args);

static void advance_msg_buf(char ch)
{
	MSG_BUF(msg_tail) = ch;
	msg_tail++;
	/**
	 * 调用printk速度太快，来不及输出到控制台
	 */
	if (msg_tail - msg_head > MSG_BUF_LEN)
		msg_head = msg_tail - MSG_BUF_LEN;
	/**
	 * 此处直接丢弃了，其实可以记录下丢弃的字符数
	 */
	if (msg_tail - write_pos > MSG_BUF_LEN)
		write_pos = msg_tail - MSG_BUF_LEN;
}

/**
 * 内核打印主接口函数
 */
asmlinkage int printk(const char *fmt, ...)
{
	unsigned long flags;
	char *p;
	va_list args;
	int len;

	va_start(args, fmt);
	/**
	 * 开始向缓冲区输出字符
	 * 先获得缓冲区的锁
	 */
	smp_lock_irqsave(&buf_lock, flags);

	/**
	 * 将当前字符输出到临时缓冲区
	 */
	len = vscnprintf(format_buf, sizeof(msg_buf), fmt, args);

	/**
	 * 将当前的字符输出到缓冲区
	 */
	for (p = format_buf; *p; p++) {
		/**
		 * 如果是新行
		 */
		if (cur_msg_level < 0) {
			/**
			 * 如果没有指定，则用默认级别
			 */
			if (p[0] != '<' || p[1] < '0' || p[1] > '7' || p[2] != '>') {
				advance_msg_buf('<');
				advance_msg_buf(default_message_loglevel + '0');
				advance_msg_buf('>');
				cur_msg_level = default_message_loglevel;
			} else
				cur_msg_level = p[1] - '0';
		}

		advance_msg_buf(*p);
		if (*p == '\n')
			cur_msg_level = -1;
	}

	if (!down_trylock(&devices_sem)) {
		__printk(flags);
		up(&devices_sem);
		smp_unlock_irqrestore(&buf_lock, flags);
	} else
		/**
		 * 控制台信号量被占用，暂时放弃本次输出
		 * 将消息放到缓冲区后直接返回
		 */
		smp_unlock_irqrestore(&buf_lock, flags);

	va_end(args);

	return len;
}

static bool inline valid_preferred_slot(int idx)
{
	return (idx < MAX_PREFERRED_CONSOLES) && preferred_consoles[idx].name[0];
}

/**
 * 注册一个控制台
 */
void register_console(struct console * console)
{
	struct double_list *curr, *next;
	unsigned long flags;
	int i;

	if (console->index < 0)
		console->index = 0;

	/**
	 * 总是启用早期打印控制台
	 */
	if (console->flags & CONSOLE_SIMPLE) {
		/**
		 * 如果没有指定setup回调，说明此控制台不需要特别的初始化
		 * 如果指定了，就调用它
		 */
		if (!console->setup || !console->setup(console, NULL))
		    console->flags |= CONSOLE_ENABLED;
	} else
		/**
		 * 遍历所有允许的控制台设置
		 */
		for(i = 0; valid_preferred_slot(i); i++) {
			/**
			 * 比较控制台的名称和索引
			 */
			if (strcmp(preferred_consoles[i].name, console->name) != 0)
				continue;
			if (console->index != preferred_consoles[i].index)
				continue;

			/**
			 * 注册了setup回调并且初始化失败
			 */
			if (console->setup &&
			    console->setup(console, preferred_consoles[i].options) != 0)
				break;

			/**
			 * 设置成功标志并退出
			 */
			console->flags |= CONSOLE_ENABLED;
			console->flags |= CONSOLE_PREFERRED;

			break;
		}

	/**
	 * 打开不成功，或者不认可的控制台，退出
	 */
	if (!(console->flags & CONSOLE_ENABLED))
		return;

	/**
	 * 在插入全局链表前，先获得锁
	 */
	lock_console_devices();

	/**
	 * 有正式的控制台了
	 * 把early控制台删除
	 */
	if ((console->flags & CONSOLE_SIMPLE) != CONSOLE_SIMPLE)
		/**
		 * 遍历现有的控制台链表，并将early控制台删除
		 */
		list_for_each_safe(curr, next, &console_devices) {
			struct console * con = list_container(curr, struct console, list);

			if (con->flags & CONSOLE_SIMPLE)
				list_del_init(&con->list);
		}

	/**
	 * 把控制台插入到全局链表中
	 */
	list_insert_behind(&console->list, &console_devices);

	if (console->flags & CONSOLE_PRINTK) {
		smp_lock_irqsave(&buf_lock, flags);
		write_pos = msg_head;
		smp_unlock_irqrestore(&buf_lock, flags);
	}

	unlock_console_devices();
}

/**
 * 返回控制台tty设备及其索引
 */
struct tty_driver *get_console_device(int *index)
{
	struct console *con;
	struct tty_driver *driver = NULL;
	struct double_list *list;

	lock_console_devices();

	list_for_each(list, &console_devices) {
		con = list_container(list, struct console, list);

		if (!con->device)
			continue;

		driver = con->device(con, index);
		if (driver)
			break;
	}

	unlock_console_devices();
	return driver;
}

/**
 * 添加一个控制台到首选数组中
 */
int __init add_preferred_console(char *name, int idx, char *options)
{
	struct preferred_console *con;
	int i;

	for(i = 0; valid_preferred_slot(i); i++)
		if (!strncmp(preferred_consoles[i].name, name, sizeof(con->name))
			&& preferred_consoles[i].index == idx)
				return 0;

	if (i == MAX_PREFERRED_CONSOLES)
		return -E2BIG;

	con = &preferred_consoles[i];
	strncpy(con->name, name, sizeof(con->name));
	con->index = idx;
	con->options = options;

	return 0;
}

int __init init_console(void)
{
	add_preferred_console("ttyAMA", 0, "");

	return 0;
}
