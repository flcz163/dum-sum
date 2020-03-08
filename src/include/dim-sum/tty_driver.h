#ifndef __DIM_SUM_TTY_DRIVER_H
#define __DIM_SUM_TTY_DRIVER_H

#include <dim-sum/fs.h>
#include <dim-sum/cdev.h>

/**
 * tty设备驱动程序。
 */
struct tty_driver {
	/**
	 * 魔术值，为TTY_DRIVER_MAGIC。
	 */
	int	magic;		/* magic number for this structure */
	struct char_device cdev;
#if 0
	/**
	 * 驱动程序模块的所有者。
	 */
	struct module	*owner;
#endif
	/**
	 * 驱动程序的名字。在/proc/tty和sysfs中使用。
	 */
	const char	*driver_name;
	const char	*sysobj_name;
	/**
	 * 驱动程序节点的名字。
	 */
	const char	*name;
	/**
	 * 当创建设备时，开始使用的编号。
	 */
	int	name_base;	/* offset of printed name */
	/**
	 * 驱动程序的主设备号。
	 */
	int	major;		/* major device number */
	/**
	 * 起始次设备号。通常与name_base相同。默认为0.
	 */
	int	minor_start;	/* start of minor device number */
	/**
	 * 可以分配给驱动程序次设备号的个数。
	 */
	int	minor_num;	/* number of *possible* devices */
	int	num;		/* number of devices allocated */
	/**
	 * tty设备驱动类型。可能的值为:TTY_DRIVER_TYPE_SYSTEM，TTY_DRIVER_TYPE_CONSOLE等。
	 */
	short	type;		/* type of tty driver */
	short	subtype;	/* subtype of tty driver */
	/**
	 * 当被创建时，含有初始值的端口值。
	 */
	struct termios init_termios; /* Initial termios */
	int	flags;		/* tty driver flags */
	int	refcount;	/* for loadable tty drivers */
	/**
	 * 该驱动程序的/proc入口结构体。
	 */
	struct proc_dir_entry *proc_entry; /* /proc fs entry */
	/**
	 * 指向tty从属设备驱动程序的指针。只能被pty驱动使用。
	 */
	struct tty_driver *other; /* only used for the PTY driver */

	/*
	 * Pointer to the tty data structures
	 */
	struct tty_struct **ttys;
	struct termios **termios;
	struct termios **termios_locked;
	/**
	 * tty驱动程序内部的状态。只能被pty驱动使用。
	 */
	void *driver_state;	/* only used for the PTY driver */
	
	/*
	 * Interface routines from the upper tty layer to the tty
	 * driver.	Will be replaced with struct tty_operations.
	 */
	/**
	 * 打开tty设备、
	 */
	int  (*open)(struct tty_struct * tty, struct file * filp);
	/**
	 * 关闭tty设备。
	 */
	void (*close)(struct tty_struct * tty, struct file * filp);
	/**
	 * 向设备写入数据。如果没有定义put_char，那么所有写入都通过此函数。
	 * 因此，如果是由于没有定义put_char而引起的问题，则可能是write函数没有写入任何一个字符引起。
	 * 请确保每次至少写入一个字符。
	 */
	int  (*write)(struct tty_struct * tty,
		      const unsigned char *buf, int count);
	/**
	 * 向设备写入一个字符，例如回车换行符。
	 */
	void (*put_char)(struct tty_struct *tty, unsigned char ch);
	/**
	 * 要求驱动程序将数据发送给硬件。
	 */
	void (*flush_chars)(struct tty_struct *tty);
	/**
	 * 返回当前缓冲区可用数目。
	 */
	int  (*write_room)(struct tty_struct *tty);
	/**
	 * 这个函数用于返回在缓冲区中还有多少个需要传输的字符。
	 * 驱动可以不提供此函数。
	 */
	int  (*chars_in_buffer)(struct tty_struct *tty);
	int  (*ioctl)(struct tty_struct *tty, struct file * file,
		    unsigned int cmd, unsigned long arg);
	void (*set_termios)(struct tty_struct *tty, struct termios * old);
	void (*throttle)(struct tty_struct * tty);
	void (*unthrottle)(struct tty_struct * tty);
	void (*stop)(struct tty_struct *tty);
	void (*start)(struct tty_struct *tty);
	void (*hangup)(struct tty_struct *tty);
	void (*break_ctl)(struct tty_struct *tty, int state);
	/**
	 * 当tty驱动程序要刷新在其写缓冲区中的所有数据时，调用此函数。
	 */
	void (*flush_buffer)(struct tty_struct *tty);
	void (*set_ldisc)(struct tty_struct *tty);
	void (*wait_until_sent)(struct tty_struct *tty, int timeout);
	void (*send_xchar)(struct tty_struct *tty, char ch);
	int (*read_proc)(char *page_frame, char **start, off_t off,
			  int count, int *eof, void *data);
	int (*write_proc)(struct file *file, const char __user *buffer,
			  unsigned long count, void *data);
	int (*tiocmget)(struct tty_struct *tty, struct file *file);
	int (*tiocmset)(struct tty_struct *tty, struct file *file,
			unsigned int set, unsigned int clear);

	struct double_list tty_drivers;
};

/* tty driver magic number */
#define TTY_DRIVER_MAGIC		0x5402

/**
 * 由tty核心设置，表示驱动是否已经被安装。
 */
#define TTY_DRIVER_INSTALLED		0x0001
/**
 * 当设置此标志后，会在最后一个进程关闭设备时，tty核心对端口设置复位。
 */
#define TTY_DRIVER_RESET_TERMIOS	0x0002
/**
 * 表示tty驱动程序使用奇偶校验或者中断字符线程规程。
 * 这使得线路规程能以理论愉的方式接收字符，因为它不必检查从tty驱动程序那里接收的每一个字符。
 * 通常设置该位。
 */
#define TTY_DRIVER_REAL_RAW		0x0004
/**
 * 核心不需要为tty驱动程序创建任何devfs入口。
 * 这对于需要动态创建和删除次设备的驱动程序来说非常有用。
 */
#define TTY_DRIVER_NO_DEVFS		0x0008
#define TTY_DRIVER_DEVPTS_MEM		0x0010

/* tty driver types */
#define TTY_DRIVER_TYPE_SYSTEM		0x0001
#define TTY_DRIVER_TYPE_CONSOLE		0x0002
#define TTY_DRIVER_TYPE_SERIAL		0x0003
#define TTY_DRIVER_TYPE_PTY		0x0004
#define TTY_DRIVER_TYPE_SCC		0x0005	/* scc driver */
#define TTY_DRIVER_TYPE_SYSCONS		0x0006

/* system subtypes (magic, used by tty_io.c) */
#define SYSTEM_TYPE_TTY			0x0001
#define SYSTEM_TYPE_CONSOLE		0x0002

/* pty subtypes (magic, used by tty_io.c) */
#define PTY_TYPE_MASTER			0x0001
#define PTY_TYPE_SLAVE			0x0002

#define SERIAL_TYPE_NORMAL	1


struct tty_struct;

/**
 * tty回调函数。由驱动设置后，由tty核心调用。
 * 目前，该结构所包含的所有函数指针也包含在tty_driver中。
 */
struct tty_operations {
	int  (*open)(struct tty_struct * tty, struct file * filp);
	void (*close)(struct tty_struct * tty, struct file * filp);
	int  (*write)(struct tty_struct * tty,
		      const unsigned char *buf, int count);
	/**
	 * 当需要向设备写入单个字符时，调用此函数。
	 * 如果没有实现此函数，将调用write。
	 */
	void (*put_char)(struct tty_struct *tty, unsigned char ch);
	/**
	 * 向硬件发送数据。
	 */
	void (*flush_chars)(struct tty_struct *tty);
	/**
	 * 返回缓冲区中的剩余空间。
	 */
	int  (*write_room)(struct tty_struct *tty);
	/**
	 * 缓冲区中的字符数。
	 */
	int  (*chars_in_buffer)(struct tty_struct *tty);
	/**
	 * 当对设备节点调用ioctl时，该函数被tty核心调用。
	 */
	int  (*ioctl)(struct tty_struct *tty, struct file * file,
		    unsigned int cmd, unsigned long arg);
	/**
	 * 当设备的termios设置发生改变时，被tty核心调用。
	 */
	void (*set_termios)(struct tty_struct *tty, struct termios * old);
	/**
	 * 数据控制函数。用来控制并防止tty核心的输入缓冲区溢出。
	 * 当tty核心的输入缓冲区满的时候，调用throttle函数。这样设备不再向核心发送更多的字符。
	 */
	void (*throttle)(struct tty_struct * tty);
	void (*unthrottle)(struct tty_struct * tty);
	void (*stop)(struct tty_struct *tty);
	void (*start)(struct tty_struct *tty);
	/**
	 * 挂起设备。
	 */
	void (*hangup)(struct tty_struct *tty);
	/**
	 * 处理RS-232端口的BREAK线路状态。
	 */
	void (*break_ctl)(struct tty_struct *tty, int state);
	/**
	 * 刷新缓冲区，里面的数据将被丢失。
	 */
	void (*flush_buffer)(struct tty_struct *tty);
	/**
	 * 设置使用线路规程。通常不由驱动使用。
	 */
	void (*set_ldisc)(struct tty_struct *tty);
	void (*wait_until_sent)(struct tty_struct *tty, int timeout);
	/**
	 * 发送X类型字符函数。要发送的字符存放在ch中。
	 */
	void (*send_xchar)(struct tty_struct *tty, char ch);
	int (*read_proc)(char *page_frame, char **start, off_t off,
			  int count, int *eof, void *data);
	int (*write_proc)(struct file *file, const char __user *buffer,
			  unsigned long count, void *data);
	/**
	 * 获得、设置特定tty设备当前的线路设置。
	 */
	int (*tiocmget)(struct tty_struct *tty, struct file *file);
	int (*tiocmset)(struct tty_struct *tty, struct file *file,
			unsigned int set, unsigned int clear);
};


void tty_set_operations(struct tty_driver *driver, struct tty_operations *op);

struct tty_driver *alloc_tty_driver(int lines);
void put_tty_driver(struct tty_driver *driver);
extern int tty_ioctl(struct file_node * file_node, struct file * file, unsigned int cmd, unsigned long arg);

#endif /* __DIM_SUM_TTY_DRIVER_H */
