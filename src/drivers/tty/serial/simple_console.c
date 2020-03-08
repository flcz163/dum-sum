#include <dim-sum/errno.h>
#include <dim-sum/init.h>
#include <dim-sum/serial_core.h>
#include <dim-sum/simple_console.h>

static struct console simple_console = {
	.name =		"uart",
	.flags =	CONSOLE_PRINTK | CONSOLE_SIMPLE,
	.index =	-1,
};

static struct simple_console_device simple_console_dev = {
	.con = &simple_console,
};

static int __init register_simple_console(char *buf,
		const struct simple_console_id *match)
{
	int err;
 
	simple_console_dev.con->data = &simple_console_dev;
	err = match->init(&simple_console_dev, buf);
	if (err < 0)
		return err;
	if (!simple_console_dev.con->write)
		return -ENODEV;

	register_console(simple_console_dev.con);
	return 0;
}

extern struct simple_console_id simple_console_pl011;

int __init init_simple_console(char *buf)
{
	register_simple_console(buf, &simple_console_pl011);

	printk("simple console is ready.\n");

	return 0;
}
