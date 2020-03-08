#ifndef __DIM_SUM_MAGIC_H
#define __DIM_SUM_MAGIC_H

#define MAGIC_BASE (0x761203)
#define TIMER_VERIFY_MAGIC         (MAGIC_BASE + 0x1)
#define TASK_MAGIC	(MAGIC_BASE + 0x2)

#define LIST_UNLINK1  ((void *) MAGIC_BASE + 0x3)
#define LIST_UNLINK2  ((void *) MAGIC_BASE + 0x4)
#define BLKFS_MAGIC	(MAGIC_BASE + 0x5)
#define RAMFS_MAGIC	(MAGIC_BASE + 0x6)
#define DEVFS_SUPER_MAGIC                (MAGIC_BASE + 0x7)

#define MEMORY_SLICE_INUSE	0xaa
#define MEMORY_SLICE_FREE	0xbb
#define MEMORY_SLICE_END	0xcc

#define STACK_MAGIC	0xdeadbeef

#define MSGQ_MAGIC 0x37210709
#define TIMER_MAGIC	0x4b87ad6e
#define TTY_MAGIC		0x5401
#define TTY_LDISC_MAGIC	0x5403
#endif /* __DIM_SUM_MAGIC_H */
