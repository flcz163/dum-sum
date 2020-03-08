#ifndef _LINUX_DIRENT_H
#define _LINUX_DIRENT_H

#include <linux/limits.h>

/* The kernel calls this struct dirent64 */
struct dirent {
	unsigned long long	d_ino;
	long long int		d_off;
	unsigned short	d_reclen;
	unsigned char	d_type;
	char		d_name[256];
};

struct dirent64 {
	unsigned long long		d_ino;
	signed long long		d_off;
	unsigned short	d_reclen;
	unsigned char	d_type;
	char		d_name[256];
};


#endif

