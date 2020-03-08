#ifndef _DIM_SUM_UIO_H
#define _DIM_SUM_UIO_H

#include <uapi/linux/uio.h>

static inline size_t
iosegments_length(const struct io_segment *segs,
	unsigned long seg_count)
{
	int i;
	size_t ret = 0;

	for (i = 0; i < seg_count; i++)
		ret += segs[i].len;

	return ret;
}

#endif /* _DIM_SUM_UIO_H */