#include <dim-sum/beehive.h>
#include <dim-sum/blkio.h>
#include <dim-sum/blk_dev.h>

int blkdev_get_max_pages(struct block_device *bdev)
{
	struct blk_request_queue *queue = blkdev_get_queue(bdev);
	int page_count;

	page_count = ((queue->request_settings.max_sectors << 9) + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (page_count > queue->request_settings.max_hw_segment_count)
		page_count = queue->request_settings.max_hw_segment_count;

	return page_count;
}

static void
recount_segments(struct blk_request_queue *queue, struct block_io_desc *blkio)
{
	struct block_io_item *io_item, *prev_item = NULL;
	int i, hw_segments, seg_size;
	bool merge_able, prev_merge_able = 0;
	bool comblined;

	if (unlikely(!blkio->item_count))
		return;

	comblined = queue->state & BLKQUEUE_COMBINED;
	seg_size = hw_segments = 0;
	bio_for_each_item(io_item, blkio, i) {
		/*
		 * 超过硬件限制的页，可能需要回弹缓冲区
		 * 因此不能参与合并
		 */
		merge_able = number_of_page(io_item->bv_page) < queue->max_pgnum;
		if (!merge_able || !prev_merge_able)
			goto new;

		if (comblined) {
			if (seg_size + io_item->length > queue->request_settings.max_size)
				goto new;
			if (!BIOVEC_PHYS_MERGEABLE(prev_item, io_item))
				goto new;
			if (!BIOVEC_SEG_BOUNDARY(queue, prev_item, io_item))
				goto new;

			seg_size += io_item->length;
			prev_item = io_item;
			continue;
		}
new:
		hw_segments++;
		prev_item = io_item;
		seg_size = io_item->length;
		prev_merge_able = merge_able;
	}

	blkio->hw_segments = hw_segments;
	__set_bit(__BIOFLAG_RECOUNT, &blkio->bi_flags);
}

int blkio_hw_segments(struct blk_request_queue *queue,
	struct block_io_desc *blkio)
{
	if (unlikely(!(blkio->bi_flags & BIOFLAG_RECOUNT)))
		recount_segments(queue, blkio);

	return blkio->hw_segments;
}

static void blkio_init(struct block_io_desc *blkio)
{
	memset(blkio, 0, sizeof(*blkio));
	blkio->bi_flags = BIOFLAG_UPTODATE;
	accurate_set(&blkio->ref_count, 1);
}

/**
 * 释放bio时的回调函数
 */
static void blkio_free(struct block_io_desc *blkio)
{
	kfree(blkio);
}

/**
 * 分配一个新的磁盘IO请求描述符
 */
struct block_io_desc *blkio_alloc(int paf_mask, int item_count)
{
	int size;
	struct block_io_desc *blkio;

	size = sizeof(struct block_io_item) * item_count
			+ sizeof(struct block_io_desc);
	blkio = kmalloc(size, paf_mask);

	if (likely(blkio)) {
		blkio_init(blkio);
		blkio->max_item_count = item_count;
		blkio->free = blkio_free;
	}

	return blkio;
}

void loosen_blkio(struct block_io_desc *blkio)
{
	ASSERT(accurate_read(&blkio->ref_count) > 0);

	if (accurate_dec_and_test_zero(&blkio->ref_count)) {
		blkio->bi_next = NULL;
		blkio->free(blkio);
	}
}

/**
 * 向请求描述符中添加一个页面
 */
int blkio_add_page(struct block_io_desc *blkio, struct page_frame *page,
	unsigned int len, unsigned int offset)
{
	int segments = 0;
	struct block_io_item *io_item;
	struct blk_request_queue *queue = blkdev_get_queue(blkio->bi_bdev);

	if (blkio->item_count >= blkio->max_item_count)
		return 0;

	if (((blkio->remain_size + len) >> 9) > queue->request_settings.max_sectors)
		return 0;

	segments = blkio_hw_segments(queue, blkio);
	if (segments >= queue->request_settings.max_hw_segment_count)
		return 0;

	io_item = &blkio->items[blkio->item_count];
	io_item->bv_page = page;
	io_item->length = len;
	io_item->bv_offset = offset;

	/**
	 * 可能会合并，硬件段数量可能失效
	 * 因此设置强制计算标志
	 */
	if (blkio->item_count && BIOVEC_PHYS_MERGEABLE(io_item - 1, io_item))
		__clear_bit(__BIOFLAG_RECOUNT, &blkio->bi_flags);

	blkio->item_count++;
	blkio->hw_segments++;
	blkio->remain_size += len;

	return len;
}

/**
 * 块设备中断里调用该函数
 * 用来告诉上层IO传输完毕
 */
void blkio_finished(struct block_io_desc *blkio,
	unsigned int finish_bytes, int error)
{
	if (error)
		atomic_clear_bit(__BIOFLAG_UPTODATE, &blkio->bi_flags);

	BUG_ON(finish_bytes > blkio->remain_size);

	blkio->remain_size -= finish_bytes;
	blkio->start_sector += (finish_bytes >> 9);

	/**
	 * 调用请求回调函数，通知上层
	 * 也有可能做一些回弹缓冲区的处理
	 */
	if (blkio->finish)
		blkio->finish(blkio, finish_bytes, error);
}
