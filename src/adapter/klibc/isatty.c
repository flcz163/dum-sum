/*
 * isatty.c
 */

#include <unistd.h>
#include <termios.h>
#include <errno.h>

int isatty(int fd)
{
#if 0
	int dummy;

	/* All ttys support TIOCGPGRP */
	return !ioctl(fd, TIOCGPGRP, &dummy);
#else
	return fd <= 2;
#endif
}
