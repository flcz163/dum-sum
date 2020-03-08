#ifndef __DIM_SUM_BIO_H
#define __DIM_SUM_BIO_H

#include <dim-sum/blk_dev.h>
#include <dim-sum/errno.h>
#include <dim-sum/mm.h>

struct block_io_desc;

enum {
	/**
	 * IO操作已经*成功*完成
	 * 因此内存中的数据是最新的
	 */
	__BIOFLAG_UPTODATE,
	/**
	 * 需要重新统计计数
	 */
	__BIOFLAG_RECOUNT,
	/**
	 * 写请求
	 */
	__BIOFLAG_WRITE,
	/**
	 * 预读请求
	 */
	__BIOFLAG_READAHEAD,
	/**
	 * IO屏障
	 */
	__BIOFLAG_BARRIER,
	/**
	 * 同步请求
	 */
	__BIOFLAG_SYNC,
	/**
	 * 不重要的请求，失败后不用重试
	 */
	__BIOFLAG_NORETRY,
	/**
	 * 超过设备扇区范围的请求
	 */
	__BIOFLAG_EOF,
	/**
	 * 不被支持的操作
	 */
	__BIOFLAG_EOPNOTSUPP,
};

/**
 * BIO描述符标志
 */
#define BIOFLAG_UPTODATE	(1UL << __BIOFLAG_UPTODATE)
#define BIOFLAG_RECOUNT	(1UL << __BIOFLAG_RECOUNT)
#define BIOFLAG_WRITE		(1UL << __BIOFLAG_WRITE)
#define BIOFLAG_READAHEAD	(1UL << __BIOFLAG_READAHEAD)
#define BIOFLAG_BARRIER		(1UL << __BIOFLAG_BARRIER)
#define BIOFLAG_SYNC		(1UL << __BIOFLAG_SYNC)
#define BIOFLAG_NORETRY	(1UL << __BIOFLAG_NORETRY)
#define BIOFLAG_EOF			(1UL << __BIOFLAG_EOF)
#define BIOFLAG_EOPNOTSUPP	(1UL << __BIOFLAG_EOPNOTSUPP)

typedef int (*bio_end_io_t) (struct block_io_desc *, unsigned int, int);
typedef void (*bio_destructor_t) (struct block_io_desc *);

/*
 * was unsigned short, but we might as well be ready for > 64kB I/O pages
 */
struct block_io_item {
	struct page_frame	*bv_page;
	unsigned int	length;
	unsigned int	bv_offset;
};

#define BLKDEV_MIN_RQ	4
/**
 * 队列中最大的请求数量
 */
#define BLKDEV_MAX_RQ	128

#define MAX_PHYS_SEGMENTS 128
#define MAX_HW_SEGMENTS 128
#define MAX_SECTORS 255

/*
 * main unit of I/O for the block layer and lower layers (ie drivers and
 * stacking drivers)
 */
struct block_io_desc {
	/**
	 * 引用计数
	 */
	struct accurate_counter		ref_count;
	/**
	 * 所能容纳的最大项
	 */
	unsigned int			max_item_count;
	/**
	 * 当前有效的项目数量
	 */
	unsigned short		item_count;
	/**
	 * 读写请求的起始扇区(512字节的逻辑扇区)
	 */
	sector_t		start_sector;

	struct block_io_desc		*bi_next;	/* request queue link */
	struct block_device	*bi_bdev;
	unsigned long		bi_flags;	/* status, command, etc */

	/**
	 * 剩余的，需要传送给设备的数量
	 */
	unsigned int		remain_size;
	/**
	 * 释放bio时调用的析构方法（通常是bio_destructor方法）
	 */
	bio_destructor_t	free;	/* destructor */

	/* Number of segments after physical and DMA remapping
	 * hardware coalescing is performed.
	 */
	/**
	 * 合并之后硬件段的数目。
	 */
	unsigned short		hw_segments;

	bio_end_io_t		finish;

	void			*bi_private;

	/**
	 * 当前未提交的块请求索引号
	 */
	unsigned short		bi_idx;
	/**
	 * 保存io请求项的数组，创建描述符时分配
	 * 必须放在最后!
	 */
	struct block_io_item		items[0];
};

static inline bool bio_is_write(struct block_io_desc *bio)
{
	return !!(bio->bi_flags & BIOFLAG_WRITE);
}

#define BIO_RW		0
/**
 * bio->bi_rw的标志值之一。表示本次IO操作是一次预读。
 * 当一个bio是预读，并且没有足够的内存时，就会直接退出并调用bio_endio.
 */
#define BIO_RW_AHEAD	1
#define BIO_RW_BARRIER	2
#define BIO_RW_FAILFAST	3
/**
 * 同步读写标志。
 * 如果设置了本标志，那么__make_request退出，就会从请求队列中摘除
 */
#define BIO_RW_SYNC	4

#define bio_flagged(bio, flag)	((bio)->bi_flags & (1 << (flag)))

extern void loosen_blkio(struct block_io_desc *);

#define BIO_BUG_ON	BUG_ON

#define BIO_MAX_PAGES		256
#define BIO_MAX_SIZE		(BIO_MAX_PAGES << PAGE_CACHE_SHIFT)
#define BIO_MAX_SECTORS		(BIO_MAX_SIZE >> 9)


#define bio_iovec_idx(bio, idx)	(&((bio)->items[(idx)]))
#define bio_iovec(bio)		bio_iovec_idx((bio), (bio)->bi_idx)
#define bio_page(bio)		bio_iovec((bio))->bv_page
#define bio_offset(bio)		bio_iovec((bio))->bv_offset
#define bio_segments(bio)	((bio)->item_count - (bio)->bi_idx)
#define bio_sectors(bio)	((bio)->remain_size >> 9)
#define bio_end_sector(bio)	((bio)->start_sector + bio_sectors((bio)))

static inline unsigned int bio_cur_bytes(struct block_io_desc *bio)
{
	if (bio->item_count)
		return bio_iovec(bio)->length;
	else /* dataless requests such as discard */
		return bio->remain_size;
}

static inline void *bio_data(struct block_io_desc *bio)
{
	if (bio->item_count)
		return page_address(bio_page(bio)) + bio_offset(bio);

	return NULL;
}

#define bvec_to_phys(bv)	(page_to_phys_addr((bv)->bv_page) + (unsigned long) (bv)->bv_offset)

/*
 * merge helpers etc
 */
#ifndef BIO_VMERGE_BOUNDARY
#define BIO_VMERGE_BOUNDARY	0
#endif

#define __BVEC_END(bio)		bio_iovec_idx((bio), (bio)->item_count - 1)
#define __BVEC_START(bio)	bio_iovec_idx((bio), (bio)->bi_idx)

/* Default implementation of BIOVEC_PHYS_MERGEABLE */
#define __BIOVEC_PHYS_MERGEABLE(vec1, vec2)	\
	((bvec_to_phys((vec1)) + (vec1)->length) == bvec_to_phys((vec2)))

/*
 * allow arch override, for eg virtualized architectures (put in asm/io.h)
 */
#ifndef BIOVEC_PHYS_MERGEABLE
#define BIOVEC_PHYS_MERGEABLE(vec1, vec2)	\
	((bvec_to_phys((vec1)) + (vec1)->length) == bvec_to_phys((vec2)))
#endif

#define __BIO_SEG_BOUNDARY(addr1, addr2, mask) \
	(((addr1) | (mask)) == (((addr2) - 1) | (mask)))
#define BIOVEC_SEG_BOUNDARY(q, b1, b2) \
	__BIO_SEG_BOUNDARY(bvec_to_phys((b1)), bvec_to_phys((b2)) + (b2)->length, (q)->request_settings.addr_boundary)
#define BIO_SEG_BOUNDARY(q, b1, b2) \
	BIOVEC_SEG_BOUNDARY((q), __BVEC_END((b1)), __BVEC_START((b2)))

#define bio_io_error(bio) blkio_finished((bio), -EIO)

/*
 * drivers should not use the __ version unless they _really_ know what
 * they're doing
 */
#define __bio_for_each_segment(bvl, bio, i, start_idx)			\
	for (bvl = bio_iovec_idx((bio), (start_idx)), i = (start_idx);	\
	     i < (bio)->item_count;					\
	     bvl++, i++)

#define bio_for_each_item(bvl, bio, i)				\
	for (i = (bio)->bi_idx;						\
	     bvl = bio_iovec_idx((bio), (i)), i < (bio)->item_count;	\
	     i++)

struct block_io_desc *blkio_alloc(int paf_mask, int nr_iovecs);

/*
 * 获得BIO的引用计数
 */
#define hold_blkio(bio)	accurate_inc(&(bio)->ref_count)
extern void loosen_blkio(struct block_io_desc *);

extern void blkio_finished(struct block_io_desc *, unsigned int, int);
struct blk_request_queue;

extern void bio_init(struct block_io_desc *);

extern int blkio_add_page(struct block_io_desc *, struct page_frame *, unsigned int,unsigned int);
extern int blkdev_get_max_pages(struct block_device *);

/**
 * 在当前页中传输的扇区数量。
 */
#define bio_fire_sectors(bio)	(bio_iovec(bio)->length >> 9)
/**
 * 被传输数据的内核逻辑地址。只有正在处理的页不在高端内存时，该地址才有效。
 * 默认情况下，块设备子系统不会把高端内存中的缓冲区传递给驱动程序，但是如果使用blk_queue_bounce_limit改变了这一设置，就不应该再使用bio_data了。
 */
#define bio_data(bio)		(page_address(bio_page((bio))) + bio_offset((bio)))
#define bio_is_barrier(bio)	((bio)->bi_flags & BIOFLAG_BARRIER)
#define bio_sync(bio)		((bio)->bi_flags & BIOFLAG_SYNC)
#define bio_noretry(bio)	((bio)->bi_flags & BIOFLAG_NORETRY)
#define bio_rw_ahead(bio)	((bio)->bi_flags & BIOFLAG_READAHEAD)

#endif /* __DIM_SUM_BIO_H */
