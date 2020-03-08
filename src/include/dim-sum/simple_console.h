#ifndef _DIM_SUM_SIMPLE_CONSOLE_H
#define _DIM_SUM_SIMPLE_CONSOLE_H

struct simple_console_device {
	struct console *con;
	void *priv;
};

struct simple_console_id {
	char	name[16];
	int	(*init)(struct simple_console_device *, const char *options);
} __aligned(32);

#endif /* _DIM_SUM_SIMPLE_CONSOLE_H */
