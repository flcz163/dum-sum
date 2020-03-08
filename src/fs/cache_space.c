#include <dim-sum/blkio.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/blk_infrast.h>
#include <dim-sum/block_buf.h>
#include <dim-sum/fs.h>
#include <dim-sum/highmem.h>
#include <dim-sum/pagemap.h>
#include <dim-sum/pagevec.h>
#include <dim-sum/sched.h>
#include <dim-sum/writeback.h>

#include <asm/cacheflush.h>

static int finish_read(struct block_io_desc *bio, unsigned int bytes_done, int err)
{
	const int uptodate = (bio->bi_flags & BIOFLAG_UPTODATE);
	struct block_io_item *items = bio->items + bio->item_count - 1;

	if (bio->remain_size)
		return 1;

	do {
		struct page_frame *page = items->bv_page;

		items--;

		if (uptodate)
			set_page_uptodate(page);
		else {
			clear_page_uptodate(page);
			set_page_error(page);
		}
		unlock_page(page);
	} while (items >= bio->items);

	loosen_blkio(bio);

	return 0;
}

static int finish_write(struct block_io_desc *bio, unsigned int bytes_done, int err)
{
	const int uptodate = (bio->bi_flags & BIOFLAG_UPTODATE);
	struct block_io_item *items = bio->items + bio->item_count - 1;

	if (bio->remain_size)
		return 1;

	do {
		struct page_frame *page = items->bv_page;

		items--;
		if (!uptodate)
			set_page_error(page);

		if (!test_clear_page_writeback(page))
			BUG();
		smp_mb__after_clear_bit();
		wake_up_page(page, PG_writeback);
	} while (items >= bio->items);

	loosen_blkio(bio);

	return 0;
}

static struct block_io_desc *submit_one_bio(int rw, struct block_io_desc *bio)
{
	bio->finish = finish_read;

	if (rw == WRITE)
		bio->finish = finish_write;
	blk_submit_request(rw, bio);

	return NULL;
}


static struct block_io_desc *alloc_one_bio(struct block_device *bdev,
	sector_t first_sector, int nr_vecs, int paf_flags)
{
	struct block_io_desc *bio;

	bio = blkio_alloc(paf_flags, nr_vecs);

	if (bio == NULL && (current->flags & TASKFLAG_RECLAIM)) {
		while (!bio && (nr_vecs /= 2))
			bio = blkio_alloc(paf_flags, nr_vecs);
	}

	if (bio) {
		bio->bi_bdev = bdev;
		bio->start_sector = first_sector;
	}

	return bio;
}

static void 
set_page_buffer(struct page_frame *page, struct blkbuf_desc *bh, int page_block) 
{
	struct file_node *file_node = page->cache_space->fnode;
	struct blkbuf_desc *page_bh, *head;
	int block = 0;

	if (!page_has_blocks(page)) {
		if (file_node->block_size_order == PAGE_CACHE_SHIFT && 
		    blkbuf_is_uptodate(bh)) {
			set_page_uptodate(page);    
			return;
		}
		blkbuf_create_desc_page(page, 1 << file_node->block_size_order, 0);
	}

	head = page_first_block(page);
	page_bh = head;
	do {
		if (block == page_block) {
			page_bh->state = bh->state;
			page_bh->blkdev = bh->blkdev;
			page_bh->block_num_dev = bh->block_num_dev;
			break;
		}
		page_bh = page_bh->next_in_page;
		block++;
	} while (page_bh != head);
}

/**
 * 对大多数文件来说，本函数是其readpage的实现方法。
 */
static struct block_io_desc *
real_readpage(struct block_io_desc *bio, struct page_frame *page, unsigned page_count,
			sector_t *last_block_in_bio, map_block_f map_block)
{
	struct file_node *file_node = page->cache_space->fnode;
	const unsigned block_size_order = file_node->block_size_order;
	const unsigned blocks_per_page = PAGE_CACHE_SIZE >> block_size_order;
	const unsigned blocksize = 1 << block_size_order;
	sector_t block_in_file;
	sector_t last_block;
	sector_t blocks[MAX_BUF_PER_PAGE];
	unsigned page_block;
	unsigned first_hole = blocks_per_page;
	struct block_device *bdev = NULL;
	struct blkbuf_desc bh;
	int length;

	/**
	 * 曾经读过相应的页面，并且页面内的块并不连续
	 */
	if (page_has_blocks(page))
		goto slow;

	block_in_file = page->index << (PAGE_CACHE_SHIFT - block_size_order);
	last_block = (fnode_size(file_node) + blocksize - 1) >> block_size_order;

	bh.page = page;
	for (page_block = 0; page_block < blocks_per_page; page_block++) {
		bh.state = 0;
		if (block_in_file < last_block) {
			if (map_block(file_node, block_in_file, &bh, 0))
				goto slow;
		}

		/**
		 * 该块是空洞
		 */
		if (!blkbuf_is_mapped(&bh)) {
			if (first_hole == blocks_per_page)
				first_hole = page_block;
			block_in_file++;
			continue;
		}

		/**
		 * 某些文件系统在映射块的时候，会顺便读取块的内容
		 */
		if (blkbuf_is_uptodate(&bh)) {
			/**
			 * 直接将缓存块与页面链接起来即可
			 */
			set_page_buffer(page, &bh, page_block);
			goto slow;
		}

		/**
		 * 当前块映射了，但是前面有空洞
		 */
		if (first_hole != blocks_per_page)
			goto slow;

		/**
		 * 与前面的块不连续
		 */
		if (page_block && blocks[page_block-1] != bh.block_num_dev-1)
			goto slow;
		blocks[page_block] = bh.block_num_dev;
		bdev = bh.blkdev;
		block_in_file++;
	}

	/**
	 * 运行到此，说明页中的所有块在磁盘上是相邻的。
	 * 但是有部分空洞
	 */
	if (first_hole != blocks_per_page) {
		char *kaddr = kmap_atomic(page, KM_USER0);

		/**
		 * 把空洞块全部清0
		 */
		memset(kaddr + (first_hole << block_size_order), 0,
				PAGE_CACHE_SIZE - (first_hole << block_size_order));
		flush_dcache_page(page);
		kunmap_atomic(kaddr, KM_USER0);
		/**
		 * 所有块都是空洞
		 */
		if (first_hole == 0) {
			set_page_uptodate(page);
			unlock_page(page);
			return bio;
		}
	} else
		set_page_map_to_disk(page);

	/**
	 * 与上一页不连续，必须要提交前一个请求
	 */
	if (bio && (*last_block_in_bio != blocks[0] - 1))
		bio = submit_one_bio(READ, bio);

alloc_new:
	if (bio == NULL) {
		bio = alloc_one_bio(bdev, blocks[0] << (block_size_order - 9),
			  	min_t(int, page_count, blkdev_get_max_pages(bdev)),
				PAF_KERNEL);
		if (bio == NULL)
			goto slow;
	}

	length = first_hole << block_size_order;
	if (blkio_add_page(bio, page, length, 0) < length) {
		bio = submit_one_bio(READ, bio);
		goto alloc_new;
	}

	/**
	 * 向驱动提交bio请求。
	 */
	if (blkbuf_is_boundary(&bh) || (first_hole != blocks_per_page))
		bio = submit_one_bio(READ, bio);
	else
		*last_block_in_bio = blocks[blocks_per_page - 1];

	return bio;

/**
 * 函数运行到这里，则页中含有的块在磁盘不连续。
 */
slow:
	if (bio)
		bio = submit_one_bio(READ, bio);
	if (!pgflag_uptodate(page))
		/**
		 * 页不是最新的，则调用block_read_full_page一次读一块的方式读该页。
		 */
		submit_read_page_blocks(page, map_block);
	else
		/**
		 * 如果页是最新的，则调用unlock_page来对该页解锁。
		 */
		unlock_page(page);

	return bio;
}

/**
 * 对大多数文件来说，本函数是其页面缓存对象的read_page实现。
 */
int generic_readpage(struct page_frame *page, map_block_f map_block)
{
	sector_t last_block_in_bio = 0;
	struct block_io_desc *bio = NULL;

	bio = real_readpage(bio, page, 1, &last_block_in_bio, map_block);
	if (bio)
		submit_one_bio(READ, bio);

	return 0;
}

int generic_readpages(struct file_cache_space *space,
	struct double_list *pages, unsigned page_count,
	map_block_f map_block)
{
	struct block_io_desc *bio = NULL;
	unsigned page_idx;
	sector_t last_block_in_bio = 0;

	for (page_idx = 0; page_idx < page_count; page_idx++) {
		struct page_frame *page = list_last_container(pages, struct page_frame, pgcache_list);

		list_del(&page->pgcache_list);
		if (!add_to_page_cache(page, space, page->index, PAF_KERNEL))
			bio = real_readpage(bio, page, page_count - page_idx,
					&last_block_in_bio, map_block);
		else
			loosen_page_cache(page);
	}
	BUG_ON(!list_is_empty(pages));

	if (bio)
		submit_one_bio(READ, bio);

	return 0;
}

/**
 * 非日志型文件系统使用此默认函数来写入页
 * 可以充分利用磁盘的分散/聚集功能
 */
static struct block_io_desc *
writepage_nojournal(struct block_io_desc *bio, struct page_frame *page,
	map_block_f map_block, sector_t *last_block_in_bio, int *ret,
	struct writeback_control *control)
{
	struct file_node *file_node = page->cache_space->fnode;
	struct file_cache_space *space = page->cache_space;
	const unsigned block_size_order = file_node->block_size_order;
	unsigned long end_index;
	const unsigned blocks_per_page = PAGE_CACHE_SIZE >> block_size_order;
	sector_t last_block;
	sector_t block_in_file;
	sector_t blocks[MAX_BUF_PER_PAGE];
	unsigned page_block;
	unsigned first_unmapped = blocks_per_page;
	struct block_device *bdev = NULL;
	int boundary = 0;
	sector_t boundary_block = 0;
	struct block_device *boundary_bdev = NULL;
	int length;
	struct blkbuf_desc blkbuf_set_map_data;
	loff_t file_size = fnode_size(file_node);

	if (page_has_blocks(page)) {
		struct blkbuf_desc *head = page_first_block(page);
		struct blkbuf_desc *bh = head;

		page_block = 0;
		do {
			BUG_ON(blkbuf_is_locked(bh));
			if (!blkbuf_is_mapped(bh)) {
				if (first_unmapped == blocks_per_page)
					first_unmapped = page_block;
				if (blkbuf_is_dirty(bh))
					goto slow;
				continue;
			}

			if (first_unmapped != blocks_per_page)
				goto slow;

			if (!blkbuf_is_dirty(bh) || !blkbuf_is_uptodate(bh))
				goto slow;

			if (page_block) {
				/**
				 * 与上一个块不连续
				 */
				if (bh->block_num_dev != blocks[page_block-1] + 1)
					goto slow;
			}
			blocks[page_block++] = bh->block_num_dev;

			/**
			 * 逻辑上连续，实际上不连续的块
			 * 如磁盘边界上的块
			 */
			boundary = blkbuf_is_boundary(bh);
			if (boundary) {
				boundary_block = bh->block_num_dev;
				boundary_bdev = bh->blkdev;
			}
			bdev = bh->blkdev;
		} while ((bh = bh->next_in_page) != head);

		/**
		 * 页面内部分或者全部块是连续的
		 */
		if (first_unmapped)
			goto page_is_mapped;

		/**
		 * 整个页面都没有映射到小块
		 * 可能是读取了一个完整的空洞页面
		 */
		goto slow;
	}

	/**
	 * 页面没有分割成小块
	 */
	BUG_ON(!pgflag_uptodate(page));
	/**
	 * 计算页面在缓存空间中的块号
	 */
	block_in_file = page->index << (PAGE_CACHE_SHIFT - block_size_order);
	last_block = (file_size - 1) >> block_size_order;
	blkbuf_set_map_data.page = page;
	for (page_block = 0; page_block < blocks_per_page; ) {
		blkbuf_set_map_data.state = 0;
		if (map_block(file_node, block_in_file, &blkbuf_set_map_data, 1))
			goto slow;
		/**
		 * 新分配的块，需要把磁盘上该块对应的缓冲区处理掉
		 * 否则内容可能被覆盖
		 */
		if (blkbuf_is_new(&blkbuf_set_map_data))
			blkbuf_sync_metablock(blkbuf_set_map_data.blkdev,
						blkbuf_set_map_data.block_num_dev);

		if (blkbuf_is_boundary(&blkbuf_set_map_data)) {
			boundary_block = blkbuf_set_map_data.block_num_dev;
			boundary_bdev = blkbuf_set_map_data.blkdev;
		}

		/**
		 * 两个块不连续
		 */
		if (page_block) {
			if (blkbuf_set_map_data.block_num_dev != blocks[page_block-1] + 1)
				goto slow;
		}

		blocks[page_block++] = blkbuf_set_map_data.block_num_dev;
		boundary = blkbuf_is_boundary(&blkbuf_set_map_data);
		bdev = blkbuf_set_map_data.blkdev;
		if (block_in_file == last_block)
			break;
		block_in_file++;
	}
	BUG_ON(page_block == 0);

	first_unmapped = page_block;

page_is_mapped:
	end_index = file_size >> PAGE_CACHE_SHIFT;
	/**
	 * 该页是文件最后一页
	 */
	if (page->index >= end_index) {
		unsigned offset = file_size & (PAGE_CACHE_SIZE - 1);
		char *kaddr;

		if (page->index > end_index || !offset)
			goto slow;
		/**
		 * 将本页内超过文件大小的部分清0
		 */
		kaddr = kmap_atomic(page, KM_USER0);
		memset(kaddr + offset, 0, PAGE_CACHE_SIZE - offset);
		flush_dcache_page(page);
		kunmap_atomic(kaddr, KM_USER0);
	}

	/**
	 * 与上一页的bio请求不连续
	 * 应当将前一个bio请求提交出去
	 */
	if (bio && *last_block_in_bio != blocks[0] - 1)
		bio = submit_one_bio(WRITE, bio);

	/**
	 * 将页追加为bio描述符中的一段。
	 */
alloc_new:
	/**
	 * 如果传入的bio为空，就初始化一个新的bio描述符地址。
	 * 并将该描述符返回给调用函数，
	 * 调用函数下次调用本函数时，将该描述符再次传入。
	 * 这样，同一个bio可以加载几个页。
	 */
	if (bio == NULL) {
		bio = alloc_one_bio(bdev, blocks[0] << (block_size_order - 9),
				blkdev_get_max_pages(bdev), PAF_NOFS | __PAF_EMERG);

		if (bio == NULL)
			goto slow;
	}

	length = first_unmapped << block_size_order;
	/**
	 * 无法合并bio请求，也提交并重新申请BIO
	 */
	if (blkio_add_page(bio, page, length, 0) < length) {
		bio = submit_one_bio(WRITE, bio);
		goto alloc_new;
	}

	/**
	 * 传输请求已经提交，清除块的dirty标志
	 */
	if (page_has_blocks(page)) {
		struct blkbuf_desc *head = page_first_block(page);
		struct blkbuf_desc *bh = head;
		unsigned buffer_counter = 0;

		do {
			if (buffer_counter++ == first_unmapped)
				break;
			blkbuf_clear_dirty(bh);
			bh = bh->next_in_page;
		} while (bh != head);
	}

	BUG_ON(pgflag_writeback(page));
	set_page_writeback(page);
	unlock_page(page);

	/**
	 * 遇到不可合并的边界块
	 * 或者有未映射的块，后续请求肯定不连续
	 */
	if (boundary || (first_unmapped != blocks_per_page)) {
		/**
		 * 把前面的请求提交掉
		 */
		bio = submit_one_bio(WRITE, bio);
		if (boundary_block) {
			blkbuf_write_boundary(boundary_bdev,
					boundary_block, 1 << block_size_order);
		}
	} else
		*last_block_in_bio = blocks[blocks_per_page - 1];

	return bio;

slow:
	if (bio)
		bio = submit_one_bio(WRITE, bio);
	*ret = page->cache_space->ops->writepage(page, control);

	if (*ret) {
		if (*ret == -ENOSPC)
			atomic_set_bit(__CS_ENOSPC, &space->flags);
		else
			atomic_set_bit(__CS_EIO, &space->flags);
	}

	return bio;
}

int writepage_journal(struct file_cache_space *space,
	struct page_frame *page, struct writeback_control *control)
{
	int ret;

	ASSERT(space && space->ops && space->ops->writepage);

	ret = space->ops->writepage(page, control);
	if (ret) {
		if (ret == -ENOSPC)
			atomic_set_bit(__CS_ENOSPC, &space->flags);
		else
			atomic_set_bit(__CS_EIO, &space->flags);
	}

	return ret;
}

static struct block_io_desc *scan_rangle(struct file_cache_space *space,
	struct writeback_control *control, pgoff_t start, pgoff_t end,
	map_block_f map_block, writepage_journal_f writepage_journal)
{
	struct blkdev_infrast *infrast = space->blkdev_infrast;
	sector_t last_block_in_bio = 0;
	struct block_io_desc *bio = NULL;
	struct pagevec pvec;
	pgoff_t index = start;
	int page_count;
	int ret;

	pagevec_init(&pvec, 0);

	while (index <= end) {
		unsigned i;

		/**
		 * 在页高速缓存中查找脏页描述符。
		 */
	 	page_count = pgcache_collect_pages_tag(&pvec, space, &index,
			PAGECACHE_TAG_DIRTY, min(end - index - 1, (pgoff_t)PAGEVEC_SIZE - 1) + 1);
		if (!page_count)
			break;

		control->scanned = true;
		/**
		 * 处理找到的每个脏页。
		 */
		for (i = 0; i < page_count; i++) {
			struct page_frame *page = pvec.pages[i];

			/**
			 * 先锁住脏页.
			 */
			lock_page(page);

			/**
			 * 确认页是有效的，并在页高速缓存内。
			 * 这是因为在锁住页之前，其他内核代码可能操作了该页。
			 */
			if (unlikely(page->cache_space != space)) {
				unlock_page(page);
				continue;
			}

			if (page->index > end) {
				unlock_page(page);
				break;
			}

			/**
			 * 页已经被提交到块设备。
			 * 等待其执行完毕
			 */
			if (control->sync_mode != WB_SYNC_NONE)
				wait_on_page_writeback(page);

			/** 
			 * 如果不需要同步等待写入完毕
			 * 或者页面不再脏
			 */
			if (pgflag_writeback(page) || !clear_page_dirty_for_io(page)) {
				unlock_page(page);
				continue;
			}

			/**
			 * 确实需要回写页面了
			 */
			if (writepage_journal)
				ret = writepage_journal(space, page, control);
			else
				bio = writepage_nojournal(bio, page, map_block,
						&last_block_in_bio, &ret, control);

			if (ret == 0)
				control->remain_page_count--;

			if ((control->flags & WB_NOBLOCK) && blkdev_write_congested(infrast)) {
				control->flags |= WB_CONGESTED;
				control->done = true;
				break;
			}

			if (ret || !control->remain_page_count) {
				control->done = true;
				break;
			}
		}

		pagevec_release(&pvec);
	}

	if (control->sync_mode == WB_SYNC_NONE)
		space->writeback_index = index;

	control->res = ret;

	return bio;
}

/**
 * 遍历缓存空间的脏页，并调用writepage方法将脏页写回磁盘。
 * 有如下执行路径会调用此方法:
 *	1、缓存回写及同步写需要调用。
 *	2、类似于fsync的系统调用
 */
int __generic_writepages(struct file_cache_space *space,
		struct writeback_control *control, map_block_f map_block,
		writepage_journal_f writepage_journal)
{
	struct blkdev_infrast *infrast = space->blkdev_infrast;
	struct block_io_desc *bio = NULL;
	int ret = 0;

	/**
	 * 请求队列写拥塞，并且进程不希望阻塞
	 */
	if ((control->flags & WB_NOBLOCK) && blkdev_write_congested(infrast)) {
		control->flags |= WB_CONGESTED;
		return 0;
	}

	control->done = false;
	control->scanned = false;
	/**
	 * 确定首页。如果wbc描述符指定线程无需等待IO数据传输结束，则将mapping->writeback_index设为初始页索引。
	 * 也就是说，从上一个写回操作的最后一页开始扫描。
	 */
	if (control->sync_mode == WB_SYNC_NONE) {
		bio = scan_rangle(space, control, space->writeback_index, -1,
			map_block, writepage_journal);
		/**
		 * 没有扫描完给定范围内的所有页，或者写到磁盘的有效页数小于wbc中给定的值，则继续
		 */
		if (!control->scanned && !control->done) {
			bio = scan_rangle(space, control, 0, -1,
				map_block, writepage_journal);
		}
	} else {
		pgoff_t index;
		pgoff_t end = -1;

		if (control->start || control->end) {
			index = control->start >> PAGE_CACHE_SHIFT;
			end = control->end >> PAGE_CACHE_SHIFT;
		} else {
			index = 0;
			end = -1;
		}

		bio = scan_rangle(space, control, index, end, map_block, writepage_journal);
	}

	/**
	 * 如果曾经调用过writepage_nojournal函数，而且返回了bio描述符地址
	 * 则提交此请求
	 */
	if (bio)
		submit_one_bio(WRITE, bio);

	ret = control->res;

	return ret;
}
