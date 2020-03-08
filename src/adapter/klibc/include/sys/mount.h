/*
 * sys/mount.h
 */

#ifndef _SYS_MOUNT_H
#define _SYS_MOUNT_H

#include <klibc/extern.h>
#include <sys/ioctl.h>

/*
 * Superblock flags that can be altered by MS_REMOUNT
 */
#define MS_RMT_MASK     (MFLAG_RDONLY|MS_SYNCHRONOUS|MS_MANDLOCK|MS_NOATIME|MS_NODIRATIME)

/*
 * umount2() flags
 */
#define MNT_FORCE	1	/* Forcibly unmount */
#define MNT_DETACH	2	/* Detach from tree only */
#define MNT_EXPIRE	4	/* Mark for expiry */

/*
 * Block device ioctls
 */
#define BLKROSET   _IO(0x12, 93)	/* Set device read-only (0 = read-write).  */
#define BLKROGET   _IO(0x12, 94)	/* Get read-only status (0 = read_write).  */
#define BLKRRPART  _IO(0x12, 95)	/* Re-read partition table.  */
#define BLKGETSIZE _IO(0x12, 96)	/* Return device size.  */
#define BLKFLSBUF  _IO(0x12, 97)	/* Flush buffer cache.  */
#define BLKRASET   _IO(0x12, 98)	/* Set read ahead for block device.  */
#define BLKRAGET   _IO(0x12, 99)	/* Get current read ahead setting.  */

/*
 * Prototypes
 */
__extern int mount(const char *, const char *,
		   const char *, unsigned long, const void *);
__extern int umount(const char *);
__extern int umount2(const char *, int);
__extern int pivot_root(const char *, const char *);

#endif				/* _SYS_MOUNT_H */
