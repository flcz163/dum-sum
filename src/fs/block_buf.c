#include <dim-sum/boot_allotter.h>
#include <dim-sum/beehive.h>
#include <dim-sum/blkio.h>
#include <dim-sum/blk_infrast.h>
#include <dim-sum/block_buf.h>
#include <dim-sum/delay.h>
#include <dim-sum/errno.h>
#include <dim-sum/fs.h>
#include <dim-sum/highmem.h>
#include <dim-sum/pagevec.h>
#include <dim-sum/sched.h>
#include <dim-sum/stacktrace.h>
#include <dim-sum/syscall.h>
#include <dim-sum/writeback.h>

#include <asm/cacheflush.h>

static struct smp_lock page_block_lock = 
	SMP_LOCK_UNLOCKED(page_block_lock);

static struct beehive_allotter *blkbuf_desc_allotter;

static inline int blkbuf_is_busy(struct blkbuf_desc *buf_desc)
{
	return accurate_read(&buf_desc->ref_count) |
		(buf_desc->state & ((1 << BS_DIRTY) | (1 << BS_LOCKED)));
}

void blkbuf_init(struct blkbuf_desc *buf_desc, blkbuf_end_io_func handler,
	void *private)
{
	buf_desc->finish_io = handler;
	buf_desc->private = private;
}

void blkbuf_lock(struct blkbuf_desc *buf_desc)
{
	might_sleep();
	if (blkbuf_try_lock(buf_desc))
		wait_on_bit_lock_io(&buf_desc->state, BS_LOCKED, TASK_UNINTERRUPTIBLE);
}

void fastcall blkbuf_unlock(struct blkbuf_desc *buf_desc)
{
	blkbuf_clear_locked(buf_desc);
	smp_mb__after_clear_bit();
	wake_up_bit(&buf_desc->state, BS_LOCKED);
}

static int
drop_block_buffers(struct page_frame *page, struct blkbuf_desc **buffers_to_free)
{
	struct blkbuf_desc *head = page_first_block(page);
	struct blkbuf_desc *buf_desc;

	buf_desc = head;
	do {
		if (blkbuf_is_write_io_error(buf_desc))
			atomic_set_bit(__CS_EIO, &page->cache_space->flags);
		if (blkbuf_is_busy(buf_desc))
			goto failed;
		buf_desc = buf_desc->next_in_page;
	} while (buf_desc != head);

	*buffers_to_free = head;

	ClearPagePrivate(page);
	page->private = 0;
	loosen_page_cache(page);

	return 1;
failed:
	return 0;
}

/**
 * 释放缓存页中的块
 */
int try_to_free_blocks(struct page_frame *page)
{
	struct file_cache_space * const space = page->cache_space;
	struct blkbuf_desc *buffers_to_free = NULL;
	int ret = 0;

	ASSERT(pgflag_locked(page));
	/**
	 * 正在回写，不能释放
	 */
	if (pgflag_writeback(page))
		return 0;

	if (space == NULL)
		ret = drop_block_buffers(page, &buffers_to_free);

	if (buffers_to_free) {
		struct blkbuf_desc *buf_desc = buffers_to_free;

		/**
		 * 反复调用free_buffer_head，以释放页的所有缓冲区首部。
		 */
		do {
			struct blkbuf_desc *next = buf_desc->next_in_page;

			free_blkbuf_desc(buf_desc);
			buf_desc = next;
		} while (buf_desc != buffers_to_free);
	}

	return ret;
}

static inline void discard_buffer(struct blkbuf_desc * buf_desc)
{
	blkbuf_lock(buf_desc);
	blkbuf_clear_dirty(buf_desc);
	buf_desc->blkdev = NULL;
	blkbuf_clear_mapped(buf_desc);
	blkbuf_clear_requested(buf_desc);
	blkbuf_clear_new(buf_desc);
	blkbuf_clear_delay(buf_desc);
	blkbuf_unlock(buf_desc);
}

/**
 * 释放缓冲区页。
 * page:	要释放的页描述符地址。
 */
int try_to_release_page(struct page_frame *page, int paf_mask)
{
	struct file_cache_space * const space = page->cache_space;

	ASSERT(pgflag_locked(page));
	/**
	 * 正在试图将页写回磁盘，因此不能将页释放。
	 */ 
	if (pgflag_writeback(page))
		return 0;

	if (space && space->ops->releasepage)
		return space->ops->releasepage(page, paf_mask);

	return try_to_free_blocks(page);
}

/**
 * 当截断文件时，使用相应的页面失效
 * 页面可以保留在页面缓存中，以备重用
 */
static int blkbuf_invalidate_page(struct page_frame *page, unsigned long offset)
{
	struct blkbuf_desc *head, *buf_desc;
	unsigned int curr_off = 0;
	int ret = 1;

	ASSERT(pgflag_locked(page));
	if (!page_has_blocks(page))
		goto out;

	head = page_first_block(page);
	buf_desc = head;
	do {
		if (offset <= curr_off)
			discard_buffer(buf_desc);
		curr_off += buf_desc->size;
		buf_desc = buf_desc->next_in_page;
	} while (buf_desc != head);

	if (offset == 0)
		ret = try_to_release_page(page, 0);
out:
	return ret;
}

int __set_page_dirty_buffers(struct page_frame *page)
{
	struct file_cache_space * const space = page->cache_space;

	smp_lock(&space->block_lock);
	if (page_has_blocks(page)) {
		struct blkbuf_desc *head = page_first_block(page);
		struct blkbuf_desc *buf_desc = head;

		do {
			blkbuf_set_dirty(buf_desc);
			buf_desc = buf_desc->next_in_page;
		} while (buf_desc != head);
	}
	smp_unlock(&space->block_lock);

	if (!pgflag_dirty(page)) {
		smp_lock_irq(&space->tree_lock);
		if (page->cache_space) {
			radix_tree_tag_set(&space->page_tree,
						page_index(page),
						PAGECACHE_TAG_DIRTY);
		}
		smp_unlock_irq(&space->tree_lock);
		__mark_filenode_dirty(space->fnode, FNODE_DIRTY_PAGES);
	}
	
	return 0;
}

/**
 * 在获得锁的情况下，等待IO完成解锁
 * 实际上是等待IO操作完成
 */
void blkbuf_wait_unlock(struct blkbuf_desc *buf_desc)
{
	might_sleep();

	ASSERT(accurate_read(&buf_desc->ref_count) > 0);

	wait_on_bit(&buf_desc->state, BS_LOCKED, TASK_UNINTERRUPTIBLE);
}

void fastcall blkbuf_mark_dirty(struct blkbuf_desc *buf_desc)
{
	if (!blkbuf_test_set_dirty(buf_desc))
		__set_page_dirty_nobuffers(buf_desc->page);
}

struct blkbuf_desc *alloc_blkbuf_desc(int paf_flags)
{
	struct blkbuf_desc *buf_desc;

	buf_desc = beehive_alloc(blkbuf_desc_allotter, paf_flags | __PAF_ZERO);

	return buf_desc;
}

/**
 * 释放缓冲区首部。
 */
void free_blkbuf_desc(struct blkbuf_desc *buf_desc)
{
	beehive_free(blkbuf_desc_allotter, buf_desc);
}

void blkbuf_set_page(struct blkbuf_desc *buf_desc,
		struct page_frame *page, unsigned long offset)
{
	buf_desc->page = page;
	if (offset >= PAGE_SIZE)
		BUG();

	buf_desc->block_data = page_address(page) + offset;
}

/**
 * 为缓存页面分配块缓冲区描述符
 */
static struct blkbuf_desc *
alloc_page_buffers(struct page_frame *page, unsigned long size, int retry)
{
	struct blkbuf_desc *buf_desc, *head;
	long offset;

try_again:
	head = NULL;
	offset = PAGE_SIZE;
	while ((offset -= size) >= 0) {
		buf_desc = alloc_blkbuf_desc(PAF_NOFS);
		if (!buf_desc)
			goto no_grow;

		buf_desc->blkdev = NULL;
		buf_desc->next_in_page = head;
		buf_desc->block_num_dev = -1;
		head = buf_desc;

		buf_desc->state = 0;
		accurate_set(&buf_desc->ref_count, 0);
		buf_desc->size = size;

		blkbuf_set_page(buf_desc, page, offset);

		buf_desc->finish_io = NULL;
	}
	return head;

no_grow:
	if (head) {
		do {
			buf_desc = head;
			head = head->next_in_page;
			free_blkbuf_desc(buf_desc);
		} while (head);
	}

	/**
	 * 对于回写过程来说，分配失败时可以忽略
	 */
	if (!retry)
		return NULL;

	msleep(1);
	goto try_again;
}

/**
 * 当truncate文件时，被截断的块需要作废
 * 本函数清除其dirty标志并释放块
 */
void blkbuf_forget(struct blkbuf_desc *buf_desc)
{
	blkbuf_clear_dirty(buf_desc);

	loosen_blkbuf(buf_desc);
}

static inline void
attach_page_blocks(struct page_frame *page, struct blkbuf_desc *head)
{
	struct blkbuf_desc *buf_desc, *tail;

	buf_desc = head;
	do {
		tail = buf_desc;
		buf_desc = buf_desc->next_in_page;
	} while (buf_desc);
	tail->next_in_page = head;

	page_cache_hold(page);
	SetPagePrivate(page);
	page->private = (unsigned long)head;
}

/**
 * 为页所含块分配描述符
 */
void blkbuf_create_desc_page(struct page_frame *page,
			unsigned long block_size, unsigned long state)
{
	struct blkbuf_desc *buf_desc, *head;

	head = alloc_page_buffers(page, block_size, 1);

	smp_lock(&page->cache_space->block_lock);
	attach_page_blocks(page, head);
	
	if (pgflag_uptodate(page) || pgflag_dirty(page)) {
		buf_desc = head;
		do {
			if (pgflag_dirty(page))
				blkbuf_set_dirty(buf_desc);
			if (pgflag_uptodate(page))
				blkbuf_set_uptodate(buf_desc);
			buf_desc = buf_desc->next_in_page;
		} while (buf_desc != head);
	}
	smp_unlock(&page->cache_space->block_lock);
}

/**
 * 初始化页面中的块描述符
 */ 
static void
init_page_blocks(struct page_frame *page, struct block_device *bdev,
			sector_t block_num, int size)
{
	struct blkbuf_desc *head = page_first_block(page);
	int uptodate = pgflag_uptodate(page);
	struct blkbuf_desc *buf_desc = head;

	/**
	 * 对块设备来说，没有空洞
	 * 将所有块都设置为*映射*状态
	 */
	do {
		if (!blkbuf_is_mapped(buf_desc)) {
			blkbuf_init(buf_desc, NULL, NULL);
			buf_desc->blkdev = bdev;
			buf_desc->block_num_dev = block_num;
			if (uptodate)
				blkbuf_set_uptodate(buf_desc);
			blkbuf_set_mapped(buf_desc);
		}
		block_num++;
		buf_desc = buf_desc->next_in_page;
	} while (buf_desc != head);
}

/**
 * 读缓存页的回调
 * 当块设备完成IO，并且块属于缓存页时回调此函数
 */
static void blkbuf_finish_read_page(struct blkbuf_desc *buf_desc, int uptodate)
{
	struct page_frame *page;
	struct blkbuf_desc *tmp;
	int page_uptodate = 1;
	unsigned long flags;

	/**
	 * 确认是读页面缓存
	 */
	ASSERT(blkbuf_is_async_read(buf_desc));

	page = buf_desc->page;
	if (uptodate)
		blkbuf_set_uptodate(buf_desc);
	else {
		blkbuf_clear_uptodate(buf_desc);
		set_page_error(page);
	}

	smp_lock_irqsave(&page_block_lock, flags);
	blkbuf_clear_async_read(buf_desc);
	blkbuf_unlock(buf_desc);
	tmp = buf_desc;
	/**
	 * 循环检查页面中所有块是否都已经读完
	 */
	do {
		if (!blkbuf_is_uptodate(tmp))
			page_uptodate = 0;
		/**
		 * 还有块没有完成
		 */
		if (blkbuf_is_async_read(tmp)) {
			ASSERT(blkbuf_is_locked(tmp));
			goto partial;/* 整个页面还没有完成，退出，待所有缓冲区首部都完成后再继续 */
		}
		tmp = tmp->next_in_page;
	} while (tmp != buf_desc);
	smp_unlock_irqrestore(&page_block_lock, flags);

	/*
	 * 页面中所有块都全部读取并且没有错误
	 */
	if (page_uptodate && !pgflag_error(page))
		set_page_uptodate(page);

	unlock_page(page);
	return;

partial:
	smp_unlock_irqrestore(&page_block_lock, flags);
	return;
}

/**
 * 块IO读完成回调
 */
void blkbuf_finish_read_block(struct blkbuf_desc *buf_desc, int uptodate)
{
	if (uptodate)
		blkbuf_set_uptodate(buf_desc);
	else
		blkbuf_clear_uptodate(buf_desc);

	blkbuf_unlock(buf_desc);
	loosen_blkbuf(buf_desc);
}

/**
 * 块缓冲区被成功写入磁盘后，回调此函数
 * 调用者不需要同步处理
 * 适用于缓存页的写
 */
static void blkbuf_finish_write_page(struct blkbuf_desc *buf_desc, int uptodate)
{
	struct page_frame *page;
	struct blkbuf_desc *tmp;
	unsigned long flags;

	ASSERT(blkbuf_is_async_write(buf_desc));

	page = buf_desc->page;
	if (uptodate)
		blkbuf_set_uptodate(buf_desc);
	else {
		atomic_set_bit(__CS_EIO, &page->cache_space->flags);
		blkbuf_clear_uptodate(buf_desc);
		set_page_error(page);
	}

	smp_lock_irqsave(&page_block_lock, flags);
	/**
	 * 清除异步写标志，表示写已经完成
	 */
	blkbuf_clear_async_write(buf_desc);
	blkbuf_unlock(buf_desc);
	tmp = buf_desc->next_in_page;
	/**
	 * 遍历缓冲区，检查是否所有块都已经写完
	 */
	while (tmp != buf_desc) {
		/**
		 * 还有块在传输中，页面未完全完成
		 */
		if (blkbuf_is_async_write(tmp)) {
			ASSERT(blkbuf_is_locked(tmp));
			goto partial;
		}

		tmp = tmp->next_in_page;
	}
	smp_unlock_irqrestore(&page_block_lock, flags);
	/**
	 * 页面已经全部回写
	 * 唤醒写等待者
	 */
	page_finish_writeback(page);

	return;

partial:
	smp_unlock_irqrestore(&page_block_lock, flags);
	return;
}

/**
 * 硬件完成IO写操作后回调此函数通知文件系统层
 * 适用于单个元数据块的写
 */
void blkbuf_finish_write_block(struct blkbuf_desc *buf_desc, int uptodate)
{
	if (uptodate)
		blkbuf_set_uptodate(buf_desc);
	else {
		blkbuf_set_write_io_error(buf_desc);
		blkbuf_clear_uptodate(buf_desc);
	}

	blkbuf_unlock(buf_desc);
	loosen_blkbuf(buf_desc);
}

/**
 * 块设备完成io操作时，回调此函数通知文件系统层
 */
static int blkbuf_finish(struct block_io_desc *bio, unsigned int bytes_done, int err)
{
	/**
	 * 从bi_private中获得缓冲区首部的地址
	 */
	struct blkbuf_desc *buf_desc = bio->bi_private;

	/**
	 * 块请求还没有完全结束
	 */
	if (bio->remain_size)
		return 1;

	if (err == -EOPNOTSUPP) {
		atomic_set_bit(__BIOFLAG_EOPNOTSUPP, &bio->bi_flags);
		atomic_set_bit(BS_EOPNOTSUP, &buf_desc->state);
	}

	/**
	 * 根据块的类型而调用不同的回调
	 * 如blkbuf_finish_write_block、blkbuf_finish_write_page
	 */
	buf_desc->finish_io(buf_desc, bio->bi_flags & BIOFLAG_UPTODATE);
	loosen_blkio(bio);

	return 0;
}

/**
 * 向内核通用块层请求传输一个数据块。
 * rw:		数据传输方向(读或者写)
 * buf_desc:		要传送数据的块缓冲区描述符。
 */
int submit_block_request(int rw, struct blkbuf_desc *buf_desc)
{
	struct block_io_desc *bio;
	int ret = 0;

	ASSERT(blkbuf_is_locked(buf_desc));
	ASSERT(blkbuf_is_mapped(buf_desc));
	ASSERT(buf_desc->finish_io);

	if (blkbuf_is_ordered(buf_desc) && (rw == WRITE))
		rw = WRITE_BARRIER;

	if (blkbuf_test_set_requested(buf_desc) && (rw == WRITE || rw == WRITE_BARRIER))
		blkbuf_clear_write_io_error(buf_desc);

	/**
	 * 分配一个新的IO请求描述符。
	 */
	bio = blkio_alloc(PAF_NOIO, 1);

	/**
	 * 初始化IO请求描述符
	 */
	bio->start_sector = buf_desc->block_num_dev * (buf_desc->size >> 9);
	bio->bi_bdev = buf_desc->blkdev;
	bio->items[0].bv_page = buf_desc->page;
	bio->items[0].length = buf_desc->size;
	bio->items[0].bv_offset = blkbuf_offset(buf_desc);

	bio->item_count = 1;
	bio->bi_idx = 0;
	bio->remain_size = buf_desc->size;

	bio->finish = blkbuf_finish;
	bio->bi_private = buf_desc;

	hold_blkio(bio);
	/**
	 * 提交请求，放到块IO队列中
	 */
	blk_submit_request(rw, bio);

	if (bio_flagged(bio, __BIOFLAG_EOPNOTSUPP))
		ret = -EOPNOTSUPP;

	loosen_blkio(bio);

	return ret;
}

/**
 * 进行多个数据块的数据传输，这些数据块不一定物理上相邻。
 * 注意:	在Io完成之前，缓冲区被锁住。
 * rw:		数据传输的方向。
 * nr:		要传输的数据块的块数量。
 * blkbufs:	指向块缓冲区所对应的缓冲区首部的指针数组。
 */
void submit_block_requests(int rw, int nr, struct blkbuf_desc *blkbufs[])
{
	int i;

	for (i = 0; i < nr; i++) {
		struct blkbuf_desc *buf_desc = blkbufs[i];

		/**
		 * 其他路径在提交请求，略过
		 */
		if (blkbuf_try_lock(buf_desc))
			continue;

		hold_blkbuf(buf_desc);
		if (rw == WRITE) {
			buf_desc->finish_io = blkbuf_finish_write_block;
			/**
			 * 只写脏块
			 */
			if (blkbuf_test_clear_dirty(buf_desc)) {
				/**
				 * 提交写请求
				 */
				submit_block_request(WRITE, buf_desc);
				continue;
			}
		} else {
			buf_desc->finish_io = blkbuf_finish_read_block;
			/**
			 * 已经与磁盘同步了，不用再读
			 */
			if (!blkbuf_is_uptodate(buf_desc)) {
				/**
				 * 提交读请求
				 */
				submit_block_request(rw, buf_desc);
				continue;
			}
		}
		/**
		 * 不需要提交IO请求，就释放引用和锁
		 */
		blkbuf_unlock(buf_desc);
		loosen_blkbuf(buf_desc);
	}
}

/**
 * 在页高速缓存中查找缓存页。如果不存在，就返回NULL。
 * bdev:	块设备描述符。
 * block:	要搜索的块号。
 * size:	块大小。
 */
struct blkbuf_desc *
__blkbuf_find_block(struct block_device *bdev, sector_t block, int size)
{
	struct file_node *fnode = bdev->fnode;
	struct file_cache_space *cache_space;
	struct blkbuf_desc *ret = NULL;
	struct blkbuf_desc *buf_desc;
	struct blkbuf_desc *head;
	struct page_frame *page;
	pgoff_t index;

	cache_space = fnode->cache_space;

	/**
	 * 根据块号得到页索引。
	 */
	index = block >> (PAGE_CACHE_SHIFT - fnode->block_size_order);
	/**
	 * 调用find_get_page确定请求的块缓冲区页的会在页高速缓存中的位置。
	 */
	page = pgcache_find_page(cache_space, index);

	/**
	 * 缓存页不存在
	 */
	if (!page)
		return NULL;

	smp_lock(&cache_space->block_lock);
	if (!page_has_blocks(page))
		goto unlock;

	buf_desc = head = page_first_block(page);
	/**
	 * 在页的缓冲区首部链表中搜索逻辑块号等于block的块。
	 */
	do {
		if (buf_desc->block_num_dev == block) {
			ret = buf_desc;
			hold_blkbuf(buf_desc);
			goto unlock;
		}

		buf_desc = buf_desc->next_in_page;
	} while (buf_desc != head);

unlock:
	smp_unlock(&cache_space->block_lock);
	/**
	 * 递减描述符的count字段(find_get_page曾递增它的值)。
	 */
	loosen_page_cache(page);

	return ret;
}

/**
 * 为块设备查找或者创建新的缓存页面
 */
static struct page_frame *
blkdev_find_alloc_pgcache(struct block_device *bdev, sector_t block,
		pgoff_t index, int size)
{
	struct file_node *file_node = bdev->fnode;
	struct blkbuf_desc *buf_desc;
	struct page_frame *page;

	/**
	 * 在页缓存中搜索需要的页
	 * 没有则分配一个页面
	 */
	page = pgcache_find_alloc_lock(file_node->cache_space, index, PAF_NOFS);
	if (!page)
		return NULL;

	ASSERT (pgflag_locked(page));

	if (page_has_blocks(page)) {
		buf_desc = page_first_block(page);
		ASSERT(buf_desc->size == size);
		init_page_blocks(page, bdev, block, size);

		return page;
	}

	/**
	 * 页还不是缓冲区页，调用alloc_page_buffers根据页中请求的块大小分配缓冲区首部。
	 * 并将它们插入由b_this_page字段实现的单向循环链表中。
	 * 此外，函数用页描述符的地址初始化缓冲区首部的b_page字段，用块缓冲区在页内的纯属地址或者偏移量初始化b_data字段。
	 */
	buf_desc = alloc_page_buffers(page, size, 0);
	if (!buf_desc) {
		unlock_page(page);
		loosen_page_cache(page);
		return NULL;
	}

	smp_lock(&file_node->cache_space->block_lock);
	/**
	 * 将块缓存区与页面绑定，注意会增加页面计数
	 */
	attach_page_blocks(page, buf_desc);
	/**
	 * 块设备的逻辑块与物理块必然是连续并映射的
	 * 因此可以直接初始化其映射关系
	 */
	init_page_blocks(page, bdev, block, size);
	smp_unlock(&file_node->cache_space->block_lock);

	return page;
}

/**
 * 为请求的页分配的一个新的缓冲区。
 */
static struct blkbuf_desc *
__blkbuf_alloc(struct block_device *bdev, sector_t block, int size)
{
	struct blkbuf_desc *buf_desc;
	struct page_frame *page;
	pgoff_t index;
	int sizebits;
	sector_t sector;

	might_sleep();
	ASSERT((size & (blkdev_hardsect_size(bdev) - 1)) == 0);
	ASSERT(size >= 512 && size <= PAGE_SIZE);

	/**
	  * 计算数据页在所请求块的块设备中的偏移量
	  */
	sizebits = -1;
	do {
		sizebits++;
	} while ((size << sizebits) < PAGE_SIZE);

	index = block >> sizebits;
	sector = index << sizebits;

	for (;;) {
		/**
		 * 确定缓冲区是否已经在页高速缓存中。
		 */
		buf_desc = __blkbuf_find_block(bdev, block, size);
		if (buf_desc)
			return buf_desc;


		/**
		 * 分配块设备缓冲区页。
		 */
		page = blkdev_find_alloc_pgcache(bdev, sector, index, size);
		if (!page) {
			/**
			 * 等待内存空闲再试
			 */
			msleep(1);
			continue;
		}

		unlock_page(page);
		loosen_page_cache(page);
	}
}

/**
 * 在页面高速缓存中查找块，并递增其引用计数
 * 如果没有，则创建一个块描述符
 *
 * bdev:	设备描述符。
 * block:	要搜索的块号。
 * size:	块大小。
 */
 struct blkbuf_desc *
__blkbuf_find_alloc(struct block_device *bdev, sector_t block, int size)
{
	/**
	 * 在页高速缓存中查找。
	 */
	struct blkbuf_desc *buf_desc = __blkbuf_find_block(bdev, block, size);

	/**
	 * 在缓存中没有，则分配一个块描述符
	 */
	if (buf_desc == NULL)
		buf_desc = __blkbuf_alloc(bdev, block, size);

	return buf_desc;
}

/**
 * 首先在缓冲区中查找块
 * 如果没有，就从磁盘中读取
 */
struct blkbuf_desc *
__blkbuf_read_block(struct block_device *bdev, sector_t block, int size)
{
	struct blkbuf_desc *buf_desc = __blkbuf_find_alloc(bdev, block, size);

	/**
	 * 块中包含有效数据，直接返回
	 */
	if (blkbuf_is_uptodate(buf_desc))
		return buf_desc;

	blkbuf_lock(buf_desc);
	/**
	 * 在锁缓冲区的过程中，其他流程可能读取了数据
	 */
	if (blkbuf_is_uptodate(buf_desc)) {
		blkbuf_unlock(buf_desc);
		return buf_desc;
	} else {
		/**
		 * 首先增加文件块缓冲区的引用计数。
		 */
		hold_blkbuf(buf_desc);
		/**
		 * 设置完成回调
		 */
		buf_desc->finish_io = blkbuf_finish_read_block;
		/**
		 * 提交读块请求
		 */
		submit_block_request(READ, buf_desc);
		/**
		 * 等待I/O操作完成。
		 */
		blkbuf_wait_unlock(buf_desc);
		/**
		 * 如果成功读取到数据，则保持对缓冲区的引用
		 */
		if (blkbuf_is_uptodate(buf_desc))
			return buf_desc;
	}

	/**
	 * 运行到这里，说明读取失败
	 * 在finish_io回调里面释放一次引用，这里再释放一次
	 * 完全释放缓冲描述符，可回收
	 */
	loosen_blkbuf(buf_desc);
	return NULL;
}

/**
 * 得到文件节点的物理块号
 */
sector_t get_mapped_block(struct file_cache_space *space,
	sector_t block, map_block_f *map_block)
{
	struct file_node *file_node = space->fnode;
	struct blkbuf_desc tmp;

	tmp.state = 0;
	tmp.block_num_dev = 0;
	map_block(file_node, block, &tmp, 0);

	return tmp.block_num_dev;
}

/**
 * 基于块设备的文件系统，如果页面块不连续
 * 则调用此函数以一次读一块的方式读一页数据。
 */
int submit_read_page_blocks(struct page_frame *page, map_block_f *map_block)
{
	struct file_node *file_node = page->cache_space->fnode;
	sector_t file_block, last_block;
	struct blkbuf_desc *buf_desc, *first_desc, *io_blocks[MAX_BUF_PER_PAGE];
	unsigned int block_size;
	int io_count, i;
	int fully_mapped = 1;
	int shift = PAGE_CACHE_SHIFT - file_node->block_size_order;

	ASSERT(pgflag_locked(page));

	block_size = 1 << file_node->block_size_order;
	/**
	 * 如果还没有块描述符，则先创建
	 */
	if (!page_has_blocks(page))
		blkbuf_create_desc_page(page, block_size, 0);
	first_desc = page_first_block(page);

	/**
	 * 页面在文件中的块号，以及文件最后一块的块号
	 */
	file_block = (sector_t)page->index << shift;
	last_block = (fnode_size(file_node)+block_size-1) >> file_node->block_size_order;

	/**
	 * 遍历每一个块
	 */
	for (i = 0, io_count = 0, buf_desc = first_desc;
		(i == 0) || (buf_desc != first_desc);
		i++, file_block++, buf_desc = buf_desc->next_in_page) {
		/**
		 * 块与磁盘内容同步
		 */
		if (blkbuf_is_uptodate(buf_desc))
			continue;

		/**
		 * 如果BH_Mapped未置位
		 */
		if (!blkbuf_is_mapped(buf_desc)) {
			/**
			 * 并且该块没有超出文件尾
			 */
			if (file_block < last_block) {
				/**
				 * 将文件节点的逻辑块号转换为块设备的块号
				 */
				if (map_block(file_node, file_block, buf_desc, 0))
					set_page_error(page);
			}
			if (!blkbuf_is_mapped(buf_desc)) {
				void *kaddr = kmap_atomic(page, KM_USER0);

				fully_mapped = 0;
				memset(kaddr + i * block_size, 0, block_size);
				flush_dcache_page(page);
				kunmap_atomic(kaddr, KM_USER0);
				blkbuf_set_uptodate(buf_desc);

				continue;
			}
			/**
			 * 如果文件系统的map_block已经读取了块
			 * 就继续处理下一块
			 */
			if (blkbuf_is_uptodate(buf_desc))
				continue;
		}
		/**
		 * 记录下需要磁盘IO的块
		 */
		io_blocks[io_count++] = buf_desc;
	} 

	/**
	 * 如果没有遇到文件洞，则设置PG_mappedtodisk标志
	 */
	if (fully_mapped)
		set_page_map_to_disk(page);

	/**
	 * arr中存放了一些缓冲区首部的地址，与其对应的缓冲区的内容不是最新的。
	 * 如果数组为空，那么页中所有缓冲区都是有效的。因此，设置页的PG_uptodate标志
	 */
	if (!io_count) {
		if (!pgflag_error(page))
			set_page_uptodate(page);

		/**
		 * 解除页面锁定
		 */
		unlock_page(page);

		return 0;
	}

	for (i = 0; i < io_count; i++) {
		/**
		 * 锁定块，如果有人在操纵此块，则等待其结束
		 */
		blkbuf_lock(io_blocks[i]);
		/**
		 * 在锁定块缓冲的过程中，其他流程可能已经更新了数据
		 * 设置IO结束回调及异步读写标志
		 */
		if (!blkbuf_is_uptodate(io_blocks[i])) {
			io_blocks[i]->finish_io = blkbuf_finish_read_page;
			blkbuf_set_async_read(io_blocks[i]);
			submit_block_request(READ, io_blocks[i]);
		}
	}

	return 0;
}

/**
 * 为文件页的缓冲区和缓冲区首部做准备
 */
static int __blkbuf_prepare_write(struct file_node *file_node,
	struct page_frame *page, unsigned from, unsigned to,
	map_block_f *map_block)
{
	struct blkbuf_desc *buf_desc, *head, *wait[2];
	unsigned long block_start, block_end;
	unsigned block_size, bbits;
	int wait_count = 0, i;
	sector_t block;
	int err = 0;

	ASSERT(pgflag_locked(page));
	ASSERT(from <= PAGE_CACHE_SIZE);
	ASSERT(to <= PAGE_CACHE_SIZE);
	ASSERT(from <= to);

	block_size = 1 << file_node->block_size_order;
	/**
	 * 还没有块缓冲区，创建空缓冲区
	 */
	if (!page_has_blocks(page))
		blkbuf_create_desc_page(page, block_size, 0);

	head = page_first_block(page);
	bbits = file_node->block_size_order;
	block = (sector_t)page->index << (PAGE_CACHE_SHIFT - bbits);
	block_start = 0;

	for (buf_desc = head; buf_desc != head || !block_start;
	    block++, block_start += block_size, buf_desc = buf_desc->next_in_page) {
		block_end = block_start + block_size;
		if (block_end <= from || block_start >= to) {
			if (pgflag_uptodate(page)) {
				if (!blkbuf_is_uptodate(buf_desc))
					blkbuf_set_uptodate(buf_desc);
			}
			continue;
		}

		blkbuf_clear_new(buf_desc);

		/**
		 * 还没有映射到磁盘
		 */
		if (!blkbuf_is_mapped(buf_desc)) {
			/**
			 * 查找逻辑块号对应的物理块号
			 */
			err = map_block(file_node, block, buf_desc, 1);
			if (err)
				goto out;

			/**
			 * 刚映射的新块
			 */
			if (blkbuf_is_new(buf_desc)) {
				blkbuf_clear_new(buf_desc);
				/**
				 * 将磁盘上的旧块内容写入，避免乱序引起的写入错误
				 */
				blkbuf_sync_metablock(buf_desc->blkdev,
							buf_desc->block_num_dev);
				/**
				 * 如果文件系统顺便将块写入磁盘了，就处理下一个块
				 */
				if (pgflag_uptodate(page)) {
					blkbuf_set_uptodate(buf_desc);
					continue;
				}
				/**
				 * 新块，并且只有部分区域被重写
				 * 未被重写的部分清0
				 */
				if (block_end > to || block_start < from) {
					void *kaddr;

					kaddr = kmap_atomic(page, KM_USER0);
					if (block_end > to)
						memset(kaddr+to, 0, block_end - to);
					if (block_start < from)
						memset(kaddr+block_start, 0, from - block_start);
					flush_dcache_page(page);
					kunmap_atomic(kaddr, KM_USER0);
				}
				continue;
			}
		}

		if (pgflag_uptodate(page)) {
			if (!blkbuf_is_uptodate(buf_desc))
				blkbuf_set_uptodate(buf_desc);
			continue; 
		}
		/**
		 * 块缓冲区与磁盘不一致，并且磁盘上存在块
		 * 并且本次操作并没有全写整个块
		 * 那就需要读取磁盘上的内容
		 */
		if (!blkbuf_is_uptodate(buf_desc) && !blkbuf_is_delay(buf_desc) &&
		     (block_start < from || block_end > to)) {
			submit_block_requests(READ, 1, &buf_desc);
			wait[wait_count] = buf_desc;
			wait_count++;
		}
	}

	/**
	 * 等待磁盘读请求完成
	 */
	for (i = 0; i < wait_count; i++) {
		blkbuf_wait_unlock(wait[i]);
		if (!blkbuf_is_uptodate(wait[i]))
			return -EIO;
	}

	return 0;

out:
	buf_desc = head;
	block_start = 0;
	do {
		block_end = block_start + block_size;
		if (block_end <= from)
			continue;
		if (block_start >= to)
			break;
		/**
		 * 前面流程中新分配的块
		 */
		if (blkbuf_is_new(buf_desc)) {
			void *kaddr;

			blkbuf_clear_new(buf_desc);
			kaddr = kmap_atomic(page, KM_USER0);
			memset(kaddr + block_start, 0, buf_desc->size);
			kunmap_atomic(kaddr, KM_USER0);
			blkbuf_set_uptodate(buf_desc);
			blkbuf_mark_dirty(buf_desc);
		}
	} while (block_start += block_size, (buf_desc = buf_desc->next_in_page) != head);

	return err;
}

/**
 * 文件系统意图写某个页面前调用此函数
 * 在prepare_write回调中调用
 */
int blkbuf_prepare_write(struct page_frame *page, unsigned from, unsigned to,
			map_block_f *map_block)
{
	struct file_node *file_node = page->cache_space->fnode;

	int err = __blkbuf_prepare_write(file_node, page, from, to, map_block);
	if (err)
		clear_page_uptodate(page);

	return err;
}

/**
 * 文件系统在写完页面缓存时
 * 调用此函数更新缓存页中的状态
 */
int blkbuf_commit_write(struct file_node *file_node, struct page_frame *page,
		unsigned from, unsigned to)
{
	unsigned block_start, block_end;
	int partial = 0;
	unsigned block_size;
	struct blkbuf_desc *buf_desc, *head;

	block_size = 1 << file_node->block_size_order;
	buf_desc = head = page_first_block(page);

	/**
	 * 考虑页中受写操作影响的所有块
	 * 更新其标志 
	 */
	for (block_start = 0; buf_desc != head || !block_start;
	    buf_desc = buf_desc->next_in_page) {
		block_end = block_start + block_size;
		if (block_end <= from || block_start >= to) {
			if (!blkbuf_is_uptodate(buf_desc))
				partial = 1;
		} else {
			blkbuf_set_uptodate(buf_desc);
			blkbuf_mark_dirty(buf_desc);
		}

		block_start = block_end;
	}

	/**
	 * 如果缓冲区页中的所有块都是最新的
	 * 则将整个页的标志设置为最新。
	 * 这样下次就可以整页读取了
	 */
	if (!partial)
		set_page_uptodate(page);

	return 0;
}

/**
 * address_space对象的commit_write方法。
 * 文件系统执行文件块写操作系统调用
 */
int generic_commit_write(struct file *file, struct page_frame *page,
		unsigned from, unsigned to)
{
	loff_t end = ((loff_t)page->index << PAGE_CACHE_SHIFT) + to;
	struct file_node *file_node = page->cache_space->fnode;

	/**
	 * 处理涉及的文件块
	 */
	blkbuf_commit_write(file_node,page,from,to);

	/**
	 * 检查写操作是否将文件增大
	 * 如果增大则更新文件大小
	 */
	if (end > file_node->file_size) {
		fnode_update_file_size(file_node, end);
		mark_fnode_dirty(file_node);
	}

	return 0;
}

/**
 * 将页面写入到磁盘块中
 * 不必检查页面内容有效性
 */
static int __blkbuf_write_page(struct file_node *file_node,
	struct page_frame *page, map_block_f *map_block,
	struct writeback_control *control)
{
	struct blkbuf_desc *buf_desc, *head;
	int req_count = 0;
	sector_t last_block;
	sector_t block;
	int err;

	ASSERT(pgflag_locked(page));

	/**
	 * 计算文件最后一个块的编号
	 */
	last_block = (fnode_size(file_node) - 1) >> file_node->block_size_order;
	/**
	 * 如果还没有块描述符，就创建空描述符并与页面绑定
	 */
	if (!page_has_blocks(page))
		blkbuf_create_desc_page(page, 1 << file_node->block_size_order,
					(1 << BS_DIRTY) | (1 << BS_UPTODATE));

	block = page->index << (PAGE_CACHE_SHIFT - file_node->block_size_order);
	head = page_first_block(page);
	buf_desc = head;

	do {
		/**
		 * 不在文件有效范围内
		 */
		if (block > last_block) {
			blkbuf_clear_dirty(buf_desc);
			blkbuf_set_uptodate(buf_desc);
		/**
		 * 如果还没有映射到磁盘，并且缓冲区为脏
		 * 必须调用文件系统的回调来获得块号
		 */
		} else if (!blkbuf_is_mapped(buf_desc) && blkbuf_is_dirty(buf_desc)) {
			/**
			 * 将逻辑块号转换为物理块号
			 */
			err = map_block(file_node, block, buf_desc, 1);
			if (err)
				goto recover;
			if (blkbuf_is_new(buf_desc)) {
				blkbuf_clear_new(buf_desc);
				/**
				 * 新分配的块，将该块关联的原有内容写入到设备
				 */
				blkbuf_sync_metablock(buf_desc->blkdev,
							buf_desc->block_num_dev);
			}
		}
		buf_desc = buf_desc->next_in_page;
		block++;
	} while (buf_desc != head);

	do {
		hold_blkbuf(buf_desc);
		/**
		 * 超出文件大小的缓冲区，不需要处理
		 */
		if (!blkbuf_is_mapped(buf_desc))
			continue;

		/**
		 * 要求完整的回写文件，或者允许阻塞
		 */
		if (control->sync_mode != WB_SYNC_NONE 
		    || !(control->flags & WB_NOBLOCK)) {
			blkbuf_lock(buf_desc);
		} else {
			/**
			 * 尝试获得锁
			 * 如果不能获得则继续处理下一个缓冲区
			 */
			if (blkbuf_try_lock(buf_desc)) {
				redirty_page_for_writepage(control, page);/* 重新标记页面为脏 */
				continue;
			}
		}

		/**
		 * 如果页面为脏
		 * 设置其回调函数
		 * 稍后将其写入磁盘
		 */
		if (blkbuf_test_clear_dirty(buf_desc)) {
			buf_desc->finish_io = blkbuf_finish_write_page;
			blkbuf_set_async_write(buf_desc);
		} else
			blkbuf_unlock(buf_desc);
	} while ((buf_desc = buf_desc->next_in_page) != head);

	ASSERT(!pgflag_writeback(page));
	set_page_writeback(page);
	unlock_page(page);

	/**
	 * 对每个块执行写请求
	 */
	do {
		struct blkbuf_desc *next = buf_desc->next_in_page;

		/**
		 * 前面设置了异步写标志，需要提交请求
		 */
		if (blkbuf_is_async_write(buf_desc)) {
			submit_block_request(WRITE, buf_desc);
			req_count++;
		}
		loosen_blkbuf(buf_desc);
		buf_desc = next;
	} while (buf_desc != head);

	err = 0;
done:
	if (req_count == 0) {
		int uptodate = 1;

		do {
			if (!blkbuf_is_uptodate(buf_desc)) {
				uptodate = 0;
				break;
			}
			buf_desc = buf_desc->next_in_page;
		} while (buf_desc != head);

		if (uptodate)
			set_page_uptodate(page);
		/**
		 * 通知上层，页面回写完成
		 */
		page_finish_writeback(page);

		control->skipped_page_count++;
	}

	return err;

recover:
	/**
	 * 遇到错误，将现有的脏块写入
	 */
	buf_desc = head;
	do {
		hold_blkbuf(buf_desc);
		if (blkbuf_is_mapped(buf_desc) && blkbuf_is_dirty(buf_desc)) {
			blkbuf_lock(buf_desc);
			buf_desc->finish_io = blkbuf_finish_write_page;
			blkbuf_set_async_write(buf_desc);
		} else
			blkbuf_clear_dirty(buf_desc);

		buf_desc = buf_desc->next_in_page;
	} while (buf_desc != head);
	set_page_error(page);

	ASSERT(!pgflag_writeback(page));
	set_page_writeback(page);
	unlock_page(page);

	buf_desc = head;
	do {
		struct blkbuf_desc *next = buf_desc->next_in_page;
		if (blkbuf_is_async_write(buf_desc)) {
			blkbuf_clear_dirty(buf_desc);
			submit_block_request(WRITE, buf_desc);
			req_count++;
		}
		loosen_blkbuf(buf_desc);
		buf_desc = next;
	} while (buf_desc != head);

	goto done;
}

/**
 * 回写页面到磁盘中
 */
int blkbuf_write_page(struct page_frame *page, map_block_f *map_block,
			struct writeback_control *control)
{
	struct file_node * const file_node = page->cache_space->fnode;
	loff_t file_size = fnode_size(file_node);
	pgoff_t last_index;
	unsigned offset;
	void *kaddr;

	last_index = file_size >> PAGE_CACHE_SHIFT;
	/**
	 * 页面内容完全在文件范围内
	 */
	if (page->index < last_index)
		return __blkbuf_write_page(file_node, page, map_block, control);

	/**
	 * 页面完全不在文件内
	 */
	offset = file_size & (PAGE_CACHE_SIZE - 1);
	if (page->index > last_index || !offset) {
		blkbuf_invalidate_page(page, 0);
		unlock_page(page);
		return 0;
	}

	/**
	 * 是文件最后一页，并且有一部分内容不在文件范围内
	 */
	kaddr = kmap_atomic(page, KM_USER0);
	/** 
	 * 将超出部分清0，避免向磁盘写入垃圾数据
	 */
	memset(kaddr + offset, 0, PAGE_CACHE_SIZE - offset);
	flush_dcache_page(page);
	kunmap_atomic(kaddr, KM_USER0);
	/**
	 * 将整个页面写入到磁盘
	 */
	return __blkbuf_write_page(file_node, page, map_block, control);
}

/**
 * 对于非数据块，例如元数据和日志块
 * 需要同步写入磁盘
 */
int sync_dirty_block(struct blkbuf_desc *buf_desc)
{
	int ret = 0;

	blkbuf_lock(buf_desc);
	if (blkbuf_test_clear_dirty(buf_desc)) {
		hold_blkbuf(buf_desc);
		buf_desc->finish_io = blkbuf_finish_write_block;
		ret = submit_block_request(WRITE, buf_desc);
		blkbuf_wait_unlock(buf_desc);
		if (blkbuf_is_eopnotsupp(buf_desc)) {
			blkbuf_clear_eopnotsupp(buf_desc);
			ret = -EOPNOTSUPP;
		}
		if (!ret && !blkbuf_is_uptodate(buf_desc))
			ret = -EIO;
	} else
		blkbuf_unlock(buf_desc);

	return ret;
}

void blkbuf_write_boundary(struct block_device *bdev,
			sector_t bblock, unsigned block_size)
{
	struct blkbuf_desc *buf_desc;

	buf_desc = __blkbuf_find_block(bdev, bblock + 1, block_size);
	if (buf_desc) {
		if (blkbuf_is_dirty(buf_desc))
			submit_block_requests(WRITE, 1, &buf_desc);
		loosen_blkbuf(buf_desc);
	}
}

/**
 * 同步写入块设备文件
 */
int blkdev_sync(struct block_device *blkdev)
{
	int ret = 0;

	if (blkdev) {
		ret = writeback_submit_data(blkdev->fnode->cache_space, 0, 0, WB_SYNC_WAIT);
		if (!ret)
			ret = writeback_wait_data(blkdev->fnode->cache_space);
	}

	return ret;
}

/**
 * 将块设备的数据写入到磁盘
 */
int fsync_blkdev(struct block_device *bdev)
{
	struct super_block *super = get_associated_superblock(bdev);
	int ret;

	/**
	 * 设备上装载了文件系统
	 */
	if (super) {
		ret = fsync_filesystem(super);
		unlock_loosen_super_block(super);

		return ret;
	}

	/**
	 * 可能在设备文件系统中打开了块设备
	 */
	ret = blkdev_sync(bdev);

	return ret;
}

/**
 * 设备块已经分配给文件块
 * 将相关的元数据块强制提交
 */
void blkbuf_sync_metablock(struct block_device *bdev, sector_t block)
{
	struct blkbuf_desc *old_bh;

	might_sleep();

	old_bh = __blkbuf_find_block(bdev, block, 0);
	if (old_bh) {
		blkbuf_clear_dirty(old_bh);
		blkbuf_wait_unlock(old_bh);
		blkbuf_clear_requested(old_bh);
		loosen_blkbuf(old_bh);
	}
}

/**
 * sync_page回调实现
 * 当有调用者在等待页面时，调用此函数回写页面
 */
int blkbuf_sync_page(struct page_frame *page)
{
	struct file_cache_space *space;
	struct blkdev_infrast *infrast;

	space = page_cache_space(page);
	infrast = space->blkdev_infrast;
	if (space) {
		if (infrast && infrast->push_io)
			infrast->push_io(infrast, page);
	}

	return 0;
}

static int sync_file(unsigned int fd, bool data)
{
	struct file_cache_space *space;
	struct file *file;
	int ret, err;

	ret = -EBADF;
	/**
	 * 通过文件句柄找到文件
	 */
	file = file_find(fd);
	if (!file)
		return ret;

	space = file->cache_space;

	ret = -EINVAL;
	/**
	 * 回写元数据及文件脏页
	 */
	if (!file->file_ops || !file->file_ops->fsync)
		goto loosen;

	current->flags |= TASKFLAG_SYNCWRITE;
	/**
	 * 回写文件内容即脏页
	 */
	ret = writeback_submit_data(space, 0, 0, WB_SYNC_WAIT);

	/**
	 * 避免并发的同步操作
	 */
	down(&space->fnode->sem);
	/**
	 * 调用文件对象的fsync方法进行数据同步。
	 * 如ext3_sync_file，它负责提交并等待日志完成
	 */
	err = file->file_ops->fsync(file, file->fnode_cache, data);
	if (!ret)
		ret = err;
	up(&space->fnode->sem);

	/**
	 * 等待脏页回写完成
	 */
	err = writeback_wait_data(space);
	if (!ret)
		ret = err;
	current->flags &= ~TASKFLAG_SYNCWRITE;

loosen:
	loosen_file(file);
	return ret;
}

asmlinkage int sys_fdatasync(unsigned int fd)
{
	return sync_file(fd, 1);
}

/**
 * 系统调用fsync的实现。
 * 提交文件的脏数据缓冲区写到磁盘中
 * 以及文件节点元数据
 */
asmlinkage int sys_fsync(unsigned int fd)
{
	return sync_file(fd, 0);
}

static void sync_all(unsigned long wait)
{
	/**
	 * 启动pdflush内核线程。将所有脏页写入到磁盘。
	 */
	kick_writeback_task(0);
	/**
	 * 扫描超级块的链表
	 * 刷新的脏文件节点。
	 */
	sync_file_nodes(0);
	/**
	 * 同步文件系统超级块
	 * 但是并不一定写到磁盘
	 */
	sync_superblocks();
	/**
	 * 调用文件系统特定的sync方法
	 * 确保数据写到磁盘。对ext3来说，就是提交日志。
	 */
	sync_filesystems(0);
	/**
	 * sync_filesystems和sync_inodes被再次调用。
	 * 可能需要等待写磁盘操作完成
	 */
	sync_filesystems(wait);
	sync_file_nodes(wait);
}

/**
 * sync系统调用的实现函数。
 * 等待所有磁盘写入完毕。
 */
asmlinkage int sys_sync(void)
{
	sync_all(1);

	return 0;
}

void __init init_buffer_module(void)
{
	blkbuf_desc_allotter = beehive_create("block-buffer",
			sizeof(struct blkbuf_desc), 0,
			BEEHIVE_PANIC, NULL);
}
