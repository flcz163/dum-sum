/*
 * sys/uio.h
 */

#ifndef _SYS_UIO_H
#define _SYS_UIO_H

#include <klibc/extern.h>
#include <sys/types.h>
#include <linux/uio.h>

__extern int readv(int, const struct io_segment *, int);
__extern int writev(int, const struct io_segment *, int);

#endif				/* _SYS_UIO_H */
