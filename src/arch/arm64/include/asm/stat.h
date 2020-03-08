#ifndef __ASMARM_STAT_H
#define __ASMARM_STAT_H

#include <dim-sum/devno.h>
#include <dim-sum/errno.h>
#include <dim-sum/string.h>
#include <dim-sum/uaccess.h>

#include <uapi/asm/stat.h>

static inline int copy_stat_to_user(struct file_attribute *attr, struct stat *stat)
{
	struct stat tmp = {0};

	tmp.st_dev = devno_to_uint(attr->dev);
	tmp.st_ino = attr->ino;
	tmp.st_mode = attr->mode;
	tmp.st_nlink = attr->nlink;
	tmp.st_rdev = devno_to_uint(attr->rdev);
	tmp.st_size = attr->size;
	tmp.st_atime = attr->atime.tv_sec;
	tmp.st_mtime = attr->mtime.tv_sec;
	tmp.st_ctime = attr->ctime.tv_sec;

	if (tmp.st_nlink != attr->nlink)
		return -EOVERFLOW;

	return copy_to_user(stat, &tmp, sizeof(tmp));
}

#endif /* __ASMARM_STAT_H */
