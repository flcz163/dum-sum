/*
 * sleep.c
 */

#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

unsigned int sleep(unsigned int msec)
{
	struct timespec ts;

	ts.tv_sec = msec / 1000;
	ts.tv_nsec = msec % 1000 * 1000 * 1000;
	if (!nanosleep(&ts, &ts))
		return 0;
	else if (errno == EINTR)
		return ts.tv_sec;
	else
		return -1;
}
