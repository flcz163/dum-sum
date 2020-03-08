#ifndef __DIM_SUM_BLOCK_DEVICE_H
#define __DIM_SUM_BLOCK_DEVICE_H

#include <dim-sum/blk_infrast.h>
#include <dim-sum/iosched.h>
#include <dim-sum/major.h>
#include <dim-sum/semaphore.h>
#include <dim-sum/wait.h>
#include <dim-sum/disk.h>
#include <dim-sum/mutex.h>
#include <dim-sum/object.h>
#include <dim-sum/timer.h>
#include <dim-sum/workqueue.h>
#include <dim-sum/pagemap.h>

struct scatterlist;
struct block_device;
struct disk_device;
struct blk_request;
struct block_io_desc;
struct page_frame;
struct block_device;
struct block_io_desc;
struct block_io_item;
struct ioscheduler;
struct work_struct;

extern struct super_block *blkfs_superblock;

/**
 * 块设备的回调方法
 */
struct block_device_operations {
	/**
	 * 打开块设备文件
	 */
	int (*open) (struct block_device *, mode_t mode);
	/**
	 * 关闭对块设备文件的最后一个引用
	 */
	int (*release) (struct file_node *, struct file *);
	int (*ioctl) (struct block_device *, fmode_t, unsigned, unsigned long);
};

/**
 * 块设备层最大的预读量
 */
#define BLK_MAX_READAHEAD	SZ_128K
#define BLK_MIN_READAHEAD	SZ_16K

#define SECTOR_TO_PAGE(sector) (pgoff_t)(sector >> (PAGE_CACHE_SHIFT-9))
#define SECOFF_IN_PAGE(sector) ((sector & ((1 << (PAGE_CACHE_SHIFT - 9)) - 1)) << 9)

/**
 * 一个块设备驱动程序可以处理几个块设备.
 * 例如：一个IDE驱动程序可以处理几个IDE磁盘。其中的每个都是一个单独的块设备。
 * 并且，每个磁盘都可以被分区。每个分区又可以被看成是一个逻辑设备。
 * 每个块设备都都是由block_device定义的。
 */
struct block_device {
	/**
	 * 块设备的主设备号和次设备号
	 */
	devno_t devno;
	/**
	 * 通过此字段，将块设备加入到全局块设备链表中
	 */
	struct double_list	list;
	/**
	 * 已打开的块设备文件节点链表头。
	 */
	struct double_list	inodes;
	/**
	 * 指向bdev文件系统中块设备对应的文件索引结点的指针。
	 */
	struct file_node *fnode;
	/**
	 * 块设备描述符的当前所有者
	 */
	void *holder;
	/**
	 * 计数器，设置排它访问的次数。
	 */
	int hold_count;
	/**
	 * 计数器，统计设备已经被打开了多少次
	 */
	int			open_count;
	/**
	 * 已经打开了分区多少次?
	 */
	int open_partitions;

	/**
	 * 如果设备是一个分区。则指向整个磁盘的块设备描述符。
	 * 否则，指向该块设备描述符
	 */
	struct block_device *container;
	/**
	 * 块设备所属的磁盘设备描述符
	 */
	struct disk_device *disk;
	/**
	 * 如果块设备是分区，则指向分区描述符
	 */
	struct partition_desc *partition;
	/**
	 * 分区信息是否同步
	 */
	int partition_uptodate;
	/**
	 * 计数器
	 * 在该块设备中，打开了多少个分区
	 */
	unsigned		partition_count;
	/**
	 * 指向块设备的专门描述符（通常为NULL）
	 */
	struct blkdev_infrast *blkdev_infrast;
	/**
	 * 用于保护mount操作的互斥信号量
	 */
	struct semaphore	mount_sem;
	/**
	 * 块大小 
	 */
	unsigned		bd_block_size;
	/**
	 * 保护块设备打开和关闭的信号量。
	 */
	struct semaphore	sem;

};

typedef int (*merge_bio_f) (struct blk_request_queue *, struct blk_request *,
				struct block_io_desc *);
/**
 * 块设备驱动处理请求队列的回调
 */
typedef void (*blkdev_engorge_queue_f) (struct blk_request_queue *queue);

enum {
	/**
	 * 屏障IO请求，要求刷新队列
	 */
	__BLKREQ_FLUSH,
	/**
	 * 强制绕过缓存
	 */
	__BLKREQ_NOCACHE,
	/**
	 * IO屏障，软件和硬件都不能使其乱序
	 */
	__BLKREQ_BARRIER,
	/**
	 * 标准的数据传送请求，读或者写
	 */
	__BLKREQ_DATA,		/* is a regular fs rw request */
	/**
	 * 正在处理的请求。
	 */
	__BLKREQ_STARTED,
	/**
	 * 打印详细信息
	 */
	__BLKREQ_VERBOSE,
	/**
	 * 由于某种原因，不宜将此请求与其他BIO合并
	 * 例如扇区数过多
	 */
	__BLKREQ_NOMERGE,
	/**
	 * 不重要的请求，底层遇到错误时不用重试
	 */
	__BLKREQ_NORETRY,	
};

#define BLKREQ_NOCACHE		(1 << __BLKREQ_NOCACHE)
#define BLKREQ_FLUSH		(1 << __BLKREQ_FLUSH)
#define BLKREQ_BARRIER			(1 << __BLKREQ_BARRIER)
#define BLKREQ_DATA			(1 << __BLKREQ_DATA)
#define BLKREQ_STARTED			(1 << __BLKREQ_STARTED)
#define BLKREQ_VERBOSE			(1 << __BLKREQ_VERBOSE)
#define BLKREQ_NORETRY	(1 << __BLKREQ_NORETRY)
#define BLKREQ_NOMERGE	(1 << __BLKREQ_NOMERGE)

enum {
	/**
	 * 驱动将队列暂停，不再接受请求*
	 * 由驱动调用blkdev_stop_request()设置
	 */
	__BLKQUEUE_STOPPED,
	/**
	 * 正在请求驱动，避免重入
	 */
	__BLKQUEUE_REQUESTING,
	/**
	 * 可以将多个请求合并起来
	 */
	__BLKQUEUE_COMBINED,
	/**
	 * 正在将队列中的请求清空
	 */
	__BLKQUEUE_DRAINING,
	/**
	 * 队列与设备已经关联起来了
	 * 设备可以接收IO请求，队列可以将请求推送给设备
	 */
	__BLKQUEUE_ATTACHED,
	/**
	 * 写请求
	 */
	__BLKQUEUE_WRITE,
};

#define BLKQUEUE_STOPPED	(1UL << __BLKQUEUE_STOPPED)
#define BLKQUEUE_REQUESTING 	(1UL << __BLKQUEUE_REQUESTING)
#define BLKQUEUE_COMBINED	(1UL << __BLKQUEUE_COMBINED)
#define BLKQUEUE_DRAINING	(1UL << __BLKQUEUE_DRAINING)
#define BLKQUEUE_ATTACHED	(1UL << __BLKQUEUE_ATTACHED)
#define BLKQUEUE_WRITE		(1UL << __BLKQUEUE_WRITE)

#define BLK_MAX_CDB 16

/**
 * 块设备请求。
 */
struct blk_request {
	/**
	 * 请求的引用计数。
	 */
	int ref_count;
	/**
	 * 用于把请求链接到请求队列中。
	 */
	struct double_list list;
	/**
	 * 请求标志
	 */
	unsigned long flags;
	/**
	 * 开始扇区号。
	 */
	sector_t start_sector;
	/**
	 * 要传输的扇区数。
	 */
	unsigned long sector_count;
	/**
	 * 请求的硬段数。
	 */
	unsigned short segcount_hw;
	/**
	 * 下一个待发射的扇区号。
	 */
	sector_t remain_sector_start;
	/**
	 * 剩余的，要传送的扇区数。由通用块层更新。
	 */
	unsigned long remain_sector_count;
	/**
	 * 当前bio的当前段中要传送的扇区数。
	 */
	unsigned int sectors_seg_drv;
	/**
	 * 当前bio的当前段中要传送的扇区数。由通用块层更新。
	 */
	unsigned int sectors_seg;
	/**
	 * 要传输或者要接收数据的缓冲区指针。该指针在内核虚拟地址中，如果有需要，驱动程序可以直接使用。
	 * 它是在当前bio中调用bio_data的结果。
	 * 如果是高端内存，则为NULL
	 */
	char *buffer;
	/**
	 * 等待数据传送终止的信号量。
	 */
	struct semaphore *waiting;
	/**
	 * 对硬件发出特殊命令的请求所使用的数据的指针。
	 */
	void *special;
	/**
	 * 请求的bio结构链表。
	 * 不能直接对该成员进行访问。而是使用rq_for_each_bio访问。
	 */
	struct block_io_desc *bio_head;
	/**
	 * 请求链表中末尾的bio
	 */
	struct block_io_desc *bio_tail;
	/**
	 * 指向IO调度程序私有数据的指针。
	 */
	void *sched_data;
	/**
	 * 请求状态，活动或者未活动。
	 */
	int rq_status;
	/**
	 * 请求所用的磁盘描述符。
	 */
	struct disk_device *disk;
	/**
	 * 当前传送中发生的IO失败次数的计数器。
	 */
	int errors;
	/**
	 * 请求的起始时间。
	 */
	unsigned long start_time;
	/**
	 * 指向包含请求的请求队列描述符指针。
	 */
	struct blk_request_queue *queue;

	/**
	 * cmd命令的长度
	 */
	unsigned int dev_cmd_len;
	/**
	 * 由请求队列的prep_rq_fn方法准备好的预先内置命令所在的缓冲区。
	 */
	unsigned char dev_cmd_buf[BLK_MAX_CDB];
	/**
	 * data缓冲区的长度。
	 */
	unsigned int data_len;
	/**
	 * 驱动程序为了跟踪所传送的数据而使用的指针。
	 */
	void *data;
	/**
	 * sense指向的缓冲区长度。
	 */
	unsigned int sense_len;
	/**
	 * sense命令的缓冲区指针。
	 */
	void *sense;
};

#if BITS_PER_LONG == 32
#define BLKDEV_LIMIT_HIGH		((u64)max_dma_pgnum << PAGE_SHIFT)
#else
#define BLKDEV_LIMIT_HIGH		-1ULL
#endif
#define BLKDEV_LIMIT_ANY		((u64)max_pgnum << PAGE_SHIFT)

/**
 * 磁盘请求队列
 */
struct blk_request_queue
{
	struct object object;
	/**
	 * 请求队列锁指针。
	 * 可能是块设备的锁，也可能是队列默认的锁
	 */
	struct smp_lock		*queue_lock;
	/**
	 * 如果块设备没有指定队列的锁，则使用此默认锁
	 */
	struct smp_lock		default_lock;
	/**
	 * 请求队列状态标志。
	 */
	unsigned long		state;
	/**
	 * 大于该页框号时，将使用回弹缓冲区。
	 */
	unsigned long		max_pgnum;
	/**
	 * 内存分配标志。
	 */
	int			alloc_flags;
	/**
	 * 设备缓存能力
	 */
	unsigned long flush_flags;
	/**
	 * 请求队列中允许的最大请求数。
	 * 分别控制读写数量
	 */
	unsigned long		max_requests;
	/**
	 * 队列中首先可能合并的请求描述符
	 * 上一次与此请求进行合并，一般是最后一个请求
	 */
	struct blk_request		*preferred_merge;
	/**
	 * 所有请求的链表头
	 */
	struct double_list	requests;
	/**
	 * 调度器对象，IO调度算法使用
	 */
	struct ioscheduler		*scheduler;
	/**
	 * 已经交给驱动，但是还没有完成的读写请求数量
	 */
	unsigned int		running_count;
	/**
	 * 临时延时的请求链表头
	 * 这些延迟的请求被新的调度器处理
	 */
	struct double_list	delayed_list;
	/**
	 * 如果积压的请求超过此数，表明队列忙
	 * 应当暂缓接收请求
	 */
	int			busy_thresh;
	/**
	 * 块设备驱动处理请求队列的回调
	 * 在此回调中遍历所有请求并发送给设备
	 */
	blkdev_engorge_queue_f		engorge_queue;
	/*
	 * 请求计数，避免分配过多的请求
	 */
	struct {
		unsigned long request_count;
		struct wait_queue wait;
	} request_pools[2];
	/**
	 * 等待队列，等待请求队列被清空的进程。
	 */
	struct wait_queue wait_drain;
	/**
	 * 检查是否可以将bio合并到请求后面。
	 */
	merge_bio_f	merge_bio_tail;
	/**
	 * 检查是否可以将bio合并到请求前面。
	 */
	merge_bio_f	merge_bio_head;
	/**
	 * 合并请求队列中两个相邻请求的方法
	 * 返回0表示如果不能合并
	 */
	int (*merge_requests) (struct blk_request_queue *, struct blk_request *,
				 struct blk_request *);
	/**
	 * 将新请求插入请求队列的方法
	 * 默认是blk_generic_sumit_request
	 * 当不使用默认方法进行BIO传输时，可以提供一个替代函数
	 * 以支持"无队列"模式的操作。
 	*/
	int (*sumit_request)(struct blk_request_queue *queue, struct block_io_desc *bio);
	/**
	 * 与单个请求相关设置值
	 */
	struct {
		/**
		 * 内存边界，跨越此边界的两个请求不能合并
		 */
		unsigned long		addr_boundary;
		/**
		 * 单个请求的最大长度
		 */
		unsigned int		max_size;
		/**
		 * 单个请求所能处理的最大段数。
		 */
		unsigned short		max_hw_segment_count;
		/**
		 * 单个请求能处理的最大扇区数。可调整的。
		 */
		unsigned short		max_sectors;
		/**
		 * 单个请求能处理的最大扇区数。强制约束。
		 */	
		unsigned short		max_hw_sectors;
		/**
		 * 扇区中以字节为单位的大小。
		 */
		unsigned short		sector_size;
		/**
		 * DMA缓冲区的起始地址和长度的对齐位图，默认是511.
		 */
		unsigned int		dma_alignment;
	} request_settings;
	/**
	 * 将请求推向块设备的方法
	 */
	void (*push_queue)(struct blk_request_queue *);
	/**
	 * 延迟向设备推送请求时间。默认是3ms
	 */
	unsigned long		push_delay;
	/** 
	 * 定时器，到期后(默认3ms)强制向设备发送请求。
	 */
	struct timer	push_timer;
	/**
	 *  延迟向设备推送请求的工作队列
	 */
	struct work_struct	push_work;
	/**
	 * 抽象给文件系统层的描述符
	 */
	struct blkdev_infrast	blkdev_infrast;
	/**
	 * 块设备驱动程序的私有数据。
	 */
	void			*queuedata;
};

static inline unsigned long bd_get_sectors(struct block_device *bd)
{
	return bd->fnode->file_size >> 9;
}

extern struct block_device *__find_hold_block_device(devno_t);
static inline struct block_device *
find_hold_block_device(unsigned int dev_major, unsigned int dev_minor)
{
	return __find_hold_block_device(MKDEV(dev_major, dev_minor));
}
	
extern char *format_block_devname(struct block_device *bdev, char *buffer);

unsigned char *blkdev_read_sector(struct block_device *, sector_t, struct page_frame **);

static inline sector_t blk_rq_pos(const struct blk_request *rq)
{
	return rq->start_sector;
}

extern void blkdev_finish_request(struct blk_request *rq, int error);

extern int blk_gather_bio(struct blk_request_queue *, struct blk_request *, struct scatterlist *);

extern void blkdev_start_quest(struct blk_request_queue *queue);
void blkdev_stop_request(struct blk_request_queue *queue);

extern int scsi_cmd_blk_ioctl(struct block_device *, fmode_t,
			      unsigned int, void __user *);
extern void blkdev_set_flush_flags(struct blk_request_queue *queue, unsigned int flush);

extern struct blk_request_queue *blk_create_queue(blkdev_engorge_queue_f, struct smp_lock *);

extern void blkdev_set_hw_max_pages(struct blk_request_queue *, unsigned short);
extern void blk_set_dma_limit(struct blk_request_queue *, u64);
extern void blk_set_request_boundary(struct blk_request_queue *, unsigned long);
extern void blk_set_request_max_size(struct blk_request_queue *, unsigned int);
extern void blk_set_request_max_hw_pages(struct blk_request_queue *, unsigned short);
extern void blk_loosen_queue(struct blk_request_queue *);
/**
 * 从request中得到传输的方向。返回0表示从设备读数据，非0表示向设备写数据。
 */
static inline bool blk_request_is_write(struct blk_request *request)
{
	return !!(request->flags & BLKQUEUE_WRITE);
}

static inline unsigned int block_size(struct block_device *bdev)
{
	return bdev->bd_block_size;
}

/**
 * 获得块设备的请求队列
 */
static inline struct blk_request_queue *blkdev_get_queue(struct block_device *bdev)
{
	return bdev->disk->queue;
}

static inline int queue_hardsect_size(struct blk_request_queue *queue)
{
	int retval = 512;

	if (queue && queue->request_settings.sector_size)
		retval = queue->request_settings.sector_size;

	return retval;
}

static inline int blkdev_hardsect_size(struct block_device *bdev)
{
	return queue_hardsect_size(blkdev_get_queue(bdev));
}

static inline unsigned int blksize_bits(unsigned int size)
{
	unsigned int bits = 8;

	do {
		bits++;
		size >>= 1;
	} while (size > 256);

	return bits;
}

#define TO_BLK_REQUEST(ptr)	list_container((ptr), struct blk_request, list)
#define BLK_FIRST_REQUEST(ptr)	\
	list_first_container((ptr), struct blk_request, list)
extern struct blkdev_infrast *blk_get_infrastructure(struct block_device *bdev);

#define end_io_error(uptodate)	(unlikely((uptodate) <= 0))

extern int blkio_hw_segments(struct blk_request_queue *, struct block_io_desc *);
/**
 * 遍历request中所有请求的bio。
 */
#define rq_for_each_bio(_bio, rq)	\
	if ((rq->bio_head))			\
		for (_bio = (rq)->bio_head; _bio; _bio = _bio->bi_next)

static inline void blkdev_dequeue_request(struct blk_request *req)
{
	iosched_remove_request(req->queue, req);
}
extern int blk_update_request(struct blk_request *, int, int);
extern void blk_finish_request(struct blk_request *);
extern void blk_end_request(struct blk_request *req, int uptodate);

extern void blk_sync_queue(struct blk_request_queue *queue);
extern void blk_attach_device(struct blk_request_queue *);

#define RQ_NOMERGE_FLAGS	\
	(BLKREQ_NOMERGE | BLKREQ_STARTED | BLKREQ_BARRIER)
#define rq_mergeable(rq)	\
	(!((rq)->flags & RQ_NOMERGE_FLAGS) && (rq->flags & BLKREQ_DATA))

int putin_blkdev_container(struct device *device, void *owner);
void takeout_blkdev_container(struct device *device);
extern int blk_disable_attach(struct blk_request_queue *);
extern void __blk_generic_push_queue(struct blk_request_queue *);
extern void blk_generic_push_queue(struct blk_request_queue *);
extern void blk_set_request_max_sectors(struct blk_request_queue *, unsigned long);
extern void blk_set_request_sector_size(struct blk_request_queue *, unsigned short);
int kblockd_schedule_work(struct work_struct *work);
extern void devfs_add_disk(struct disk_device *dev);
void devfs_remove_disk(struct disk_device *disk);
void blk_hold_queue(struct blk_request_queue *queue);
void blk_loosen_queue(struct blk_request_queue *queue);
#endif /* __DIM_SUM_BLOCK_DEVICE_H */
