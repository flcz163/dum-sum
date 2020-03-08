#ifndef __DIM_SUM_KDEV_T_H
#define __DIM_SUM_KDEV_T_H

#include <dim-sum/types.h>

#define MAXMAJOR	(1UL << 12)
#define MINORBITS	20
#define MINORMASK	((1U << MINORBITS) - 1)

#define INVDEVNO	(-1)
#define MAJOR(dev)	((unsigned int) ((dev) >> MINORBITS))
#define MINOR(dev)	((unsigned int) ((dev) & MINORMASK))
#define MKDEV(ma,mi)	(((ma) << MINORBITS) | (mi))

static inline u32 devno_to_uint(devno_t dev)
{
	unsigned major = MAJOR(dev);
	unsigned minor = MINOR(dev);

	return (minor & 0xff) | (major << 8) | ((minor & ~0xff) << 12);
}

static inline devno_t uint_to_devno(u32 dev)
{
	unsigned major = (dev & 0xfff00) >> 8;
	unsigned minor = (dev & 0xff) | ((dev >> 12) & 0xfff00);

	return MKDEV(major, minor);
}

#endif /* __DIM_SUM_KDEV_T_H */
