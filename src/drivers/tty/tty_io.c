#include <dim-sum/beehive.h>
#include <dim-sum/capability.h>
#include <dim-sum/console.h>
#include <dim-sum/devno.h>
#include <dim-sum/errno.h>
#include <dim-sum/init.h>
#include <dim-sum/poll.h>
#include <dim-sum/tty.h>
#include <dim-sum/sched.h>
#include <dim-sum/uaccess.h>

struct semaphore tty_sem = 
			SEMAPHORE_INITIALIZER(tty_sem, 1);

struct double_list tty_drivers = LIST_HEAD_INITIALIZER(tty_drivers);

static struct smp_lock redirect_lock = 
			SMP_LOCK_UNLOCKED(redirect_lock);
static struct file *redirect;

static struct file_ops tty_fops;

/*
 *	This guards the refcounted line discipline lists. The lock
 *	must be taken with irqs off because there are hangup path
 *	callers who will do ldisc lookups and cannot sleep.
 */
static struct smp_lock tty_ldisc_lock = 
			SMP_LOCK_UNLOCKED(tty_ldisc_lock);
struct wait_queue tty_ldisc_wait = __WAIT_QUEUE_INITIALIZER(tty_ldisc_wait);
static struct tty_ldisc tty_ldiscs[NR_LDISCS];	/* line disc dispatch table	*/

static int tty_fasync(int fd, struct file * filp, int on);

/*	intr=^C		quit=^\		erase=del	kill=^U
	eof=^D		vtime=\0	vmin=\1		sxtc=\0
	start=^Q	stop=^S		susp=^Z		eol=\0
	reprint=^R	discard=^U	werase=^W	lnext=^V
	eol2=\0
*/
#define INIT_C_CC "\003\034\177\025\004\0\1\0\021\023\032\0\022\017\027\026\0"


/*
 * Routine which returns the baud rate of the tty
 *
 * Note that the baud_table needs to be kept in sync with the
 * include/asm/termbits.h file.
 */
static int baud_table[] = {
	0, 50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400, 4800,
	9600, 19200, 38400, 57600, 115200, 230400, 460800,
#ifdef __sparc__
	76800, 153600, 307200, 614400, 921600
#else
	500000, 576000, 921600, 1000000, 1152000, 1500000, 2000000,
	2500000, 3000000, 3500000, 4000000
#endif
};

static int n_baud_table = ARRAY_SIZE(baud_table);

/**
 * 默认的tty端口属性。
 */
struct termios tty_std_termios = {	/* for the benefit of tty drivers  */
	.c_iflag = ICRNL | IXON,
	.c_oflag = OPOST | ONLCR,
	.c_cflag = B38400 | CS8 | CREAD | HUPCL,
	.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK |
		   ECHOCTL | ECHOKE | IEXTEN,
	.c_cc = INIT_C_CC
};

static struct tty_struct *alloc_tty_struct(void)
{
	struct tty_struct *tty;

	tty = kmalloc(sizeof(struct tty_struct), PAF_KERNEL);
	if (tty)
		memset(tty, 0, sizeof(struct tty_struct));
	return tty;
}


static inline void free_tty_struct(struct tty_struct *tty)
{
	kfree(tty->write_buf);
	kfree(tty);
}


/**
 * 分配一个tty设备驱动程序结构。
 *		lines:		该驱动程序支持的设备数量。
 */
struct tty_driver *alloc_tty_driver(int lines)
{
	struct tty_driver *driver;

	driver = kmalloc(sizeof(struct tty_driver), PAF_KERNEL);
	if (driver) {
		memset(driver, 0, sizeof(struct tty_driver));
		driver->magic = TTY_DRIVER_MAGIC;
		driver->num = lines;
		/* later we'll move allocation of tables here */
	}
	return driver;
}

void put_tty_driver(struct tty_driver *driver)
{
	kfree(driver);
}

void tty_wait_until_sent(struct tty_struct * tty, long timeout)
{
	struct wait_task_desc wait = __WAIT_TASK_INITIALIZER(wait, current);

#ifdef TTY_DEBUG_WAIT_UNTIL_SENT
	char buf[64];
	
	printk(KERN_DEBUG "%s wait until sent...\n", tty_name(tty, buf));
#endif
	if (!tty->driver->chars_in_buffer)
		return;
	add_to_wait_queue(&tty->write_wait, &wait);
	if (!timeout)
		timeout = MAX_SCHEDULE_TIMEOUT;
	do {
#ifdef TTY_DEBUG_WAIT_UNTIL_SENT
		printk(KERN_DEBUG "waiting %s...(%d)\n", tty_name(tty, buf),
		       tty->driver->chars_in_buffer(tty));
#endif
		set_current_state(TASK_INTERRUPTIBLE);
		if (signal_pending(current))
			goto stop_waiting;
		if (!tty->driver->chars_in_buffer(tty))
			break;
		timeout = schedule_timeout(timeout);
	} while (timeout);
	if (tty->driver->wait_until_sent)
		tty->driver->wait_until_sent(tty, timeout);
stop_waiting:
	set_current_state(TASK_RUNNING);
	del_from_wait_queue(&tty->write_wait, &wait);
}


static ssize_t hung_up_tty_read(struct file * file, char __user * buf,
				size_t count, loff_t *ppos)
{
	return 0;
}

static ssize_t hung_up_tty_write(struct file * file, const char __user * buf,
				 size_t count, loff_t *ppos)
{
	return -EIO;
}

/* No kernel lock held - none needed ;) */
static unsigned int hung_up_tty_poll(struct file * filp, poll_table * wait)
{
	return POLLIN | POLLOUT | POLLERR | POLLHUP | POLLRDNORM | POLLWRNORM;
}

static int hung_up_tty_ioctl(struct file_node * file_node, struct file * file,
			     unsigned int cmd, unsigned long arg)
{
	return cmd == TIOCSPGRP ? -ENOTTY : -EIO;
}

static void release_dev(struct file * filp)
{
/* TO-DO */
}

static int tty_release(struct file_node * file_node, struct file * filp)
{
	release_dev(filp);
	return 0;
}

static struct file_ops hung_up_tty_fops = {
	.llseek		= no_llseek,
	.read		= hung_up_tty_read,
	.write		= hung_up_tty_write,
	.poll		= hung_up_tty_poll,
	.ioctl		= hung_up_tty_ioctl,
	.release	= tty_release,
};

int tty_hung_up_p(struct file * filp)
{
	return (filp->file_ops == &hung_up_tty_fops);
}

/*
 * If we try to write to, or set the state of, a terminal and we're
 * not in the foreground, send a SIGTTOU.  If the signal is blocked or
 * ignored, go ahead and perform the operation.  (POSIX 7.2)
 */
int tty_check_change(struct tty_struct * tty)
{
/* TO-DO */
#if 0
	if (current->signal->tty != tty)
		return 0;
	if (tty->pgrp <= 0) {
		printk(KERN_WARNING "tty_check_change: tty->pgrp <= 0!\n");
		return 0;
	}
	if (process_group(current) == tty->pgrp)
		return 0;
	if (is_ignored(SIGTTOU))
		return 0;
	if (is_orphaned_pg(process_group(current)))
		return -EIO;
	(void) post_signal_to_procgroup(process_group(current), SIGTTOU, 1);
	return -ERESTARTSYS;
#else
	return 0;
#endif
}

char *tty_name(struct tty_struct *tty, char *buf)
{
	if (!tty) /* Hmm.  NULL pointer.  That's fun. */
		strcpy(buf, "NULL tty");
	else
		strcpy(buf, tty->name);
	return buf;
}

/**
 *	tty_ldisc_try		-	internal helper
 *	@tty: the tty
 *
 *	Make a single attempt to grab and bump the refcount on
 *	the tty ldisc. Return 0 on failure or 1 on success. This is
 *	used to implement both the waiting and non waiting versions
 *	of tty_ldisc_ref
 */

static int tty_ldisc_try(struct tty_struct *tty)
{
	unsigned long flags;
	struct tty_ldisc *ld;
	int ret = 0;
	
	smp_lock_irqsave(&tty_ldisc_lock, flags);
	ld = &tty->ldisc;
	if(test_bit(TTY_LDISC, &tty->flags))
	{
		ld->refcount++;
		ret = 1;
	}
	smp_unlock_irqrestore(&tty_ldisc_lock, flags);
	return ret;
}

/**
 *	tty_ldisc_ref		-	get the tty ldisc
 *	@tty: tty device
 *
 *	Dereference the line discipline for the terminal and take a 
 *	reference to it. If the line discipline is in flux then 
 *	return NULL. Can be called from IRQ and timer functions.
 */
 
struct tty_ldisc *tty_ldisc_ref(struct tty_struct *tty)
{
	if(tty_ldisc_try(tty))
		return &tty->ldisc;
	return NULL;
}


/**
 *	tty_ldisc_deref		-	free a tty ldisc reference
 *	@ld: reference to free up
 *
 *	Undoes the effect of tty_ldisc_ref or tty_ldisc_ref_wait. May
 *	be called in IRQ context.
 */
 
void tty_ldisc_deref(struct tty_ldisc *ld)
{
	unsigned long flags;

	if(ld == NULL)
		BUG();
		
	smp_lock_irqsave(&tty_ldisc_lock, flags);
	if(ld->refcount == 0)
		printk(KERN_ERR "tty_ldisc_deref: no references.\n");
	else
		ld->refcount--;
	if(ld->refcount == 0)
		wake_up(&tty_ldisc_wait);
	smp_unlock_irqrestore(&tty_ldisc_lock, flags);
}

/**
 *	tty_wakeup	-	request more data
 *	@tty: terminal
 *
 *	Internal and external helper for wakeups of tty. This function
 *	informs the line discipline if present that the driver is ready
 *	to receive more output data.
 */
 
void tty_wakeup(struct tty_struct *tty)
{
	struct tty_ldisc *ld;
	
	if (test_bit(TTY_DO_WRITE_WAKEUP, &tty->flags)) {
		ld = tty_ldisc_ref(tty);
		if(ld) {
			if(ld->write_wakeup)
				ld->write_wakeup(tty);
			tty_ldisc_deref(ld);
		}
	}
	wake_up_interruptible(&tty->write_wait);
}

void start_tty(struct tty_struct *tty)
{
	if (!tty->stopped || tty->flow_stopped)
		return;
	tty->stopped = 0;
	if (tty->link && tty->link->packet) {
		tty->ctrl_status &= ~TIOCPKT_STOP;
		tty->ctrl_status |= TIOCPKT_START;
		wake_up_interruptible(&tty->link->read_wait);
	}
	if (tty->driver->start)
		(tty->driver->start)(tty);

	/* If we have a running line discipline it may need kicking */
	tty_wakeup(tty);
	wake_up_interruptible(&tty->write_wait);
}


void stop_tty(struct tty_struct *tty)
{
	if (tty->stopped)
		return;
	tty->stopped = 1;
	if (tty->link && tty->link->packet) {
		tty->ctrl_status &= ~TIOCPKT_START;
		tty->ctrl_status |= TIOCPKT_STOP;
		wake_up_interruptible(&tty->link->read_wait);
	}
	if (tty->driver->stop)
		(tty->driver->stop)(tty);
}



/*
 * Split writes up in sane blocksizes to avoid
 * denial-of-service type attacks
 */
static inline ssize_t do_tty_write(
	ssize_t (*write)(struct tty_struct *, struct file *, const unsigned char *, size_t),
	struct tty_struct *tty,
	struct file *file,
	const char __user *buf,
	size_t count)
{
	ssize_t ret = 0, written = 0;
	unsigned int chunk;
	
	if (down_interruptible(&tty->atomic_write)) {
		return -ERESTARTSYS;
	}

	/*
	 * We chunk up writes into a temporary buffer. This
	 * simplifies low-level drivers immensely, since they
	 * don't have locking issues and user mode accesses.
	 *
	 * But if TTY_NO_WRITE_SPLIT is set, we should use a
	 * big chunk-size..
	 *
	 * The default chunk-size is 2kB, because the NTTY
	 * layer has problems with bigger chunks. It will
	 * claim to be able to handle more characters than
	 * it actually does.
	 */
	chunk = 2048;
	if (test_bit(TTY_NO_WRITE_SPLIT, &tty->flags))
		chunk = 65536;
	if (count < chunk)
		chunk = count;

	/* write_buf/write_cnt is protected by the atomic_write semaphore */
	if (tty->write_cnt < chunk) {
		unsigned char *buf;

		if (chunk < 1024)
			chunk = 1024;

		buf = kmalloc(chunk, PAF_KERNEL);
		if (!buf) {
			up(&tty->atomic_write);
			return -ENOMEM;
		}
		kfree(tty->write_buf);
		tty->write_cnt = chunk;
		tty->write_buf = buf;
	}

	/* Do the write .. */
	for (;;) {
		size_t size = count;
		if (size > chunk)
			size = chunk;
		ret = -EFAULT;
		if (copy_from_user(tty->write_buf, buf, size))
			break;
		ret = write(tty, file, tty->write_buf, size);
		if (ret <= 0)
			break;
		written += ret;
		buf += ret;
		count -= ret;
		if (!count)
			break;
		ret = -ERESTARTSYS;
		if (signal_pending(current))
			break;
		//cond_resched();
	}
	if (written) {
		//struct file_node *file_node = file->fnode_cache->file_node;
		//file_node->data_modify_time = current_fs_time(file_node->super);
		ret = written;
	}
	up(&tty->atomic_write);
	return ret;
}

/**
 *	tty_ldisc_ref_wait	-	wait for the tty ldisc
 *	@tty: tty device
 *
 *	Dereference the line discipline for the terminal and take a 
 *	reference to it. If the line discipline is in flux then 
 *	wait patiently until it changes.
 *
 *	Note: Must not be called from an IRQ/timer context. The caller
 *	must also be careful not to hold other locks that will deadlock
 *	against a discipline change, such as an existing ldisc reference
 *	(which we check for)
 */
 
struct tty_ldisc *tty_ldisc_ref_wait(struct tty_struct *tty)
{
	/* cond_wait is a macro */
	cond_wait(tty_ldisc_wait, tty_ldisc_try(tty));
	if(tty->ldisc.refcount == 0)
		printk(KERN_ERR "tty_ldisc_ref_wait\n");
	return &tty->ldisc;
}


static ssize_t tty_write(struct file * file, const char __user * buf, size_t count,
			 loff_t *ppos)
{
	struct tty_struct * tty;
	ssize_t ret;
	struct tty_ldisc *ld;
	
	tty = (struct tty_struct *)file->private_data;

	if (!tty || !tty->driver->write || (test_bit(TTY_IO_ERROR, &tty->flags)))
		return -EIO;

	ld = tty_ldisc_ref_wait(tty);		
	if (!ld->write)
		ret = -EIO;
	else
		ret = do_tty_write(ld->write, tty, file, buf, count);
	tty_ldisc_deref(ld);
	return ret;
}


ssize_t redirected_tty_write(struct file * file, const char __user * buf, size_t count,
			 loff_t *ppos)
{
	struct file *p = NULL;

	smp_lock(&redirect_lock);
	if (redirect) {
		hold_file(redirect);
		p = redirect;
	}
	smp_unlock(&redirect_lock);

	if (p) {
		ssize_t res;
		res = vfs_write(p, buf, count, &p->pos);
		loosen_file(p);
		return res;
	}

	return tty_write(file, buf, count, ppos);
}

int tty_register_ldisc(int disc, struct tty_ldisc *new_ldisc)
{
	unsigned long flags;
	int ret = 0;
	
	if (disc < N_TTY || disc >= NR_LDISCS)
		return -EINVAL;
	
	smp_lock_irqsave(&tty_ldisc_lock, flags);
	if (new_ldisc) {
		tty_ldiscs[disc] = *new_ldisc;
		tty_ldiscs[disc].num = disc;
		tty_ldiscs[disc].flags |= LDISC_FLAG_DEFINED;
		tty_ldiscs[disc].refcount = 0;
	} else {
		if(tty_ldiscs[disc].refcount)
			ret = -EBUSY;
		else
			tty_ldiscs[disc].flags &= ~LDISC_FLAG_DEFINED;
	}
	smp_unlock_irqrestore(&tty_ldisc_lock, flags);
	
	return ret;
}

static struct char_device console_cdev;


static ssize_t tty_read(struct file * file, char __user * buf, size_t count, 
			loff_t *ppos)
{
	int i;
	struct tty_struct * tty;
	struct file_node *file_node;
	struct tty_ldisc *ld;

	tty = (struct tty_struct *)file->private_data;
	file_node = file->fnode_cache->file_node;
	if (!tty || (test_bit(TTY_IO_ERROR, &tty->flags)))
		return -EIO;

	/* We want to wait for the line discipline to sort out in this
	   situation */
	ld = tty_ldisc_ref_wait(tty);
	//lock_kernel();
	if (ld->read)
		i = (ld->read)(tty,file,buf,count);
	else
		i = -EIO;
	tty_ldisc_deref(ld);
	//unlock_kernel();
	if (i > 0)
		file_node->access_time = CURRENT_TIME;//(file_node->super);
	return i;
}

/* No kernel lock held - fine */
static unsigned int tty_poll(struct file * filp, poll_table * wait)
{
	struct tty_struct * tty;
	struct tty_ldisc *ld;
	int ret = 0;

	tty = (struct tty_struct *)filp->private_data;
		
	ld = tty_ldisc_ref_wait(tty);
	if (ld->poll)
		ret = (ld->poll)(tty, filp, wait);
	tty_ldisc_deref(ld);
	return ret;
}

/*
 * Split this up, as gcc can choke on it otherwise..
 */
int tty_ioctl(struct file_node * file_node, struct file * file,
	      unsigned int cmd, unsigned long arg)
{
	return -EINVAL;
}

/*
 * This routine returns a tty driver structure, given a device number
 */
static struct tty_driver *get_tty_driver(devno_t device, int *index)
{
	struct tty_driver *p;

	list_for_each_entry(p, &tty_drivers, tty_drivers) {
		devno_t base = MKDEV(p->major, p->minor_start);
		if (device < base || device >= base + p->num)
			continue;
		*index = device - base;
		return p;
	}
	return NULL;
}


static void tty_ldisc_assign(struct tty_struct *tty, struct tty_ldisc *ld)
{
	tty->ldisc = *ld;
	tty->ldisc.refcount = 0;
}


/*
 * This routine is called out of the software interrupt to flush data
 * from the flip buffer to the line discipline. 
 */
 
static void flush_to_ldisc(void *private_)
{
	struct tty_struct *tty = (struct tty_struct *) private_;
	unsigned char	*cp;
	char		*fp;
	int		count;
	unsigned long 	flags;
	struct tty_ldisc *disc;

	disc = tty_ldisc_ref(tty);
	if (disc == NULL)	/*  !TTY_LDISC */
		return;

	if (test_bit(TTY_DONT_FLIP, &tty->flags)) {
		/*
		 * Do it after the next timer tick:
		 */
		schedule_delayed_work(&tty->flip.work, 1);
		goto out;
	}
	smp_lock_irqsave(&tty->smp_read_lock, flags);
	if (tty->flip.buf_num) {
		cp = tty->flip.char_buf + TTY_FLIPBUF_SIZE;
		fp = tty->flip.flag_buf + TTY_FLIPBUF_SIZE;
		tty->flip.buf_num = 0;
		tty->flip.char_buf_ptr = tty->flip.char_buf;
		tty->flip.flag_buf_ptr = tty->flip.flag_buf;
	} else {
		cp = tty->flip.char_buf;
		fp = tty->flip.flag_buf;
		tty->flip.buf_num = 1;
		tty->flip.char_buf_ptr = tty->flip.char_buf + TTY_FLIPBUF_SIZE;
		tty->flip.flag_buf_ptr = tty->flip.flag_buf + TTY_FLIPBUF_SIZE;
	}
	count = tty->flip.count;
	tty->flip.count = 0;
	smp_unlock_irqrestore(&tty->smp_read_lock, flags);

	disc->receive_buf(tty, cp, fp, count);
out:
	tty_ldisc_deref(disc);
}


static int check_tty_count(struct tty_struct *tty, const char *routine)
{
	return 0;
}


/**
 *	tty_ldisc_enable	-	allow ldisc use
 *	@tty: terminal to activate ldisc on
 *
 *	Set the TTY_LDISC flag when the line discipline can be called
 *	again. Do neccessary wakeups for existing sleepers.
 *
 *	Note: nobody should set this bit except via this function. Clearing
 *	directly is allowed.
 */

static void tty_ldisc_enable(struct tty_struct *tty)
{
	atomic_set_bit(TTY_LDISC, &tty->flags);
	wake_up(&tty_ldisc_wait);
}


/*
 * This can be called by the "eventd" kernel thread.  That is process synchronous,
 * but doesn't hold any locks, so we need to make sure we have the appropriate
 * locks for what we're doing..
 */
static void do_tty_hangup(void *data)
{
	struct tty_struct *tty = (struct tty_struct *) data;
	struct file * cons_filp = NULL;
	struct file *filp, *f = NULL;
	//struct task_desc *p;
	struct tty_ldisc *ld;
	int    closecount = 0, n;

	if (!tty)
		return;

	/* inuse_filps is protected by the single kernel lock */
	//lock_kernel();

	smp_lock(&redirect_lock);
	if (redirect && redirect->private_data == tty) {
		f = redirect;
		redirect = NULL;
	}
	smp_unlock(&redirect_lock);
	
	check_tty_count(tty, "do_tty_hangup");
	smp_lock(&files_lock);
	/* This breaks for file handles being sent over AF_UNIX sockets ? */
	list_for_each_entry(filp, &tty->tty_files, super) {
		if (filp->file_ops->write == redirected_tty_write)
			cons_filp = filp;
		if (filp->file_ops->write != tty_write)
			continue;
		closecount++;
		tty_fasync(-1, filp, 0);	/* can't block */
		filp->file_ops = &hung_up_tty_fops;
	}
	smp_unlock(&files_lock);
	
	/* FIXME! What are the locking issues here? This may me overdoing things..
	 * this question is especially important now that we've removed the irqlock. */

	ld = tty_ldisc_ref(tty);
	if(ld != NULL)	/* We may have no line discipline at this point */
	{
		if (ld->flush_buffer)
			ld->flush_buffer(tty);
		if (tty->driver->flush_buffer)
			tty->driver->flush_buffer(tty);
		if ((test_bit(TTY_DO_WRITE_WAKEUP, &tty->flags)) &&
		    ld->write_wakeup)
			ld->write_wakeup(tty);
		if (ld->hangup)
			ld->hangup(tty);
	}

	/* FIXME: Once we trust the LDISC code better we can wait here for
	   ldisc completion and fix the driver call race */
	   
	wake_up_interruptible(&tty->write_wait);
	wake_up_interruptible(&tty->read_wait);

	/*
	 * Shutdown the current line discipline, and reset it to
	 * N_TTY.
	 */
	if (tty->driver->flags & TTY_DRIVER_RESET_TERMIOS)
	{
		down(&tty->termios_sem);
		*tty->termios = tty->driver->init_termios;
		up(&tty->termios_sem);
	}
	
	/* Defer ldisc switch */
	/* tty_deferred_ldisc_switch(N_TTY);
	
	  This should get done automatically when the port closes and
	  tty_release is called */
#if 0	
	smp_read_lock(&tasklist_lock);
	if (tty->session > 0) {
		do_each_task_pid(tty->session, PIDTYPE_SID, p) {
			if (p->signal->tty == tty)
				p->signal->tty = NULL;
			if (!p->signal->leader)
				continue;
			send_group_sig_info(SIGHUP, SEND_SIG_PRIV, p);
			send_group_sig_info(SIGCONT, SEND_SIG_PRIV, p);
			if (tty->pgrp > 0)
				p->signal->tty_old_pgrp = tty->pgrp;
		} while_each_task_pid(tty->session, PIDTYPE_SID, p);
	}
	smp_read_unlock(&tasklist_lock);
#endif

	tty->flags = 0;
	tty->session = 0;
	tty->pgrp = -1;
	tty->ctrl_status = 0;
	/*
	 *	If one of the devices matches a console pointer, we
	 *	cannot just call hangup() because that will cause
	 *	tty->count and state->count to go out of sync.
	 *	So we just call close() the right number of times.
	 */
	if (cons_filp) {
		if (tty->driver->close)
			for (n = 0; n < closecount; n++)
				tty->driver->close(tty, cons_filp);
	} else if (tty->driver->hangup)
		(tty->driver->hangup)(tty);
		
	/* We don't want to have driver/ldisc interactions beyond
	   the ones we did here. The driver layer expects no
	   calls after ->hangup() from the ldisc side. However we
	   can't yet guarantee all that */

	atomic_set_bit(TTY_HUPPED, &tty->flags);
	if (ld) {
		tty_ldisc_enable(tty);
		tty_ldisc_deref(ld);
	}
	//unlock_kernel();
	if (f)
		loosen_file(f);
}


struct tty_ldisc *tty_ldisc_get(int disc)
{
	unsigned long flags;
	struct tty_ldisc *ld;

	if (disc < N_TTY || disc >= NR_LDISCS)
		return NULL;
	
	smp_lock_irqsave(&tty_ldisc_lock, flags);

	ld = &tty_ldiscs[disc];
	/* Check the entry is defined */
	if(ld->flags & LDISC_FLAG_DEFINED)
	{
#if 0
		/* If the module is being unloaded we can't use it */
		if (!try_module_get(ld->owner))
		       	ld = NULL;
		else /* lock it */
			ld->refcount++;
#else
		ld->refcount++;
#endif
	}
	else
		ld = NULL;
	smp_unlock_irqrestore(&tty_ldisc_lock, flags);
	return ld;
}


/*
 * This subroutine initializes a tty structure.
 */
static void initialize_tty_struct(struct tty_struct *tty)
{
	memset(tty, 0, sizeof(struct tty_struct));
	tty->magic = TTY_MAGIC;
	tty_ldisc_assign(tty, tty_ldisc_get(N_TTY));
	tty->pgrp = -1;
	tty->flip.char_buf_ptr = tty->flip.char_buf;
	tty->flip.flag_buf_ptr = tty->flip.flag_buf;
	INIT_WORK(&tty->flip.work, flush_to_ldisc, tty);
	sema_init(&tty->flip.pty_sem, 1);
	sema_init(&tty->termios_sem, 1);
	init_waitqueue(&tty->write_wait);
	init_waitqueue(&tty->read_wait);
	INIT_WORK(&tty->hangup_work, do_tty_hangup, tty);
	sema_init(&tty->accurate_read, 1);
	sema_init(&tty->atomic_write, 1);
	smp_lock_init(&tty->smp_read_lock);
	list_init(&tty->tty_files);
	INIT_WORK(&tty->SAK_work, NULL, NULL);
}


static inline void tty_line_name(struct tty_driver *driver, int index, char *p)
{
	sprintf(p, "%s%d", driver->name, index + driver->name_base);
}


/*
 * Releases memory associated with a tty structure, and clears out the
 * driver table slots.
 */
static void release_mem(struct tty_struct *tty, int idx)
{
	struct tty_struct *o_tty;
	struct termios *tp;
	int devpts = 0;//tty->driver->flags & TTY_DRIVER_DEVPTS_MEM;

	if ((o_tty = tty->link) != NULL) {
		if (!devpts)
			o_tty->driver->ttys[idx] = NULL;
		if (o_tty->driver->flags & TTY_DRIVER_RESET_TERMIOS) {
			tp = o_tty->termios;
			if (!devpts)
				o_tty->driver->termios[idx] = NULL;
			kfree(tp);

			tp = o_tty->termios_locked;
			if (!devpts)
				o_tty->driver->termios_locked[idx] = NULL;
			kfree(tp);
		}
		o_tty->magic = 0;
		o_tty->driver->refcount--;
		smp_lock(&files_lock);
		list_del_init(&o_tty->tty_files);
		smp_unlock(&files_lock);
		free_tty_struct(o_tty);
	}

	if (!devpts)
		tty->driver->ttys[idx] = NULL;
	if (tty->driver->flags & TTY_DRIVER_RESET_TERMIOS) {
		tp = tty->termios;
		if (!devpts)
			tty->driver->termios[idx] = NULL;
		kfree(tp);

		tp = tty->termios_locked;
		if (!devpts)
			tty->driver->termios_locked[idx] = NULL;
		kfree(tp);
	}

	tty->magic = 0;
	tty->driver->refcount--;
	smp_lock(&files_lock);
	list_del_init(&tty->tty_files);
	smp_unlock(&files_lock);
	//module_put(tty->driver->owner);
	free_tty_struct(tty);
}


/*
 * WSH 06/09/97: Rewritten to remove races and properly clean up after a
 * failed open.  The new code protects the open with a semaphore, so it's
 * really quite straightforward.  The semaphore locking can probably be
 * relaxed for the (most common) case of reopening a tty.
 */
static int init_dev(struct tty_driver *driver, int idx,
	struct tty_struct **ret_tty)
{
	struct tty_struct *tty, *o_tty;
	struct termios *tp, **tp_loc, *o_tp/*, **o_tp_loc*/;
	struct termios *ltp, **ltp_loc, *o_ltp/*, **o_ltp_loc*/;
	int retval=0;

#if 0
	/* check whether we're reopening an existing tty */
	if (driver->flags & TTY_DRIVER_DEVPTS_MEM) {
		tty = devpts_get_tty(idx);
		if (tty && driver->subtype == PTY_TYPE_MASTER)
			tty = tty->link;
	} else {
		tty = driver->ttys[idx];
	}
#else
	tty = driver->ttys[idx];
#endif
	if (tty) goto fast_track;

	/*
	 * First time open is complex, especially for PTY devices.
	 * This code guarantees that either everything succeeds and the
	 * TTY is ready for operation, or else the table slots are vacated
	 * and the allocated memory released.  (Except that the termios 
	 * and locked termios may be retained.)
	 */
#if 0
	if (!try_module_get(driver->owner)) {
		retval = -ENODEV;
		goto end_init;
	}
#endif
	o_tty = NULL;
	tp = o_tp = NULL;
	ltp = o_ltp = NULL;

	tty = alloc_tty_struct();
	if(!tty)
		goto fail_no_mem;
	initialize_tty_struct(tty);
	tty->driver = driver;
	tty->index = idx;
	tty_line_name(driver, idx, tty->name);

#if 0
	if (driver->flags & TTY_DRIVER_DEVPTS_MEM) {
		tp_loc = &tty->termios;
		ltp_loc = &tty->termios_locked;
	} else {
		tp_loc = &driver->termios[idx];
		ltp_loc = &driver->termios_locked[idx];
	}
#else
	tp_loc = &driver->termios[idx];
	ltp_loc = &driver->termios_locked[idx];	
#endif
	if (!*tp_loc) {
		tp = (struct termios *) kmalloc(sizeof(struct termios),
						PAF_KERNEL);
		if (!tp)
			goto free_mem_out;
		*tp = driver->init_termios;
	}

	if (!*ltp_loc) {
		ltp = (struct termios *) kmalloc(sizeof(struct termios),
						 PAF_KERNEL);
		if (!ltp)
			goto free_mem_out;
		memset(ltp, 0, sizeof(struct termios));
	}

#if 0
	if (driver->type == TTY_DRIVER_TYPE_PTY) {
		o_tty = alloc_tty_struct();
		if (!o_tty)
			goto free_mem_out;
		initialize_tty_struct(o_tty);
		o_tty->driver = driver->other;
		o_tty->index = idx;
		tty_line_name(driver->other, idx, o_tty->name);

		if (driver->flags & TTY_DRIVER_DEVPTS_MEM) {
			o_tp_loc = &o_tty->termios;
			o_ltp_loc = &o_tty->termios_locked;
		} else {
			o_tp_loc = &driver->other->termios[idx];
			o_ltp_loc = &driver->other->termios_locked[idx];
		}

		if (!*o_tp_loc) {
			o_tp = (struct termios *)
				kmalloc(sizeof(struct termios), PAF_KERNEL);
			if (!o_tp)
				goto free_mem_out;
			*o_tp = driver->other->init_termios;
		}

		if (!*o_ltp_loc) {
			o_ltp = (struct termios *)
				kmalloc(sizeof(struct termios), PAF_KERNEL);
			if (!o_ltp)
				goto free_mem_out;
			memset(o_ltp, 0, sizeof(struct termios));
		}

		/*
		 * Everything allocated ... set up the o_tty structure.
		 */
		if (!(driver->other->flags & TTY_DRIVER_DEVPTS_MEM)) {
			driver->other->ttys[idx] = o_tty;
		}
		if (!*o_tp_loc)
			*o_tp_loc = o_tp;
		if (!*o_ltp_loc)
			*o_ltp_loc = o_ltp;
		o_tty->termios = *o_tp_loc;
		o_tty->termios_locked = *o_ltp_loc;
		driver->other->refcount++;
		if (driver->subtype == PTY_TYPE_MASTER)
			o_tty->count++;

		/* Establish the links in both directions */
		tty->link   = o_tty;
		o_tty->link = tty;
	}

	/* 
	 * All structures have been allocated, so now we install them.
	 * Failures after this point use release_mem to clean up, so 
	 * there's no need to null out the local pointers.
	 */
	if (!(driver->flags & TTY_DRIVER_DEVPTS_MEM)) {
		driver->ttys[idx] = tty;
	}
#endif

	if (!*tp_loc)
		*tp_loc = tp;
	if (!*ltp_loc)
		*ltp_loc = ltp;
	tty->termios = *tp_loc;
	tty->termios_locked = *ltp_loc;
	driver->refcount++;
	tty->count++;

	/* 
	 * Structures all installed ... call the ldisc open routines.
	 * If we fail here just call release_mem to clean up.  No need
	 * to decrement the use counts, as release_mem doesn't care.
	 */

	if (tty->ldisc.open) {
		retval = (tty->ldisc.open)(tty);
		if (retval)
			goto release_mem_out;
	}
	if (o_tty && o_tty->ldisc.open) {
		retval = (o_tty->ldisc.open)(o_tty);
		if (retval) {
			if (tty->ldisc.close)
				(tty->ldisc.close)(tty);
			goto release_mem_out;
		}
		tty_ldisc_enable(o_tty);
	}
	tty_ldisc_enable(tty);
	goto success;

	/*
	 * This fast open can be used if the tty is already open.
	 * No memory is allocated, and the only failures are from
	 * attempting to open a closing tty or attempting multiple
	 * opens on a pty master.
	 */
fast_track:
	if (test_bit(TTY_CLOSING, &tty->flags)) {
		retval = -EIO;
		goto end_init;
	}
	if (driver->type == TTY_DRIVER_TYPE_PTY &&
	    driver->subtype == PTY_TYPE_MASTER) {
		/*
		 * special case for PTY masters: only one open permitted, 
		 * and the slave side open count is incremented as well.
		 */
		if (tty->count) {
			retval = -EIO;
			goto end_init;
		}
		tty->link->count++;
	}
	tty->count++;
	tty->driver = driver; /* N.B. why do this every time?? */

	/* FIXME */
	if(!test_bit(TTY_LDISC, &tty->flags))
		printk(KERN_ERR "init_dev but no ldisc\n");
success:
	*ret_tty = tty;
	
	/* All paths come through here to release the semaphore */
end_init:
	return retval;

	/* Release locally allocated memory ... nothing placed in slots */
free_mem_out:
	if (o_tp)
		kfree(o_tp);
	if (o_tty)
		free_tty_struct(o_tty);
	if (ltp)
		kfree(ltp);
	if (tp)
		kfree(tp);
	free_tty_struct(tty);

fail_no_mem:
	//module_put(driver->owner);
	retval = -ENOMEM;
	goto end_init;

	/* call the tty release_mem routine to clean out this slot */
release_mem_out:
	printk(KERN_INFO "init_dev: ldisc open failed, "
			 "clearing slot %d\n", idx);
	release_mem(tty, idx);
	goto end_init;
}

/*
 * tty_open and tty_release keep up the tty count that contains the
 * number of opens done on a tty. We cannot use the file_node-count, as
 * different inodes might point to the same tty.
 *
 * Open-counting is needed for pty masters, as well as for keeping
 * track of serial lines: DTR is dropped when the last close happens.
 * (This is not done solely through tty->count, now.  - Ted 1/27/92)
 *
 * The termios state of a pty is reset on first open so that
 * settings don't persist across reuse.
 */
static int tty_open(struct file_node * file_node, struct file * filp)
{
	struct tty_struct *tty;
	int /*noctty, */retval;
	struct tty_driver *driver;
	int index;
	devno_t device = file_node->devno;
	unsigned short saved_flags = filp->flags;

	nonseekable_open(file_node, filp);
	
retry_open:
	//noctty = filp->flags & O_NOCTTY;
	index  = -1;
	retval = 0;
	
	down(&tty_sem);

#if 0
	if (device == MKDEV(TTYAUX_MAJOR,0)) {
		if (!current->signal->tty) {
			up(&tty_sem);
			return -ENXIO;
		}
		driver = current->signal->tty->driver;
		index = current->signal->tty->index;
		filp->flags |= O_NONBLOCK; /* Don't let /dev/tty block */
		/* noctty = 1; */
		goto got_driver;
	}
#endif
	if (device == MKDEV(TTYAUX_MAJOR,1)) {
		driver = get_console_device(&index);
		if (driver) {
			/* Don't let /dev/console block */
			filp->flags |= O_NONBLOCK;
			//noctty = 1;
			goto got_driver;
		}
		up(&tty_sem);
		return -ENODEV;
	}

	driver = get_tty_driver(device, &index);
	if (!driver) {
		up(&tty_sem);
		return -ENODEV;
	}
got_driver:
	retval = init_dev(driver, index, &tty);
	up(&tty_sem);
	if (retval)
		return retval;

	filp->private_data = tty;
	file_move_to_list(filp, &tty->tty_files);
	check_tty_count(tty, "tty_open");
#if 0
	if (tty->driver->type == TTY_DRIVER_TYPE_PTY &&
	    tty->driver->subtype == PTY_TYPE_MASTER)
		noctty = 1;
#endif
	if (!retval) {
		if (tty->driver->open)
			retval = tty->driver->open(tty, filp);
		else
			retval = -ENODEV;
	}
	filp->flags = saved_flags;

	if (!retval && test_bit(TTY_EXCLUSIVE, &tty->flags) && !capable(CAP_SYS_ADMIN))
		retval = -EBUSY;

	if (retval) {
		release_dev(filp);
		if (retval != -ERESTARTSYS)
			return retval;
		if (signal_pending(current))
			return retval;
		schedule();
		/*
		 * Need to reset file_ops in case a hangup happened.
		 */
		if (filp->file_ops == &hung_up_tty_fops)
			filp->file_ops = &tty_fops;
		goto retry_open;
	}
#if 0
	if (!noctty &&
	    current->signal->leader &&
	    !current->signal->tty &&
	    tty->session == 0) {
	    	task_lock(current);
		current->signal->tty = tty;
		task_unlock(current);
		current->signal->tty_old_pgrp = 0;
		tty->session = current->signal->session;
		tty->pgrp = process_group(current);
	}
#endif
	return 0;
}


void tty_set_operations(struct tty_driver *driver, struct tty_operations *op)
{
	driver->open = op->open;
	driver->close = op->close;
	driver->write = op->write;
	driver->put_char = op->put_char;
	driver->flush_chars = op->flush_chars;
	driver->write_room = op->write_room;
	driver->chars_in_buffer = op->chars_in_buffer;
	driver->ioctl = op->ioctl;
	driver->set_termios = op->set_termios;
	driver->throttle = op->throttle;
	driver->unthrottle = op->unthrottle;
	driver->stop = op->stop;
	driver->start = op->start;
	driver->hangup = op->hangup;
	driver->break_ctl = op->break_ctl;
	driver->flush_buffer = op->flush_buffer;
	driver->set_ldisc = op->set_ldisc;
	driver->wait_until_sent = op->wait_until_sent;
	driver->send_xchar = op->send_xchar;
	driver->read_proc = op->read_proc;
	driver->write_proc = op->write_proc;
	driver->tiocmget = op->tiocmget;
	driver->tiocmset = op->tiocmset;
}

static int tty_fasync(int fd, struct file * filp, int on)
{
#if 0
	struct tty_struct * tty;
	int retval;

	tty = (struct tty_struct *)filp->private_data;

	retval = fasync_helper(fd, filp, on, &tty->fasync);
	if (retval <= 0)
		return retval;

	if (on) {
		if (!waitqueue_active(&tty->read_wait))
			tty->minimum_to_wake = 1;
		retval = f_setown(filp, (-tty->pgrp) ? : current->pid, 0);
		if (retval)
			return retval;
	} else {
		if (!tty->fasync && !waitqueue_active(&tty->read_wait))
			tty->minimum_to_wake = N_TTY_BUF_SIZE;
	}
	return 0;
#else
	return 0;
#endif
}

static struct file_ops tty_fops = {
	.llseek		= no_llseek,
	.read		= tty_read,
	.write		= tty_write,
	.poll		= tty_poll,
	.ioctl		= tty_ioctl,
	.open		= tty_open,
	.release	= tty_release,
	.fasync		= tty_fasync,
};

static struct file_ops console_fops = {
	.llseek		= no_llseek,
	.read		= tty_read,
	.write		= redirected_tty_write,
	.poll		= tty_poll,
	.ioctl		= tty_ioctl,
	.open		= tty_open,
	.release	= tty_release,
	.fasync		= tty_fasync,
};


/**
 *	tty_ldisc_flush	-	flush line discipline queue
 *	@tty: tty
 *
 *	Flush the line discipline queue (if any) for this tty. If there
 *	is no line discipline active this is a no-op.
 */
 
void tty_ldisc_flush(struct tty_struct *tty)
{
	struct tty_ldisc *ld = tty_ldisc_ref(tty);
	if(ld) {
		if(ld->flush_buffer)
			ld->flush_buffer(tty);
		tty_ldisc_deref(ld);
	}
}

/*
 * The default put_char routine if the driver did not define one.
 */
static void tty_default_put_char(struct tty_struct *tty, unsigned char ch)
{
	tty->driver->write(tty, &ch, 1);
}


/**
 * tty_register_device - register a tty device
 * @driver: the tty driver that describes the tty device
 * @index: the index in the tty driver for this tty device
 * @device: a struct device that is associated with this tty device.
 *	This field is optional, if there is no known struct device for this
 *	tty device it can be set to NULL safely.
 *
 * This call is required to be made to register an individual tty device if
 * the tty driver's flags have the TTY_DRIVER_NO_DEVFS bit set.  If that
 * bit is not set, this function should not be called.
 */
/**
 * 注册tty设备。
 *		driver:		所属驱动。
 *		index:		设备的次设备号。
 *		device:		要注册的设备。
 */
void tty_register_device(struct tty_driver *driver, unsigned index,
			 struct device *device)
{
	char name[64];
	devno_t dev = MKDEV(driver->major, driver->minor_start) + index;

	if (index >= driver->num) {
		printk(KERN_ERR "Attempt to register invalid tty line number "
		       " (%d).\n", index);
		return;
	}

	devfs_mk_chrdev(dev, S_IFCHR | S_IRUSR | S_IWUSR,
			"%s%d", driver->sysobj_name, index + driver->name_base);
#if 0
	if (driver->type == TTY_DRIVER_TYPE_PTY)
		pty_line_name(driver, index, name);
	else
		tty_line_name(driver, index, name);
	class_simple_device_add(tty_class, dev, device, name);
#else
	tty_line_name(driver, index, name);
#endif
}

/*
 * Called by a tty driver to register itself.
 */
/**
 * 向tty核心注册一个tty设备驱动程序。
 */
int tty_register_driver(struct tty_driver *driver)
{
	int error;
        int i;
	devno_t dev;
	void **p = NULL;

	if (driver->flags & TTY_DRIVER_INSTALLED)
		return 0;

	if (!(driver->flags & TTY_DRIVER_DEVPTS_MEM)) {
		p = kmalloc(driver->num * 3 * sizeof(void *), PAF_KERNEL);
		if (!p)
			return -ENOMEM;
		memset(p, 0, driver->num * 3 * sizeof(void *));
	}

	if (!driver->major) {
		error = alloc_chrdev_region(&dev, driver->minor_start, driver->num,
						(char*)driver->name);
		if (!error) {
			driver->major = MAJOR(dev);
			driver->minor_start = MINOR(dev);
		}
	} else {
		dev = MKDEV(driver->major, driver->minor_start);
		error = register_chrdev_region(dev, driver->num,
						(char*)driver->name);
	}
	if (error < 0) {
		kfree(p);
		return error;
	}

	if (p) {
		driver->ttys = (struct tty_struct **)p;
		driver->termios = (struct termios **)(p + driver->num);
		driver->termios_locked = (struct termios **)(p + driver->num * 2);
	} else {
		driver->ttys = NULL;
		driver->termios = NULL;
		driver->termios_locked = NULL;
	}

	cdev_init(&driver->cdev, &tty_fops);
	//driver->cdev.owner = driver->owner;
	error = putin_chrdev_container(&driver->cdev, dev, driver->num);
	if (error) {
		takeout_chrdev_container(&driver->cdev);
		unregister_chrdev_region(dev, driver->num);
		driver->ttys = NULL;
		driver->termios = driver->termios_locked = NULL;
		kfree(p);
		return error;
	}

	if (!driver->put_char)
		driver->put_char = tty_default_put_char;
	
	list_insert_front(&driver->tty_drivers, &tty_drivers);
	
	if ( !(driver->flags & TTY_DRIVER_NO_DEVFS) ) {
		for(i = 0; i < driver->num; i++)
		    tty_register_device(driver, i, NULL);
	}
	//proc_tty_register_driver(driver);
	return 0;
}


/**
 * tty_unregister_device - unregister a tty device
 * @driver: the tty driver that describes the tty device
 * @index: the index in the tty driver for this tty device
 *
 * If a tty device is registered with a call to tty_register_device() then
 * this function must be made when the tty device is gone.
 */
void tty_unregister_device(struct tty_driver *driver, unsigned index)
{
	devfs_remove("%s%d", driver->sysobj_name, index + driver->name_base);
	//class_simple_device_remove(MKDEV(driver->major, driver->minor_start) + index);
}

/*
 * Called by a tty driver to unregister itself.
 */
int tty_unregister_driver(struct tty_driver *driver)
{
	int i;
	struct termios *tp;
	void *p;

	if (driver->refcount)
		return -EBUSY;

	unregister_chrdev_region(MKDEV(driver->major, driver->minor_start),
				driver->num);

	list_del(&driver->tty_drivers);

	/*
	 * Free the termios and termios_locked structures because
	 * we don't want to get memory leaks when modular tty
	 * drivers are removed from the kernel.
	 */
	for (i = 0; i < driver->num; i++) {
		tp = driver->termios[i];
		if (tp) {
			driver->termios[i] = NULL;
			kfree(tp);
		}
		tp = driver->termios_locked[i];
		if (tp) {
			driver->termios_locked[i] = NULL;
			kfree(tp);
		}
		if (!(driver->flags & TTY_DRIVER_NO_DEVFS))
			tty_unregister_device(driver, i);
	}
	p = driver->ttys;
	//proc_tty_unregister_driver(driver);
	driver->ttys = NULL;
	driver->termios = driver->termios_locked = NULL;
	kfree(p);
	takeout_chrdev_container(&driver->cdev);
	return 0;
}


/**
 *	tty_termios_baud_rate
 *	@termios: termios structure
 *
 *	Convert termios baud rate data into a speed. This should be called
 *	with the termios lock held if this termios is a terminal termios
 *	structure. May change the termios data.
 */
 
int tty_termios_baud_rate(struct termios *termios)
{
	unsigned int cbaud;
	
	cbaud = termios->c_cflag & CBAUD;

	if (cbaud & CBAUDEX) {
		cbaud &= ~CBAUDEX;

		if (cbaud < 1 || cbaud + 15 > n_baud_table)
			termios->c_cflag &= ~CBAUDEX;
		else
			cbaud += 15;
	}
	return baud_table[cbaud];
}

void tty_vhangup(struct tty_struct * tty)
{
	do_tty_hangup((void *) tty);
}

void tty_hangup(struct tty_struct * tty)
{
#ifdef TTY_DEBUG_HANGUP
	char	buf[64];
	
	printk(KERN_DEBUG "%s hangup...\n", tty_name(tty, buf));
#endif
	schedule_work(&tty->hangup_work);
}

/**
 *	tty_flip_buffer_push	-	terminal
 *	@tty: tty to push
 *
 *	Queue a push of the terminal flip buffers to the line discipline. This
 *	function must not be called from IRQ context if tty->low_latency is set.
 *
 *	In the event of the queue being busy for flipping the work will be
 *	held off and retried later.
 */

void tty_flip_buffer_push(struct tty_struct *tty)
{
	if (tty->low_latency)
		flush_to_ldisc((void *) tty);
	else
		schedule_delayed_work(&tty->flip.work, 1);
}


/*
 * This implements the "Secure Attention Key" ---  the idea is to
 * prevent trojan horses by killing all processes associated with this
 * tty when the user hits the "Secure Attention Key".  Required for
 * super-paranoid applications --- see the Orange Book for more details.
 * 
 * This code could be nicer; ideally it should send a HUP, wait a few
 * seconds, then send a INT, and then a KILL signal.  But you then
 * have to coordinate with the init process, since all processes associated
 * with the current tty must be dead before the new getty is allowed
 * to spawn.
 *
 * Now, if it would be correct ;-/ The current code has a nasty hole -
 * it doesn't catch files in flight. We may send the descriptor to ourselves
 * via AF_UNIX socket, close it and later fetch from socket. FIXME.
 *
 * Nasty bug: do_SAK is being called in interrupt context.  This can
 * deadlock.  We punt it up to process context.  AKPM - 16Mar2001
 */
static void __do_SAK(void *arg)
{
#if 1
	struct tty_struct *tty = arg;
	tty_hangup(tty);
#else
#ifdef TTY_SOFT_SAK
	tty_hangup(tty);
#else
	struct tty_struct *tty = arg;
	struct task_desc *p;
	int session;
	int		i;
	struct file	*filp;
	struct tty_ldisc *disc;
	
	if (!tty)
		return;
	session  = tty->session;
	
	/* We don't want an ldisc switch during this */
	disc = tty_ldisc_ref(tty);
	if (disc && disc->flush_buffer)
		disc->flush_buffer(tty);
	tty_ldisc_deref(disc);

	if (tty->driver->flush_buffer)
		tty->driver->flush_buffer(tty);
	
	smp_read_lock(&tasklist_lock);
	do_each_task_pid(session, PIDTYPE_SID, p) {
		if (p->signal->tty == tty || session > 0) {
			printk(KERN_NOTICE "SAK: killed process %d"
			    " (%s): p->signal->session==tty->session\n",
			    p->pid, p->comm);
			post_signal(SIGKILL, p, 1);
			continue;
		}
		task_lock(p);
		if (p->files) {
			smp_lock(&p->files->file_lock);
			for (i=0; i < p->files->max_handle; i++) {
				filp = fcheck_files(p->files, i);
				if (!filp)
					continue;
				if (filp->file_ops->read == tty_read &&
				    filp->private_data == tty) {
					printk(KERN_NOTICE "SAK: killed process %d"
					    " (%s): fd#%d opened to the tty\n",
					    p->pid, p->comm, i);
					post_signal(SIGKILL, p, 1);
					break;
				}
			}
			smp_unlock(&p->files->file_lock);
		}
		task_unlock(p);
	} while_each_task_pid(session, PIDTYPE_SID, p);
	smp_read_unlock(&tasklist_lock);
#endif
#endif
}

/*
 * The tq handling here is a little racy - tty->SAK_work may already be queued.
 * Fortunately we don't need to worry, because if ->SAK_work is already queued,
 * the values which we write to it will be identical to the values which it
 * already has. --akpm
 */
void do_SAK(struct tty_struct *tty)
{
	if (!tty)
		return;
	PREPARE_WORK(&tty->SAK_work, __do_SAK, tty);
	schedule_work(&tty->SAK_work);
}

int __init init_tty(void)
{
	tty_register_ldisc(N_TTY, &tty_ldisc_N_TTY);
	cdev_init(&console_cdev, &console_fops);
	if (putin_chrdev_container(&console_cdev, MKDEV(TTYAUX_MAJOR, 1), 1) ||
	    register_chrdev_region(MKDEV(TTYAUX_MAJOR, 1), 1, "/dev/console") < 0)
		panic("Couldn't register /dev/console driver\n");
	devfs_mk_chrdev(MKDEV(TTYAUX_MAJOR, 1), S_IFCHR|S_IRUSR|S_IWUSR, "console");
	//class_simple_device_add(tty_class, MKDEV(TTYAUX_MAJOR, 1), NULL, "console");

	return 0;
}

