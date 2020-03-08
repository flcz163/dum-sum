#ifndef __DIM_SUM_ERR_H
#define __DIM_SUM_ERR_H

#include <linux/compiler.h>

#include <asm/errno.h>

static inline void *ERR_PTR(long error)
{
	return (void *) error;
}

static inline long PTR_ERR(const void *ptr)
{
	return (long) ptr;
}

static inline long IS_ERR(const void *ptr)
{
	return unlikely((unsigned long)ptr > (unsigned long)-1000L);
}

#endif /* __DIM_SUM_ERR_H */
