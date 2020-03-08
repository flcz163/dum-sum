#ifndef __DIM_SUM_BLOCK_BUF_H
#define __DIM_SUM_BLOCK_BUF_H

#include <dim-sum/fs.h>
#include <dim-sum/mm_types.h>
#include <dim-sum/swap.h>

struct file_node;
struct writeback_control;
struct blkbuf_desc;

#define MAX_BUF_PER_PAGE (PAGE_CACHE_SIZE / 512)

typedef void (*blkbuf_end_io_func)(struct blkbuf_desc *blkbuf, int uptodate);

/**
 * 块缓冲区状态
 */
enum blkbuf_state {
	/**
	 * 如果在块缓冲区包含有效数据时就置位
	 */
	BS_UPTODATE,
	/**
	 * 如果缓冲区脏就置位
	 * 表示其数据必须被写回磁盘
	 */
	BS_DIRTY,
	/**
	 * 如果缓冲区被加锁就置位
	 * 通常发生在缓冲区进行磁盘传输时。
	 */
	BS_LOCKED,
	/**
	 * 如果已经为缓冲区请求数据传输就置位
	 * 表示相应的IO请求已经发送
	 */
	BS_REQUESTED,
	/**
	 * 如果缓冲区被映射到磁盘就置位。
	 */
	BS_MAPPED,
	/**
	 * 如果相应的块刚被分配而还没有被访问过就置位
	 */
	BS_NEW,
	/**
	 * 如果在异步的读缓冲区就置位
	 */
	BS_ASYNC_READ,
	/**
	 * 如果在异步的写缓冲区就置位
	 */
	BS_ASYNC_WRITE,	
	/**
	 * 如果还没有在磁盘上分配缓冲区就置位
	 */
	BS_DELAY,
	/**
	 * 磁盘块后面跟随着一个不连续的块
	 * 例如在某一级间接块的边界处
	 */
	BS_BOUNDARY,
	/**
	 * 如果写磁盘块时出现IO错误就置位
	 */
	BS_WRITE_EIO,
	/**
	 * 不能乱序提交
	 * 主要用于日志文件
	 */
	BS_ORDERED,
	/**
	 * 如果块设备的驱动程序不支持所请求的操作就置位
	 * 如IO屏障请求可能就不被支持
	 */
	BS_EOPNOTSUP,
	/**
	 * 各模块可用的私有标志
	 */
	BS_PRIVATESTART,
};

/**
 * 磁盘块缓冲区描述符
 * 对应一个磁盘块的内容
 */
struct blkbuf_desc {
	/**
	 * 缓冲区状态标志,如BS_UPTODATE
	 */
	unsigned long state;
	/**
	 * 块大小
	 */
	u32 size;
	/**
	 * 指向缓冲区页中的下一个磁盘块的指针
	 */
	struct blkbuf_desc *next_in_page;
	/**
	 * 该块所在的缓冲页
	 */
	struct page_frame *page;
	union {
		/**
		 * 数据块地址
		 */
		char *block_data;
		/**
		 * 在页面中的偏移
		 */
		unsigned long offset;
	};
	/**
	 * 引用计数
	 */
	struct accurate_counter ref_count;
	/**
	 * 指向块设备描述符的指针,通常是磁盘或者分区。
	 */
	struct block_device *blkdev;
	/**
	 * 与块设备相关的块号
	 */
	sector_t block_num_dev;
	/**
	 * IO完成回调
	 */
	blkbuf_end_io_func finish_io;
	/**
	 * 指向IO完成方法数据的指针
	 * 对于日志来说，是管理该缓冲区的日志描述符
	 */
 	void *private;
};

static inline int blkbuf_is_uptodate(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_UPTODATE, &blkbuf->state);
}
static inline void blkbuf_set_uptodate(struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_UPTODATE, &blkbuf->state);
}
static inline void blkbuf_clear_uptodate(struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_UPTODATE, &blkbuf->state);
}

static inline int blkbuf_is_dirty(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_DIRTY, &blkbuf->state);
}
static inline void blkbuf_set_dirty(struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_DIRTY, &blkbuf->state);
}
static inline void blkbuf_clear_dirty(struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_DIRTY, &blkbuf->state);
}
static inline int blkbuf_test_set_dirty(struct blkbuf_desc *blkbuf)
{
	return atomic_test_and_set_bit(BS_DIRTY, &blkbuf->state);
}
static inline int blkbuf_test_clear_dirty(struct blkbuf_desc *blkbuf)
{
	return atomic_test_and_clear_bit(BS_DIRTY, &blkbuf->state);
}

static inline int blkbuf_is_locked(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_LOCKED, &blkbuf->state);
}
static inline void blkbuf_set_locked(struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_LOCKED, &blkbuf->state);
}
static inline void blkbuf_clear_locked(struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_LOCKED, &blkbuf->state);
}
static inline int blkbuf_try_lock(struct blkbuf_desc *blkbuf)
{
	return atomic_test_and_set_bit(BS_LOCKED, &blkbuf->state);
}
static inline int blkbuf_test_clear_locked(struct blkbuf_desc *blkbuf)
{
	return atomic_test_and_clear_bit(BS_LOCKED, &blkbuf->state);
}

static inline int blkbuf_is_requested(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_REQUESTED, &blkbuf->state);
}
static inline void blkbuf_set_requested(struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_REQUESTED, &blkbuf->state);
}
static inline void blkbuf_clear_requested(struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_REQUESTED, &blkbuf->state);
}
static inline int blkbuf_test_set_requested(struct blkbuf_desc *blkbuf)
{
	return atomic_test_and_set_bit(BS_REQUESTED, &blkbuf->state);
}
static inline int blkbuf_test_clear_requested(struct blkbuf_desc *blkbuf)
{
	return atomic_test_and_clear_bit(BS_REQUESTED, &blkbuf->state);
}

static inline int blkbuf_is_mapped(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_MAPPED, &blkbuf->state);
}
static inline void blkbuf_set_mapped(struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_MAPPED, &blkbuf->state);
}
static inline void blkbuf_clear_mapped(struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_MAPPED, &blkbuf->state);
}

static inline int blkbuf_is_new(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_NEW, &blkbuf->state);
}
static inline void blkbuf_set_new(struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_NEW, &blkbuf->state);
}
static inline void blkbuf_clear_new(struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_NEW, &blkbuf->state);
}

static inline int blkbuf_is_async_read(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_ASYNC_READ, &blkbuf->state);
}
static inline void blkbuf_set_async_read(struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_ASYNC_READ, &blkbuf->state);
}
static inline void blkbuf_clear_async_read(struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_ASYNC_READ, &blkbuf->state);
}

static inline int blkbuf_is_async_write(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_ASYNC_WRITE, &blkbuf->state);
}
static inline void blkbuf_set_async_write (struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_ASYNC_WRITE , &blkbuf->state);
}
static inline void blkbuf_clear_async_write(struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_ASYNC_WRITE, &blkbuf->state);
}

static inline int blkbuf_is_delay(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_DELAY, &blkbuf->state);
}
static inline void blkbuf_set_delay(struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_DELAY , &blkbuf->state);
}
static inline void blkbuf_clear_delay (struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_DELAY, &blkbuf->state);
}

static inline int blkbuf_is_boundary(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_BOUNDARY, &blkbuf->state);
}
static inline void blkbuf_set_boundary (struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_BOUNDARY, &blkbuf->state);
}
static inline void blkbuf_clear_boundary(struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_BOUNDARY, &blkbuf->state);
}

static inline int blkbuf_is_write_io_error(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_WRITE_EIO, &blkbuf->state);
}
static inline void blkbuf_set_write_io_error(struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_WRITE_EIO , &blkbuf->state);
}
static inline void blkbuf_clear_write_io_error(struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_WRITE_EIO, &blkbuf->state);
}

static inline int blkbuf_is_ordered(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_ORDERED, &blkbuf->state);
}
static inline void blkbuf_set_ordered(struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_ORDERED, &blkbuf->state);
}
static inline void blkbuf_clear_ordered(struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_ORDERED, &blkbuf->state);
}

static inline int blkbuf_is_eopnotsupp(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_EOPNOTSUP, &blkbuf->state);
}
static inline void blkbuf_set_eopnotsupp(struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_EOPNOTSUP, &blkbuf->state);
}
static inline void blkbuf_clear_eopnotsupp(struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_EOPNOTSUP, &blkbuf->state);
}

struct blkbuf_desc *alloc_blkbuf_desc(int paf_flags);
void free_blkbuf_desc(struct blkbuf_desc * blkbuf);
int blkbuf_sync_page(struct page_frame *);
int blkbuf_prepare_write(struct page_frame*, unsigned, unsigned, map_block_f*);
int blkbuf_commit_write(struct file_node *file_node,
	struct page_frame *page, unsigned from, unsigned to);
void blkbuf_sync_metablock(struct block_device *bdev, sector_t block);
void blkbuf_write_boundary(struct block_device *bdev,
	sector_t bblock, unsigned blocksize);
sector_t get_mapped_block(struct file_cache_space *, sector_t, map_block_f *);
int fsync_filesystem(struct super_block *);
int submit_block_request(int, struct blkbuf_desc *);
void blkbuf_forget(struct blkbuf_desc *);
struct blkbuf_desc * __blkbuf_find_alloc(struct block_device *, sector_t, int);
struct blkbuf_desc *__blkbuf_read_block(struct block_device *,
	sector_t block, int size);
/**
 * 为文件系统读取一个逻辑块
 */
static inline struct blkbuf_desc *
blkbuf_read_block(struct super_block *super, sector_t block)
{
	return __blkbuf_read_block(super->blkdev, block, super->block_size);
}

static inline void hold_blkbuf(struct blkbuf_desc *blkbuf)
{
        accurate_inc(&blkbuf->ref_count);
}

static inline void loosen_blkbuf(struct blkbuf_desc *blkbuf)
{
	if (!blkbuf)
		return;

        smp_mb__before_atomic_dec();
        accurate_dec(&blkbuf->ref_count);
}

int blkdev_sync(struct block_device *bdev);
void blkdev_invalidate_pgcache(struct block_device *);
#define page_first_block(page)					\
	({							\
		BUG_ON(!pgflag_private(page));		\
		((struct blkbuf_desc *)(page)->private);	\
	})
#define page_has_blocks(page)	pgflag_private(page)
#define blkbuf_offset(blkbuf)		\
	((unsigned long)blkbuf->block_data & ~PAGE_MASK)

int fsync_blkdev(struct block_device *);
int submit_read_page_blocks(struct page_frame*, map_block_f*);
int blkbuf_write_page(struct page_frame *page, map_block_f *map_block,
				struct writeback_control *wbc);
int submit_block_request(int, struct blkbuf_desc *);
int fsync_filesystem(struct super_block *);
void blkbuf_unlock(struct blkbuf_desc *buf_desc);
void blkbuf_lock(struct blkbuf_desc *buf_desc);
void blkbuf_wait_unlock(struct blkbuf_desc *blkbuf);
int sync_dirty_block(struct blkbuf_desc *blkbuf);
void blkbuf_mark_dirty(struct blkbuf_desc *blkbuf);

static inline void
blkbuf_set_map_data(struct blkbuf_desc *blkbuf,
	struct super_block *super, sector_t block)
{
	blkbuf_set_mapped(blkbuf);
	blkbuf->blkdev = super->blkdev;
	blkbuf->block_num_dev = block;
}

static inline struct blkbuf_desc *
blkbuf_find_alloc(struct super_block *super, sector_t block)
{
	return __blkbuf_find_alloc(super->blkdev, block, super->block_size);
}

void blkbuf_finish_read_block(struct blkbuf_desc *blkbuf, int uptodate);

struct blkbuf_desc *__blkbuf_find_block(struct block_device *, sector_t, int);

static inline struct blkbuf_desc *
blkbuf_find_block(struct super_block *super, sector_t block)
{
	return __blkbuf_find_block(super->blkdev, block, super->block_size);
}

void blkbuf_finish_write_block(struct blkbuf_desc *blkbuf, int uptodate);
void blkbuf_set_page(struct blkbuf_desc *blkbuf,
		struct page_frame *page, unsigned long offset);
void blkbuf_create_desc_page(struct page_frame *page,
			unsigned long blocksize, unsigned long state);
int try_to_release_page(struct page_frame * page, int paf_mask);
int try_to_free_blocks(struct page_frame *);
int fsync_blkdev(struct block_device *);
int generic_commit_write(struct file *, struct page_frame *, unsigned, unsigned);
void blkbuf_init(struct blkbuf_desc *blkbuf, blkbuf_end_io_func handler, void *private);

#endif /* __DIM_SUM_BLOCK_BUF_H */
