#ifndef __DIM_SUM_CONSOLE_H
#define __DIM_SUM_CONSOLE_H

#include <dim-sum/types.h>

struct tty_struct;

enum {
	/**
	 * 最简单的初期控制台，一般只能输出
	 * 一旦有正式的控制台，就注销
	 */
	__CONSOLE_SIMPLE,
	/**
	 * 可以用于printk打印
	 */
	__CONSOLE_PRINTK,
	/**
	 * 功能齐全，用起来趁手的控制台。
	 */
	__CONSOLE_PREFERRED,
	/**
	 * 成功初始化，可用
	 */
	__CONSOLE_ENABLED,
};
/**
 * 控制台设备标志位
 */
#define CONSOLE_SIMPLE	(1UL << __CONSOLE_SIMPLE)
#define CONSOLE_PRINTK	(1UL << __CONSOLE_PRINTK)
#define CONSOLE_PREFERRED	(1UL << __CONSOLE_PREFERRED)
#define CONSOLE_ENABLED	(1UL << __CONSOLE_ENABLED)

/**
 * 控制台描述符
 */
struct console {
	char	name[16];
	/**
	 * 控制台标志，如CONSOLE_SIMPLE
	 */
	short	flags;
	/**
	 * 控制台索引，同一个驱动可能管理多个控制台
	 * 该索引表示控制台设备在驱动中的索引
	 */
	short	index;
	/**
	 * 控制台初始化函数，可能为NULL，表示无需初始化
	 * 返回0表示初始化成功
	 */
	int	(*setup)(struct console *, char *);
	/**
	 * 向控制台输出一串字符
	 */
	void	(*write)(struct console *, const char *, unsigned);
	/**
	 * 从控制台读取一串字符
	 */
	int	(*read)(struct console *, char *, unsigned);
	/**
	 * 得到控制台设备及其索引
	 */
	struct tty_driver *(*device)(struct console *, int *);
	/**
	 * 控制台终端标志
	 */
	int	cflag;
	/**
	 * 供驱动使用私有数据
	 */
	void	*data;
	/**
	 * 通过此字段将驱动链接到全局链表中
	 */
	struct double_list list;
};

extern int add_preferred_console(char *name, int idx, char *options);
extern void register_console(struct console *);
extern int unregister_console(struct console *);
extern void lock_console_devices(void);
extern void unlock_console_devices(void);
extern struct tty_driver *get_console_device(int *);

#endif /* __DIM_SUM_CONSOLE_H */
