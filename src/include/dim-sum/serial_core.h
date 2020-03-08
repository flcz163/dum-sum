#ifndef __DIM_SUM_SERIAL_CORE_H
#define __DIM_SUM_SERIAL_CORE_H

#include <dim-sum/circ_buf.h>
#include <dim-sum/console.h>
#include <dim-sum/smp_lock.h>
#include <dim-sum/sched.h>
#include <dim-sum/termios.h>
#include <dim-sum/semaphore.h>
#include <dim-sum/tty.h>

struct uart_port;
struct serial_struct;

/* number of characters left in xmit buffer before we ask for more */
#define WAKEUP_CHARS		256

struct uart_icount {
	__u32	cts;
	__u32	dsr;
	__u32	rng;
	__u32	dcd;
	__u32	rx;
	__u32	tx;
	__u32	frame;
	__u32	overrun;
	__u32	parity;
	__u32	brk;
	__u32	buf_overrun;
};

typedef unsigned int __bitwise__ upf_t;
typedef unsigned int __bitwise__ upstat_t;

#define UPF_BOOT_AUTOCONF	((__force upf_t) (1 << 28))

#define USF_CLOSING_WAIT_NONE	(~0U)
/*
 * This is the state information which is persistent across opens.
 * The low level driver must not to touch any elements contained
 * within.
 */
struct uart_state {
	unsigned int		close_delay;		/* msec */
	unsigned int		closing_wait;		/* msec */

	int			count;
	int			pm_state;
	struct uart_info	*info;
	struct uart_port	*port;

	struct semaphore	sem;
};

#define UART_CONFIG_TYPE	(1 << 0)
#define UART_CONFIG_IRQ		(1 << 1)

/*
 * These are the flags that specific to info->flags, and reflect our
 * internal state.  They can not be accessed via port->flags.  Low
 * level drivers must not change these, but may query them instead.
 */
#define UIF_CHECK_CD		(1 << 25)
#define UIF_CTS_FLOW		(1 << 26)
#define UIF_NORMAL_ACTIVE	(1 << 29)
#define UIF_INITIALIZED		(1 << 31)

/*
 * This is the state information which is only valid when the port
 * is open; it may be freed by the core driver once the device has
 * been closed.  Either the low level driver or the core can modify
 * stuff here.
 */
struct uart_info {
	struct tty_struct	*tty;
	struct circ_buf		xmit;
	unsigned int		flags;

	int			blocked_open;
	
	struct wait_queue	open_wait;
	struct wait_queue	delta_msr_wait;
};

/*
 * This structure describes all the operations that can be
 * done on the physical hardware.
 */
struct uart_ops {
	unsigned int	(*tx_empty)(struct uart_port *);
	void		(*set_mctrl)(struct uart_port *, unsigned int mctrl);
	unsigned int	(*get_mctrl)(struct uart_port *);
	void		(*stop_tx)(struct uart_port *, unsigned int tty_stop);
	void		(*start_tx)(struct uart_port *, unsigned int tty_start);
	void		(*send_xchar)(struct uart_port *, char ch);
	void		(*stop_rx)(struct uart_port *);
	void		(*enable_ms)(struct uart_port *);
	void		(*break_ctl)(struct uart_port *, int ctl);
	int		(*startup)(struct uart_port *);
	void		(*flush_buffer)(struct uart_port *);
	void		(*shutdown)(struct uart_port *);
	void		(*set_termios)(struct uart_port *, struct termios *new,
				       struct termios *old);
	void		(*pm)(struct uart_port *, unsigned int state,
			      unsigned int oldstate);
	int		(*set_wake)(struct uart_port *, unsigned int state);

	/*
	 * Return a string describing the type of the port
	 */
	const char *(*type)(struct uart_port *);

	/*
	 * Release IO and memory resources used by the port.
	 * This includes iounmap if necessary.
	 */
	void		(*release_port)(struct uart_port *);

	/*
	 * Request IO and memory resources used by the port.
	 * This includes iomapping the port if necessary.
	 */
	int		(*request_port)(struct uart_port *);
	void		(*config_port)(struct uart_port *, int);
	int		(*verify_port)(struct uart_port *, struct serial_struct *);
	int		(*ioctl)(struct uart_port *, unsigned int, unsigned long);
};

struct uart_port {
	struct smp_lock		lock;			/* port lock */
	unsigned int		iobase;			/* in/out[bwl] */
	unsigned char __iomem	*membase;		/* read/write[bwl] */
	unsigned int		irq;			/* irq number */
	unsigned int		uartclk;		/* base uart clock */
	unsigned char		fifosize;		/* tx fifo size */
	unsigned char		x_char;			/* xon/xoff char */
	unsigned char		regshift;		/* reg offset shift */
	unsigned char		iotype;			/* io access style */

#define UPIO_PORT		(0)
#define UPIO_HUB6		(1)
#define UPIO_MEM		(2)
#define UPIO_MEM32		(3)

	unsigned int		read_status_mask;	/* driver specific */
	unsigned int		ignore_status_mask;	/* driver specific */
	struct uart_info	*info;			/* pointer to parent info */
	struct uart_icount	icount;			/* statistics */

	struct console		*cons;			/* struct console, if any */

	unsigned int		flags;

	unsigned int		mctrl;			/* current modem ctrl settings */
	unsigned int		timeout;		/* character-based timeout */
	unsigned int		type;			/* port type */
	struct uart_ops		*ops;
	unsigned int		custom_divisor;
	unsigned int		line;			/* port index */
	unsigned long		mapbase;		/* for ioremap */
	struct device		*dev;			/* parent device */
	unsigned char		hub6;			/* this should be in the 8250 driver */
	unsigned char		unused[3];
};

#define UART_XMIT_SIZE	PAGE_SIZE

struct tty_driver;

struct uart_driver {
	const char		*driver_name;
	const char		*dev_name;
	int			 major;
	int			 minor;
	int			 nr;
	struct console		*cons;

	/*
	 * these are private; the low level driver should not
	 * touch these; they should be initialised to NULL
	 */
	struct uart_state	*state;
	struct tty_driver	*tty_driver;
	const char	*sysobj_name;
};

void uart_console_write(struct uart_port *port, const char *s,
			unsigned int count,
			void (*putchar)(struct uart_port *, int));

#define uart_circ_empty(circ)		((circ)->head == (circ)->tail)
#define uart_circ_clear(circ)		((circ)->head = (circ)->tail = 0)

#define uart_circ_chars_pending(circ)	\
	(CIRC_CNT((circ)->head, (circ)->tail, UART_XMIT_SIZE))

#define uart_circ_chars_free(circ)	\
	(CIRC_SPACE((circ)->head, (circ)->tail, UART_XMIT_SIZE))

#define uart_tx_stopped(port)		\
	((port)->info->tty->stopped || (port)->info->tty->hw_stopped)

void uart_write_wakeup(struct uart_port *port);

int uart_register_driver(struct uart_driver *uart);
void uart_unregister_driver(struct uart_driver *uart);

#define UPF_SAK			(1 << 2)
/*
 * We do the SysRQ and SAK checking like this...
 */
static inline int uart_handle_break(struct uart_port *port)
{
	struct uart_info *info = port->info;
	if (info->flags & UPF_SAK)
		do_SAK(info->tty);
	return 0;
}

extern void uart_insert_char(struct uart_port *port, unsigned int status,
		 unsigned int overrun, unsigned int ch, unsigned int flag);


/**
 *	uart_handle_dcd_change - handle a change of carrier detect state
 *	@port: uart_port structure for the open port
 *	@status: new carrier detect status, nonzero if active
 */
static inline void
uart_handle_dcd_change(struct uart_port *port, unsigned int status)
{
	struct uart_info *info = port->info;

	port->icount.dcd++;

#ifdef CONFIG_HARD_PPS
	if ((port->flags & UPF_HARDPPS_CD) && status)
		hardpps();
#endif

	if (info->flags & UIF_CHECK_CD) {
		if (status)
			wake_up_interruptible(&info->open_wait);
		else if (info->tty)
			tty_hangup(info->tty);
	}
}

/**
 *	uart_handle_cts_change - handle a change of clear-to-send state
 *	@port: uart_port structure for the open port
 *	@status: new clear to send status, nonzero if active
 */
static inline void
uart_handle_cts_change(struct uart_port *port, unsigned int status)
{
	struct uart_info *info = port->info;
	struct tty_struct *tty = info->tty;

	port->icount.cts++;

	if (info->flags & UIF_CTS_FLOW) {
		if (tty->hw_stopped) {
			if (status) {
				tty->hw_stopped = 0;
				port->ops->start_tx(port, 0);
				uart_write_wakeup(port);
			}
		} else {
			if (!status) {
				tty->hw_stopped = 1;
				port->ops->stop_tx(port, 0);
			}
		}
	}
}

unsigned int uart_get_baud_rate(struct uart_port *port, struct termios *termios,
				struct termios *old, unsigned int min,
				unsigned int max);
void uart_update_timeout(struct uart_port *port, unsigned int cflag,
			 unsigned int baud);

struct tty_driver *uart_console_device(struct console *co, int *index);
void uart_parse_options(char *options, int *baud, int *parity, int *bits,
			int *flow);
int uart_set_options(struct uart_port *port, struct console *co, int baud,
		     int parity, int bits, int flow);
/*
 *	UART_ENABLE_MS - determine if port should enable modem status irqs
 */
#define UART_ENABLE_MS(port,cflag)	((port)->flags & UPF_HARDPPS_CD || \
					 (cflag) & CRTSCTS || \
					 !((cflag) & CLOCAL))

#define PORT_AMBA	32

#define UPF_FOURPORT		(1 << 1)
#define UPF_SPD_MASK		(0x1030)
#define UPF_SPD_HI		(0x0010)
#define UPF_SPD_VHI		(0x0020)
#define UPF_SPD_CUST		(0x0030)
#define UPF_SPD_SHI		(0x1000)
#define UPF_SPD_WARP		(0x1010)
#define UPF_SKIP_TEST		(1 << 6)
#define UPF_AUTO_IRQ		(1 << 7)
#define UPF_HARDPPS_CD		(1 << 11)
#define UPF_LOW_LATENCY		(1 << 13)
#define UPF_BUGGY_UART		(1 << 14)
#define UPF_AUTOPROBE		(1 << 15)
#define UPF_MAGIC_MULTIPLIER	(1 << 16)
#define UPF_BOOT_ONLYMCA	(1 << 22)
#define UPF_CONS_FLOW		(1 << 23)
#define UPF_SHARE_IRQ		(1 << 24)
#define UPF_IOREMAP		(1 << 31)

#define UPF_CHANGE_MASK		(0x17fff)
#define UPF_USR_MASK		(UPF_SPD_MASK|UPF_LOW_LATENCY)


int uart_add_one_port(struct uart_driver *reg, struct uart_port *port);
int uart_remove_one_port(struct uart_driver *reg, struct uart_port *port);

#endif /* __DIM_SUM_SERIAL_CORE_H */
