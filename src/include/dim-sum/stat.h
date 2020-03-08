#ifndef _DIM_SUM_STAT_H
#define _DIM_SUM_STAT_H

#include <dim-sum/types.h>
#include <dim-sum/time.h>

struct file_attribute {
	unsigned long	ino;
	devno_t		dev;
	umode_t		mode;
	unsigned int	nlink;
	uid_t		uid;
	gid_t		gid;
	devno_t		rdev;
	loff_t		size;
	struct timespec  atime;
	struct timespec	mtime;
	struct timespec	ctime;
	unsigned long	blksize;
	unsigned long	blocks;
};

#include <asm/stat.h>

#endif /* _DIM_SUM_STAT_H */
