#include <dim-sum/beehive.h>
#include <dim-sum/amba/bus.h>
#include <dim-sum/amba/serial.h>
#include <dim-sum/errno.h>
#include <dim-sum/init.h>
#include <dim-sum/irq.h>
#include <dim-sum/irq_mapping.h>
#include <dim-sum/serial.h>
#include <dim-sum/serial_core.h>
#include <dim-sum/workqueue.h>
#include <linux/mod_devicetable.h>
#include <asm/memory.h>
#include <asm/io.h>

#include <dim-sum/linkage.h>
#include <dim-sum/types.h>

#include <dim-sum/cache.h>
#include <dim-sum/of.h>
#include <dim-sum/printk.h>
#include <dim-sum/delay.h>
#include <asm/early_map.h>
#include <asm/io.h>

#define AMBA_ISR_PASS_LIMIT	256
#define UART_DUMMY_DR_RX	(1 << 16)


#define UART_DR_ERROR		(UART011_DR_OE|UART011_DR_BE|UART011_DR_PE|UART011_DR_FE)

static struct amba_id pl011_ids[];
static struct uart_driver amba_reg;
/*
 * We wrap our port structure around the generic uart_port.
 */
struct uart_amba_port {
	struct uart_port	port;
	struct clk		*clk;
	const struct vendor_data *vendor;
	unsigned int		dmacr;		/* dma control reg */
	unsigned int		im;		/* interrupt mask */
	unsigned int		old_status;
	unsigned int		fifosize;	/* vendor-specific */
	unsigned int		lcrh_tx;	/* vendor-specific */
	unsigned int		lcrh_rx;	/* vendor-specific */
	unsigned int		old_cr;		/* state during shutdown */
	struct work_struct tx_softirq_work;
	bool			autorts;
	unsigned int		tx_irq_seen;	/* 0=none, 1=1, 2=2 or more */
	char			type[12];
};

/* There is by now at least one vendor with differing details, so handle it */
struct vendor_data {
	unsigned int		ifls;
	unsigned int		lcrh_tx;
	unsigned int		lcrh_rx;
	bool			oversampling;
	bool			dma_threshold;
	bool			cts_event_workaround;

	unsigned int (*get_fifosize)(struct amba_device *dev);
};

#define UART_NR			14

static struct uart_amba_port *amba_ports[UART_NR];


/*
 * Transmit a character
 *
 * Returns true if the character was successfully queued to the FIFO.
 * Returns false otherwise.
 */
static bool pl011_tx_char(struct uart_amba_port *uap, unsigned char c)
{
	if (readw(uap->port.membase + UART01x_FR) & UART01x_FR_TXFF)
		return false; /* unable to transmit character */

	writew(c, uap->port.membase + UART01x_DR);
	uap->port.icount.tx++;

	return true;
}


/* Blank functions if the DMA engine is not available */
static inline void pl011_dma_probe(struct uart_amba_port *uap)
{
}

static inline void pl011_dma_remove(struct uart_amba_port *uap)
{
}

static inline void pl011_dma_startup(struct uart_amba_port *uap)
{
}

static inline void pl011_dma_shutdown(struct uart_amba_port *uap)
{
}

static inline bool pl011_dma_tx_irq(struct uart_amba_port *uap)
{
	return false;
}

static inline void pl011_dma_tx_stop(struct uart_amba_port *uap)
{
}

static inline bool pl011_dma_tx_start(struct uart_amba_port *uap)
{
	return false;
}

static inline void pl011_dma_rx_irq(struct uart_amba_port *uap)
{
}

static inline void pl011_dma_rx_stop(struct uart_amba_port *uap)
{
}

static inline int pl011_dma_rx_trigger_dma(struct uart_amba_port *uap)
{
	return -EIO;
}

static inline bool pl011_dma_rx_available(struct uart_amba_port *uap)
{
	return false;
}

static inline bool pl011_dma_rx_running(struct uart_amba_port *uap)
{
	return false;
}

#define pl011_dma_flush_buffer	NULL

static void pl011_stop_tx(struct uart_port *port, unsigned int tty_stop)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	uap->im &= ~UART011_TXIM;
	writew(uap->im, uap->port.membase + UART011_IMSC);
	pl011_dma_tx_stop(uap);
}

static bool pl011_tx_chars(struct uart_amba_port *uap)
{
	struct circ_buf *xmit = &uap->port.info->xmit;
	int count;

	if (unlikely(uap->tx_irq_seen < 2))
		/*
		 * Initial FIFO fill level unknown: we must check TXFF
		 * after each write, so just try to fill up the FIFO.
		 */
		count = uap->fifosize;
	else /* tx_irq_seen >= 2 */
		/*
		 * FIFO initially at least half-empty, so we can simply
		 * write half the FIFO without polling TXFF.

		 * Note: the *first* TX IRQ can still race with
		 * pl011_start_tx_pio(), which can result in the FIFO
		 * being fuller than expected in that case.
		 */
		count = uap->fifosize >> 1;

	/*
	 * If the FIFO is full we're guaranteed a TX IRQ at some later point,
	 * and can't transmit immediately in any case:
	 */
	if (unlikely(uap->tx_irq_seen < 2 &&
		     readw(uap->port.membase + UART01x_FR) & UART01x_FR_TXFF))
		return false;

	if (uap->port.x_char) {
		if (!pl011_tx_char(uap, uap->port.x_char))
			goto done;
		uap->port.x_char = 0;
		--count;
	}
	if (uart_circ_empty(xmit) || uart_tx_stopped(&uap->port)) {
		pl011_stop_tx(&uap->port, 0);
		goto done;
	}

	/* If we are using DMA mode, try to send some characters. */
	if (pl011_dma_tx_irq(uap))
		goto done;

	while (count-- > 0 && pl011_tx_char(uap, xmit->buf[xmit->tail])) {
		xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
		if (uart_circ_empty(xmit))
			break;
	}

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(&uap->port);

	if (uart_circ_empty(xmit)) {
		pl011_stop_tx(&uap->port, 0);
		goto done;
	}

	if (unlikely(!uap->tx_irq_seen))
		schedule_delayed_work(&uap->tx_softirq_work, uap->port.timeout);

done:
	return false;
}

static void pl011_tx_softirq(void *data)
{
	struct uart_amba_port *uap = data;

	smp_lock_irq(&uap->port.lock);
	while (pl011_tx_chars(uap)) ;
	smp_unlock_irq(&uap->port.lock);
}


static unsigned int pl011_tx_empty(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned int status = readw(uap->port.membase + UART01x_FR);
	return status & (UART01x_FR_BUSY|UART01x_FR_TXFF) ? 0 : TIOCSER_TEMT;
}

static void pl011_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned int cr;

	cr = readw(uap->port.membase + UART011_CR);

#define	TIOCMBIT(tiocmbit, uartbit)		\
	if (mctrl & tiocmbit)		\
		cr |= uartbit;		\
	else				\
		cr &= ~uartbit

	TIOCMBIT(TIOCM_RTS, UART011_CR_RTS);
	TIOCMBIT(TIOCM_DTR, UART011_CR_DTR);
	TIOCMBIT(TIOCM_OUT1, UART011_CR_OUT1);
	TIOCMBIT(TIOCM_OUT2, UART011_CR_OUT2);
	TIOCMBIT(TIOCM_LOOP, UART011_CR_LBE);

	if (uap->autorts) {
		/* We need to disable auto-RTS if we want to turn RTS off */
		TIOCMBIT(TIOCM_RTS, UART011_CR_RTSEN);
	}
#undef TIOCMBIT

	writew(cr, uap->port.membase + UART011_CR);
}

static unsigned int pl011_get_mctrl(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned int result = 0;
	unsigned int status = readw(uap->port.membase + UART01x_FR);

#define TIOCMBIT(uartbit, tiocmbit)	\
	if (status & uartbit)		\
		result |= tiocmbit

	TIOCMBIT(UART01x_FR_DCD, TIOCM_CAR);
	TIOCMBIT(UART01x_FR_DSR, TIOCM_DSR);
	TIOCMBIT(UART01x_FR_CTS, TIOCM_CTS);
	TIOCMBIT(UART011_FR_RI, TIOCM_RNG);
#undef TIOCMBIT
	return result;
}


/* Start TX with programmed I/O only (no DMA) */
static void pl011_start_tx_pio(struct uart_amba_port *uap)
{
	uap->im |= UART011_TXIM;
	writew(uap->im, uap->port.membase + UART011_IMSC);
	if (!uap->tx_irq_seen)
		pl011_tx_chars(uap);
}

static void pl011_start_tx(struct uart_port *port, unsigned int tty_start)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	if (!pl011_dma_tx_start(uap))
		pl011_start_tx_pio(uap);
}

static void pl011_stop_rx(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	uap->im &= ~(UART011_RXIM|UART011_RTIM|UART011_FEIM|
		     UART011_PEIM|UART011_BEIM|UART011_OEIM);
	writew(uap->im, uap->port.membase + UART011_IMSC);

	pl011_dma_rx_stop(uap);
}

static void pl011_enable_ms(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	uap->im |= UART011_RIMIM|UART011_CTSMIM|UART011_DCDMIM|UART011_DSRMIM;
	writew(uap->im, uap->port.membase + UART011_IMSC);
}

static void pl011_break_ctl(struct uart_port *port, int break_state)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned long flags;
	unsigned int lcr_h;

	smp_lock_irqsave(&uap->port.lock, flags);
	lcr_h = readw(uap->port.membase + uap->lcrh_tx);
	if (break_state == -1)
		lcr_h |= UART01x_LCRH_BRK;
	else
		lcr_h &= ~UART01x_LCRH_BRK;
	writew(lcr_h, uap->port.membase + uap->lcrh_tx);
	smp_unlock_irqrestore(&uap->port.lock, flags);
}

static int pl011_hwinit(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	//int retval;

	/* Optionaly enable pins to be muxed in and configured */
	//pinctrl_pm_select_default_state(port->dev);

	/*
	 * Try to enable the clock producer.
	 */
	//retval = clk_prepare_enable(uap->clk);
	//if (retval)
	//	return retval;

	uap->port.uartclk = 24000000;//clk_get_rate(uap->clk);

	/* Clear pending error and receive interrupts */
	writew(UART011_OEIS | UART011_BEIS | UART011_PEIS | UART011_FEIS |
	       UART011_RTIS | UART011_RXIS, uap->port.membase + UART011_ICR);

	/*
	 * Save interrupts enable mask, and enable RX interrupts in case if
	 * the interrupt is used for NMI entry.
	 */
	uap->im = readw(uap->port.membase + UART011_IMSC);
	writew(UART011_RTIM | UART011_RXIM, uap->port.membase + UART011_IMSC);

	if (dev_get_platdata(uap->port.dev)) {
		struct amba_pl011_data *plat;

		plat = dev_get_platdata(uap->port.dev);
		if (plat->init)
			plat->init();
	}
	return 0;
}

/*
 * Reads up to 256 characters from the FIFO or until it's empty and
 * inserts them into the TTY layer. Returns the number of characters
 * read from the FIFO.
 */
static int pl011_fifo_to_tty(struct uart_amba_port *uap)
{
	u16 status, ch;
	unsigned int flag, max_count = 256;
	int fifotaken = 0;

	while (max_count--) {
		status = readw(uap->port.membase + UART01x_FR);
		if (status & UART01x_FR_RXFE)
			break;

		/* Take chars from the FIFO and update status */
		ch = readw(uap->port.membase + UART01x_DR) |
			UART_DUMMY_DR_RX;
		flag = TTY_NORMAL;
		uap->port.icount.rx++;
		fifotaken++;

		if (unlikely(ch & UART_DR_ERROR)) {
			if (ch & UART011_DR_BE) {
				ch &= ~(UART011_DR_FE | UART011_DR_PE);
				uap->port.icount.brk++;
				if (uart_handle_break(&uap->port))
					continue;
			} else if (ch & UART011_DR_PE)
				uap->port.icount.parity++;
			else if (ch & UART011_DR_FE)
				uap->port.icount.frame++;
			if (ch & UART011_DR_OE)
				uap->port.icount.overrun++;

			ch &= uap->port.read_status_mask;

			if (ch & UART011_DR_BE)
				flag = TTY_BREAK;
			else if (ch & UART011_DR_PE)
				flag = TTY_PARITY;
			else if (ch & UART011_DR_FE)
				flag = TTY_FRAME;
		}

		//if (uart_handle_sysrq_char(&uap->port, ch & 255))
		//	continue;

		uart_insert_char(&uap->port, ch, UART011_DR_OE, ch, flag);
	}

	return fifotaken;
}

static void pl011_rx_chars(struct uart_amba_port *uap)
__releases(&uap->port.lock)
__acquires(&uap->port.lock)
{
	pl011_fifo_to_tty(uap);

	smp_unlock(&uap->port.lock);
	tty_flip_buffer_push(uap->port.info->tty);
	/*
	 * If we were temporarily out of DMA mode for a while,
	 * attempt to switch back to DMA mode again.
	 */
	if (pl011_dma_rx_available(uap)) {
		if (pl011_dma_rx_trigger_dma(uap)) {
			uap->im |= UART011_RXIM;
			writew(uap->im, uap->port.membase + UART011_IMSC);
		}
	}
	smp_lock(&uap->port.lock);
}


static void pl011_modem_status(struct uart_amba_port *uap)
{
	unsigned int status, delta;

	status = readw(uap->port.membase + UART01x_FR) & UART01x_FR_MODEM_ANY;

	delta = status ^ uap->old_status;
	uap->old_status = status;

	if (!delta)
		return;

	if (delta & UART01x_FR_DCD)
		uart_handle_dcd_change(&uap->port, status & UART01x_FR_DCD);

	if (delta & UART01x_FR_DSR)
		uap->port.icount.dsr++;

	if (delta & UART01x_FR_CTS)
		uart_handle_cts_change(&uap->port, status & UART01x_FR_CTS);

	wake_up_interruptible(&uap->port.info->delta_msr_wait);
}


static void pl011_tx_irq_seen(struct uart_amba_port *uap)
{
	if (likely(uap->tx_irq_seen > 1))
		return;

	uap->tx_irq_seen++;
	if (uap->tx_irq_seen < 2)
		/* first TX IRQ */
		cancel_delayed_work(&uap->tx_softirq_work);
}

static enum isr_result pl011_int(unsigned int irq, void *dev_id)
{
	struct uart_amba_port *uap = dev_id;
	unsigned long flags;
	unsigned int status, pass_counter = AMBA_ISR_PASS_LIMIT;
	int handled = 0;
	//unsigned int dummy_read;

	smp_lock_irqsave(&uap->port.lock, flags);
	status = readw(uap->port.membase + UART011_MIS);
	if (status) {
		do {
			if (uap->vendor->cts_event_workaround) {
				/* workaround to make sure that all bits are unlocked.. */
				writew(0x00, uap->port.membase + UART011_ICR);

				/*
				 * WA: introduce 26ns(1 uart clk) delay before W1C;
				 * single apb access will incur 2 pclk(133.12Mhz) delay,
				 * so add 2 dummy reads
				 */
				readw(uap->port.membase + UART011_ICR);
				readw(uap->port.membase + UART011_ICR);
			}

			writew(status & ~(UART011_TXIS|UART011_RTIS|
					  UART011_RXIS),
			       uap->port.membase + UART011_ICR);

			if (status & (UART011_RTIS|UART011_RXIS)) {
				if (pl011_dma_rx_running(uap))
					pl011_dma_rx_irq(uap);
				else
					pl011_rx_chars(uap);
			}
			if (status & (UART011_DSRMIS|UART011_DCDMIS|
				      UART011_CTSMIS|UART011_RIMIS))
				pl011_modem_status(uap);
			if (status & UART011_TXIS) {
				pl011_tx_irq_seen(uap);
				pl011_tx_chars(uap);
			}

			if (pass_counter-- == 0)
				break;

			status = readw(uap->port.membase + UART011_MIS);
		} while (status != 0);
		handled = 1;
	}

	smp_unlock_irqrestore(&uap->port.lock, flags);

	return handled ? ISR_EATEN : ISR_SKIP;
}

static int pl011_startup(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned int cr;
	int retval;

	retval = pl011_hwinit(port);
	if (retval)
		goto clk_dis;

	writew(uap->im, uap->port.membase + UART011_IMSC);

	/*
	 * Allocate the IRQ
	 */
	retval = register_isr_handle(33/* uap->port.irq */, pl011_int, 0, "uart-pl011", uap);
	if (retval)
		goto clk_dis;

	writew(uap->vendor->ifls, uap->port.membase + UART011_IFLS);

	/* Assume that TX IRQ doesn't work until we see one: */
	uap->tx_irq_seen = 0;

	smp_lock_irq(&uap->port.lock);

	/* restore RTS and DTR */
	cr = uap->old_cr & (UART011_CR_RTS | UART011_CR_DTR);
	cr |= UART01x_CR_UARTEN | UART011_CR_RXE | UART011_CR_TXE;
	writew(cr, uap->port.membase + UART011_CR);

	smp_unlock_irq(&uap->port.lock);

	/*
	 * initialise the old status of the modem signals
	 */
	uap->old_status = readw(uap->port.membase + UART01x_FR) & UART01x_FR_MODEM_ANY;

	/* Startup DMA */
	pl011_dma_startup(uap);

	/*
	 * Finally, enable interrupts, only timeouts when using DMA
	 * if initial RX DMA job failed, start in interrupt mode
	 * as well.
	 */
	smp_lock_irq(&uap->port.lock);
	/* Clear out any spuriously appearing RX interrupts */
	 writew(UART011_RTIS | UART011_RXIS,
		uap->port.membase + UART011_ICR);
	uap->im = UART011_RTIM;
	if (!pl011_dma_rx_running(uap))
		uap->im |= UART011_RXIM;
	writew(uap->im, uap->port.membase + UART011_IMSC);
	smp_unlock_irq(&uap->port.lock);

	return 0;

 clk_dis:
	//clk_disable_unprepare(uap->clk);
	return retval;
}


static void pl011_shutdown_channel(struct uart_amba_port *uap,
					unsigned int lcrh)
{
      unsigned long val;

      val = readw(uap->port.membase + lcrh);
      val &= ~(UART01x_LCRH_BRK | UART01x_LCRH_FEN);
      writew(val, uap->port.membase + lcrh);
}

static void pl011_shutdown(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned int cr;

	cancel_delayed_work(&uap->tx_softirq_work);

	/*
	 * disable all interrupts
	 */
	smp_lock_irq(&uap->port.lock);
	uap->im = 0;
	writew(uap->im, uap->port.membase + UART011_IMSC);
	writew(0xffff, uap->port.membase + UART011_ICR);
	smp_unlock_irq(&uap->port.lock);

	pl011_dma_shutdown(uap);

	/*
	 * Free the interrupt
	 */
	unregister_isr_handle(uap->port.irq, uap);

	/*
	 * disable the port
	 * disable the port. It should not disable RTS and DTR.
	 * Also RTS and DTR state should be preserved to restore
	 * it during startup().
	 */
	uap->autorts = false;
	smp_lock_irq(&uap->port.lock);
	cr = readw(uap->port.membase + UART011_CR);
	uap->old_cr = cr;
	cr &= UART011_CR_RTS | UART011_CR_DTR;
	cr |= UART01x_CR_UARTEN | UART011_CR_TXE;
	writew(cr, uap->port.membase + UART011_CR);
	smp_unlock_irq(&uap->port.lock);

	/*
	 * disable break condition and fifos
	 */
	pl011_shutdown_channel(uap, uap->lcrh_rx);
	if (uap->lcrh_rx != uap->lcrh_tx)
		pl011_shutdown_channel(uap, uap->lcrh_tx);

	/*
	 * Shut down the clock producer
	 */
	//clk_disable_unprepare(uap->clk);
	/* Optionally let pins go into sleep states */
	//pinctrl_pm_select_sleep_state(port->dev);

	if (dev_get_platdata(uap->port.dev)) {
		struct amba_pl011_data *plat;

		plat = dev_get_platdata(uap->port.dev);
		if (plat->exit)
			plat->exit();
	}

	if (uap->port.ops->flush_buffer)
		uap->port.ops->flush_buffer(port);
}

static void pl011_write_lcr_h(struct uart_amba_port *uap, unsigned int lcr_h)
{
	writew(lcr_h, uap->port.membase + uap->lcrh_rx);
	if (uap->lcrh_rx != uap->lcrh_tx) {
		int i;
		/*
		 * Wait 10 PCLKs before writing LCRH_TX register,
		 * to get this delay write read only register 10 times
		 */
		for (i = 0; i < 10; ++i)
			writew(0xff, uap->port.membase + UART011_MIS);
		writew(lcr_h, uap->port.membase + uap->lcrh_tx);
	}
}

static void
pl011_set_termios(struct uart_port *port, struct termios *termios,
		     struct termios *old)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned int lcr_h, old_cr;
	unsigned long flags;
	unsigned int baud, quot, clkdiv;

	if (uap->vendor->oversampling)
		clkdiv = 8;
	else
		clkdiv = 16;

	/*
	 * Ask the core to calculate the divisor for us.
	 */
	baud = uart_get_baud_rate(port, termios, old, 0,
				  port->uartclk / clkdiv);

	if (baud > port->uartclk/16)
		quot = DIV_ROUND_CLOSEST(port->uartclk * 8, baud);
	else
		quot = DIV_ROUND_CLOSEST(port->uartclk * 4, baud);

	switch (termios->c_cflag & CSIZE) {
	case CS5:
		lcr_h = UART01x_LCRH_WLEN_5;
		break;
	case CS6:
		lcr_h = UART01x_LCRH_WLEN_6;
		break;
	case CS7:
		lcr_h = UART01x_LCRH_WLEN_7;
		break;
	default: // CS8
		lcr_h = UART01x_LCRH_WLEN_8;
		break;
	}
	if (termios->c_cflag & CSTOPB)
		lcr_h |= UART01x_LCRH_STP2;
	if (termios->c_cflag & PARENB) {
		lcr_h |= UART01x_LCRH_PEN;
		if (!(termios->c_cflag & PARODD))
			lcr_h |= UART01x_LCRH_EPS;
	}
	if (uap->fifosize > 1)
		lcr_h |= UART01x_LCRH_FEN;

	smp_lock_irqsave(&port->lock, flags);

	/*
	 * Update the per-port timeout.
	 */
	uart_update_timeout(port, termios->c_cflag, baud);

	port->read_status_mask = UART011_DR_OE | 255;
	if (termios->c_iflag & INPCK)
		port->read_status_mask |= UART011_DR_FE | UART011_DR_PE;
	if (termios->c_iflag & (IGNBRK | BRKINT | PARMRK))
		port->read_status_mask |= UART011_DR_BE;

	/*
	 * Characters to ignore
	 */
	port->ignore_status_mask = 0;
	if (termios->c_iflag & IGNPAR)
		port->ignore_status_mask |= UART011_DR_FE | UART011_DR_PE;
	if (termios->c_iflag & IGNBRK) {
		port->ignore_status_mask |= UART011_DR_BE;
		/*
		 * If we're ignoring parity and break indicators,
		 * ignore overruns too (for real raw support).
		 */
		if (termios->c_iflag & IGNPAR)
			port->ignore_status_mask |= UART011_DR_OE;
	}

	/*
	 * Ignore all characters if CREAD is not set.
	 */
	if ((termios->c_cflag & CREAD) == 0)
		port->ignore_status_mask |= UART_DUMMY_DR_RX;

	if (UART_ENABLE_MS(port, termios->c_cflag))
		pl011_enable_ms(port);

	/* first, disable everything */
	old_cr = readw(port->membase + UART011_CR);
	writew(0, port->membase + UART011_CR);

	if (termios->c_cflag & CRTSCTS) {
		if (old_cr & UART011_CR_RTS)
			old_cr |= UART011_CR_RTSEN;

		old_cr |= UART011_CR_CTSEN;
		uap->autorts = true;
	} else {
		old_cr &= ~(UART011_CR_CTSEN | UART011_CR_RTSEN);
		uap->autorts = false;
	}

	if (uap->vendor->oversampling) {
		if (baud > port->uartclk / 16)
			old_cr |= ST_UART011_CR_OVSFACT;
		else
			old_cr &= ~ST_UART011_CR_OVSFACT;
	}

	/*
	 * Workaround for the ST Micro oversampling variants to
	 * increase the bitrate slightly, by lowering the divisor,
	 * to avoid delayed sampling of start bit at high speeds,
	 * else we see data corruption.
	 */
	if (uap->vendor->oversampling) {
		if ((baud >= 3000000) && (baud < 3250000) && (quot > 1))
			quot -= 1;
		else if ((baud > 3250000) && (quot > 2))
			quot -= 2;
	}
	/* Set baud rate */
	writew(quot & 0x3f, port->membase + UART011_FBRD);
	writew(quot >> 6, port->membase + UART011_IBRD);

	/*
	 * hold_percpu_var--------v----------v----------v----------v-----
	 * NOTE: lcrh_tx and lcrh_rx MUST BE WRITTEN AFTER
	 * UART011_FBRD & UART011_IBRD.
	 * hold_percpu_var--------^----------^----------^----------^-----
	 */
	pl011_write_lcr_h(uap, lcr_h);
	writew(old_cr, port->membase + UART011_CR);

	smp_unlock_irqrestore(&port->lock, flags);
}


static const char *pl011_type(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	return uap->port.type == PORT_AMBA ? uap->type : NULL;
}

/*
 * Release the memory region(s) being used by 'port'
 */
static void pl011_release_port(struct uart_port *port)
{
	release_mem_region(port->mapbase, SZ_4K);
}

/*
 * Request the memory region(s) being used by 'port'
 */
static int pl011_request_port(struct uart_port *port)
{
	return request_mem_region(port->mapbase, SZ_4K, "uart-pl011")
			!= NULL ? 0 : -EBUSY;
}

/*
 * Configure/autoconfigure the port.
 */
static void pl011_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE) {
		port->type = PORT_AMBA;
		pl011_request_port(port);
	}
}

/*
 * verify the new serial_struct (for TIOCSSERIAL).
 */
static int pl011_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	int ret = 0;
	if (ser->type != PORT_UNKNOWN && ser->type != PORT_AMBA)
		ret = -EINVAL;
	if (ser->irq < 0 || ser->irq >= NR_IRQS)
		ret = -EINVAL;
	if (ser->baud_base < 9600)
		ret = -EINVAL;
	return ret;
}

static struct uart_ops amba_pl011_pops = {
	.tx_empty	= pl011_tx_empty,
	.set_mctrl	= pl011_set_mctrl,
	.get_mctrl	= pl011_get_mctrl,
	.stop_tx	= pl011_stop_tx,
	.start_tx	= pl011_start_tx,
	.stop_rx	= pl011_stop_rx,
	.enable_ms	= pl011_enable_ms,
	.break_ctl	= pl011_break_ctl,
	.startup	= pl011_startup,
	.shutdown	= pl011_shutdown,
	.flush_buffer	= pl011_dma_flush_buffer,
	.set_termios	= pl011_set_termios,
	.type		= pl011_type,
	.release_port	= pl011_release_port,
	.request_port	= pl011_request_port,
	.config_port	= pl011_config_port,
	.verify_port	= pl011_verify_port,
};

static struct irq_configure pl011_of_desc[] =
	{
		[0] = {3, {0, 1, 4}, NULL},
	};
	
static int pl011_probe(struct amba_device *dev, const struct amba_id *id)
{
	struct uart_amba_port *uap;
	struct vendor_data *vendor = id->data;
	void __iomem *base;
	int i, ret;

	init_one_hwirq(&pl011_of_desc[0]);
	
	for (i = 0; i < ARRAY_SIZE(amba_ports); i++)
		if (amba_ports[i] == NULL)
			break;

	if (i == ARRAY_SIZE(amba_ports))
		return -EBUSY;

	uap = kzalloc(sizeof(struct uart_amba_port),
			   PAF_KERNEL);
	if (uap == NULL)
		return -ENOMEM;

	//i = pl011_probe_dt_alias(i, &dev->dev);

	base = ioremap(dev->res.start,
			    resource_size(&dev->res));
	if (!base)
		return -ENOMEM;

	//uap->clk = devm_clk_get(&dev->dev, NULL);
	//if (IS_ERR(uap->clk))
	//	return PTR_ERR(uap->clk);

	uap->vendor = vendor;
	uap->lcrh_rx = vendor->lcrh_rx;
	uap->lcrh_tx = vendor->lcrh_tx;
	uap->old_cr = 0;
	uap->fifosize = vendor->get_fifosize(dev);
	uap->port.dev = &dev->dev;
	uap->port.mapbase = dev->res.start;
	uap->port.membase = base;
	uap->port.iotype = UPIO_MEM;
	uap->port.irq = dev->irq[0];
	uap->port.fifosize = uap->fifosize;
	uap->port.ops = &amba_pl011_pops;
	uap->port.flags = UPF_BOOT_AUTOCONF;
	uap->port.line = i;
	INIT_WORK(&uap->tx_softirq_work, pl011_tx_softirq, uap);

	/* Ensure interrupts from this UART are masked and cleared */
	writew(0, uap->port.membase + UART011_IMSC);
	writew(0xffff, uap->port.membase + UART011_ICR);

	snprintf(uap->type, sizeof(uap->type), "PL011 rev%u", amba_rev(dev));

	amba_ports[i] = uap;

	amba_set_drvdata(dev, uap);

	if (!amba_reg.state) {
		ret = uart_register_driver(&amba_reg);
		if (ret < 0) {
			return ret;
		}
	}

	ret = uart_add_one_port(&amba_reg, &uap->port);
	if (ret) {
		amba_ports[i] = NULL;
		uart_unregister_driver(&amba_reg);
	}

	return ret;
}


static int pl011_remove(struct amba_device *dev)
{
	struct uart_amba_port *uap = amba_get_drvdata(dev);
	bool busy = false;
	int i;

	uart_remove_one_port(&amba_reg, &uap->port);

	for (i = 0; i < ARRAY_SIZE(amba_ports); i++)
		if (amba_ports[i] == uap)
			amba_ports[i] = NULL;
		else if (amba_ports[i])
			busy = true;

	pl011_dma_remove(uap);
	if (!busy)
		uart_unregister_driver(&amba_reg);
	return 0;
}

static struct amba_driver pl011_driver = {
	.drv = {
		.name	= "uart-pl011",
		//.pm	= NULL,
	},
	.id_table	= pl011_ids,
	.probe		= pl011_probe,
	.remove		= pl011_remove,
};

int __init pl011_init(void)
{
	printk(KERN_INFO "Serial: AMBA PL011 UART driver\n");

	return amba_driver_register(&pl011_driver);
}

static unsigned int get_fifosize_arm(struct amba_device *dev)
{
	return amba_rev(dev) < 3 ? 16 : 32;
}

static struct vendor_data vendor_arm = {
	.ifls			= UART011_IFLS_RX4_8|UART011_IFLS_TX4_8,
	.lcrh_tx		= UART011_LCRH,
	.lcrh_rx		= UART011_LCRH,
	.oversampling		= false,
	.dma_threshold		= false,
	.cts_event_workaround	= false,
	.get_fifosize		= get_fifosize_arm,
};

static unsigned int get_fifosize_st(struct amba_device *dev)
{
	return 64;
}


static void pl011_console_putchar(struct uart_port *port, int ch)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	while (readw(uap->port.membase + UART01x_FR) & UART01x_FR_TXFF)
		barrier();
	writew(ch, uap->port.membase + UART01x_DR);
}

static void
pl011_console_write(struct console *co, const char *s, unsigned int count)
{
	struct uart_amba_port *uap = amba_ports[co->index];
	unsigned int status, old_cr, new_cr;
	unsigned long flags;
	int locked = 1;

	//clk_enable(uap->clk);

	local_irq_save(flags);
	if (0/* uap->port.sysrq */)
		locked = 0;
	else
		smp_lock(&uap->port.lock);

	/*
	 *	First save the CR then disable the interrupts
	 */
	old_cr = readw(uap->port.membase + UART011_CR);
	new_cr = old_cr & ~UART011_CR_CTSEN;
	new_cr |= UART01x_CR_UARTEN | UART011_CR_TXE;
	writew(new_cr, uap->port.membase + UART011_CR);

	uart_console_write(&uap->port, s, count, pl011_console_putchar);

	/*
	 *	Finally, wait for transmitter to become empty
	 *	and restore the TCR
	 */
	do {
		status = readw(uap->port.membase + UART01x_FR);
	} while (status & UART01x_FR_BUSY);
	writew(old_cr, uap->port.membase + UART011_CR);

	if (locked)
		smp_unlock(&uap->port.lock);
	local_irq_restore(flags);

	//clk_disable(uap->clk);
}

struct tty_driver *uart_console_device(struct console *co, int *index)
{
	struct uart_driver *p = co->data;
	*index = co->index;
	return p->tty_driver;
}


static void __init
pl011_console_get_options(struct uart_amba_port *uap, int *baud,
			     int *parity, int *bits)
{
	if (readw(uap->port.membase + UART011_CR) & UART01x_CR_UARTEN) {
		unsigned int lcr_h, ibrd, fbrd;

		lcr_h = readw(uap->port.membase + uap->lcrh_tx);

		*parity = 'n';
		if (lcr_h & UART01x_LCRH_PEN) {
			if (lcr_h & UART01x_LCRH_EPS)
				*parity = 'e';
			else
				*parity = 'o';
		}

		if ((lcr_h & 0x60) == UART01x_LCRH_WLEN_7)
			*bits = 7;
		else
			*bits = 8;

		ibrd = readw(uap->port.membase + UART011_IBRD);
		fbrd = readw(uap->port.membase + UART011_FBRD);

		*baud = uap->port.uartclk * 4 / (64 * ibrd + fbrd);

		if (uap->vendor->oversampling) {
			if (readw(uap->port.membase + UART011_CR)
				  & ST_UART011_CR_OVSFACT)
				*baud *= 2;
		}
	}
}

static int __init pl011_console_setup(struct console *co, char *options)
{
	struct uart_amba_port *uap;
	int baud = 38400;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';
	//int ret;

	/*
	 * Check whether an invalid uart number has been specified, and
	 * if so, search for the first available port that does have
	 * console support.
	 */
	if (co->index >= UART_NR)
		co->index = 0;
	uap = amba_ports[co->index];
	if (!uap)
		return -ENODEV;

#if 0
	/* Allow pins to be muxed in and configured */
	pinctrl_pm_select_default_state(uap->port.dev);

	ret = clk_prepare(uap->clk);
	if (ret)
		return ret;
#endif

	if (dev_get_platdata(uap->port.dev)) {
		struct amba_pl011_data *plat;

		plat = dev_get_platdata(uap->port.dev);
		if (plat->init)
			plat->init();
	}

	uap->port.uartclk = 24000000;//clk_get_rate(uap->clk);

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);
	else
		pl011_console_get_options(uap, &baud, &parity, &bits);

	return uart_set_options(&uap->port, co, baud, parity, bits, flow);
}


#define SERIAL_AMBA_MAJOR	204
#define SERIAL_AMBA_MINOR	64

static struct console amba_console = {
	.name		= "ttyAMA",
	.write		= pl011_console_write,
	.device		= uart_console_device,
	.setup		= pl011_console_setup,
	.flags		= CONSOLE_PRINTK,
	.index		= -1,
	.data		= &amba_reg,
};

#define AMBA_CONSOLE	(&amba_console)

static struct uart_driver amba_reg = {
	//.owner			= THIS_MODULE,
	.driver_name		= "ttyAMA",
	.dev_name		= "ttyAMA",
	.sysobj_name		= "ttyAMA",
	.major			= SERIAL_AMBA_MAJOR,
	.minor			= SERIAL_AMBA_MINOR,
	.nr			= UART_NR,
	.cons			= AMBA_CONSOLE,
};

static struct vendor_data vendor_st = {
	.ifls			= UART011_IFLS_RX_HALF|UART011_IFLS_TX_HALF,
	.lcrh_tx		= ST_UART011_LCRH_TX,
	.lcrh_rx		= ST_UART011_LCRH_RX,
	.oversampling		= true,
	.dma_threshold		= true,
	.cts_event_workaround	= true,
	.get_fifosize		= get_fifosize_st,
};

static struct amba_id pl011_ids[] = {
	{
		.id	= 0x00041011,
		.mask	= 0x000fffff,
		.data	= &vendor_arm,
	},
	{
		.id	= 0x00380802,
		.mask	= 0x00ffffff,
		.data	= &vendor_st,
	},
	{ 0, 0 },
};
