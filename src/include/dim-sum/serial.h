#ifndef __DIM_SUM_SERIAL_H
#define __DIM_SUM_SERIAL_H

#define ASYNC_CLOSING_WAIT_NONE	65535

/*
 * 所支持的串口类型
 */
#define PORT_UNKNOWN	0
#define PORT_MAX	1

#ifdef __KERNEL__

struct serial_struct {
	int	type;
	int	line;
	unsigned int	port;
	int	irq;
	int	flags;
	int	xmit_fifo_size;
	int	custom_divisor;
	int	baud_base;
	unsigned short	close_delay;
	char	io_type;
	char	reserved_char[1];
	int	hub6;
	unsigned short	closing_wait; /* time to wait before closing */
	unsigned short	closing_wait2; /* no longer used... */
	unsigned char	*iomem_base;
	unsigned short	iomem_reg_shift;
	unsigned int	port_high;
	unsigned long	iomap_base;	/* cookie passed into ioremap */
};


/*
 * Serial input interrupt line counters -- external structure
 * Four lines can interrupt: CTS, DSR, RI, DCD
 */
struct serial_icounter_struct {
	int cts, dsr, rng, dcd;
	int rx, tx;
	int frame, overrun, parity, brk;
	int buf_overrun;
	int reserved[9];
};


/* Export to allow PCMCIA to use this - Dave Hinds */
extern int register_serial(struct serial_struct *req);
extern void unregister_serial(int line);
#endif /* __KERNEL__ */

#endif /* __DIM_SUM_SERIAL_H */
