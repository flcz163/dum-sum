#ifndef _UAPI__LINUX_UIO_H
#define _UAPI__LINUX_UIO_H

#include <linux/compiler.h>
#include <linux/types.h>

struct io_segment
{
	void __user *base;
	__kernel_size_t len;
};

#endif /* _UAPI__LINUX_UIO_H */
