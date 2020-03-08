#ifndef __DIM_SUM_BLK_INFRAST_H
#define __DIM_SUM_BLK_INFRAST_H

#include <dim-sum/mm_types.h>

typedef int (congested_fn)(void *, int);

enum {
	__BLK_WRITE_CONGESTED,
	__BLK_READ_CONGESTED,
};

#define BLK_WRITE_CONGESTED	(1UL << __BLK_WRITE_CONGESTED)
#define BLK_READ_CONGESTED		(1UL << __BLK_READ_CONGESTED)

/**
 * 块设备层向文件层提供的基础结构描述符
 */
struct blkdev_infrast {
	unsigned long state;
	/**
	 * 最大的预读页面
	 */
	unsigned long max_ra_pages;
	/**
	 * 内存设备，不用回写
	 */
	int mem_device;
	/**
	 * 向设备推送IO请求的方法
	 */
	void (*push_io)(struct blkdev_infrast *, struct page_frame *);
	void *push_io_data;
};

extern struct blkdev_infrast default_blkdev_infrast;

static inline int blkdev_read_congested(struct blkdev_infrast *blkdev)
{
	return blkdev->state & BLK_READ_CONGESTED;
}

static inline int blkdev_write_congested(struct blkdev_infrast *blkdev)
{
	return blkdev->state & BLK_WRITE_CONGESTED;
}

#endif /* __DIM_SUM_BLK_INFRAST_H */
