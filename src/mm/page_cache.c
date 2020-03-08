#include <dim-sum/aio.h>
#include <dim-sum/blk_infrast.h>
#include <dim-sum/err.h>
#include <dim-sum/errno.h>
#include <dim-sum/hash.h>
#include <dim-sum/highmem.h>
#include <dim-sum/mm.h>
#include <dim-sum/pagemap.h>
#include <dim-sum/pagevec.h>
#include <dim-sum/sched.h>
#include <dim-sum/swap.h>
#include <dim-sum/uio.h>
#include <dim-sum/writeback.h>

#include <asm/cacheflush.h>
#include <uapi/asm-generic/resource.h>

struct approximate_counter pagecache_count;

static struct wait_queue *page_waitqueue(struct page_frame *page)
{
	const struct page_area *pg_area = page_to_pgarea(page);

	return &pg_area->wait_table[hash_ptr(page, pg_area->wait_table_bits)];
}

void wait_on_page_bit(struct page_frame *page, int bit_nr)
{
	DEFINE_WAIT_BIT(wait, &page->flags, bit_nr);

	if (test_bit(bit_nr, &page->flags))
		__wait_on_bit(page_waitqueue(page), &wait, iosched_bit_wait,
							TASK_UNINTERRUPTIBLE);
}

void wake_up_page(struct page_frame *page, int bit)
{
	__wake_up_bit(page_waitqueue(page), &page->flags, bit);
}

void lock_page(struct page_frame *page)
{
	might_sleep();

	if (pgflag_test_and_set_locked(page)) {
		DEFINE_WAIT_BIT(wait, &page->flags, PG_locked);

		__wait_on_bit_lock(page_waitqueue(page),
			&wait, iosched_bit_wait, TASK_UNINTERRUPTIBLE);
	}
}

void fastcall unlock_page(struct page_frame *page)
{
	if (!TestClearPageLocked(page))
		BUG();

	clear_bit_unlock(PG_locked, &page->flags);
	smp_mb__after_atomic();

	wake_up_page(page, PG_locked);
}

/**
 * 在页面缓存中查找一组页面
 */
unsigned pgcache_find_pages(struct file_cache_space *space, pgoff_t start,
			    unsigned int page_count, struct page_frame **pages)
{
	unsigned int i, ret;

	smp_lock_irq(&space->tree_lock);

	/**
	 * radix_tree_gang_lookup实现真正的查找操作
	 * 它为指针数组赋值并返回找到的页数。
	 * 其中一些页可能不在页高速缓存中
	 * 这些页的页面索引为空
	 */
	ret = radix_tree_gang_lookup(&space->page_tree,
				(void **)pages, start, page_count);
	for (i = 0; i < ret; i++)
		page_cache_hold(pages[i]);

	smp_unlock_irq(&space->tree_lock);
	return ret;
}

/**
 * 在页面缓存中，查找特定的页面
 */
struct page_frame * pgcache_find_page(struct file_cache_space *space, unsigned long offset)
{
	struct page_frame *page;

	smp_lock_irq(&space->tree_lock);
	page = radix_tree_lookup(&space->page_tree, offset);
	if (page)
		page_cache_hold(page);
	smp_unlock_irq(&space->tree_lock);

	return page;
}

/**
 * 在页面缓存中，查找特定的页面
 * 并锁住该页
 */
static struct page_frame *pgcache_find_lock_page(struct file_cache_space *space,
				unsigned long offset)
{
	struct page_frame *page;

	smp_lock_irq(&space->tree_lock);

repeat:
	page = radix_tree_lookup(&space->page_tree, offset);
	if (page) {
		page_cache_hold(page);
		/**
		 * 被其他任务占住了页面，必须等待
		 */
		if (pgflag_test_and_set_locked(page)) {
			smp_unlock_irq(&space->tree_lock);
			lock_page(page);
			smp_lock_irq(&space->tree_lock);

			if (page->cache_space != space || page->index != offset) {
				unlock_page(page);
				loosen_page_cache(page);
				goto repeat;
			}
		}
	}

	smp_unlock_irq(&space->tree_lock);

	return page;
}

/**
 * 与find_get_pages类似，但是返回的只是那些用tag参数标记的页。
 * 这个函数对于快速找到一个索引节点的所有脏页是非常关键的。
 */
unsigned pgcache_find_pages_tag(struct file_cache_space *space, pgoff_t *index,
			int tag, unsigned int page_count, struct page_frame **pages)
{
	unsigned int i;
	unsigned int ret;

	smp_lock_irq(&space->tree_lock);
	ret = radix_tree_gang_lookup_tag(&space->page_tree,
				(void **)pages, *index, page_count, tag);
	for (i = 0; i < ret; i++)
		page_cache_hold(pages[i]);
	if (ret)
		*index = pages[ret - 1]->index + 1;
	smp_unlock_irq(&space->tree_lock);

	return ret;
}

unsigned pgcache_collect_pages(struct pagevec *pvec, struct file_cache_space *space,
		pgoff_t start, unsigned page_count)
{
	pvec->nr = pgcache_find_pages(space, start, page_count, pvec->pages);

	return pagevec_count(pvec);
}

/**
 * 在页面缓存中查找特定标志的页面
 */
unsigned pgcache_collect_pages_tag(struct pagevec *pvec,
	struct file_cache_space *space, pgoff_t *index, int tag,
	unsigned page_count)
{
	pvec->nr = pgcache_find_pages_tag(space, index, tag,
					page_count, pvec->pages);

	return pagevec_count(pvec);
}

/**
 * 遍历文件缓存空间，查找是否存在特定标志的页面
 */
int cache_space_tagged(struct file_cache_space *space, int tag)
{
	unsigned long flags;
	int ret;

	smp_lock_irqsave(&space->tree_lock, flags);
	ret = radix_tree_tagged(&space->page_tree, tag);
	smp_unlock_irqrestore(&space->tree_lock, flags);

	return ret;
}

int add_to_page_cache(struct page_frame *page, struct file_cache_space *space,
		pgoff_t offset, int paf_mask)
{
	int error = radix_tree_preload(paf_mask & ~__PAF_USER);

	if (error == 0) {
		smp_lock_irq(&space->tree_lock);
		/**
		 * 调用radix_tree_insert在树中插入新结点
		 */
		error = radix_tree_insert(&space->page_tree, offset, page);
		if (!error) {
			page_cache_hold(page);
			pgflag_set_locked(page);
			page->cache_space = space;
			page->index = offset;
			space->page_count++;
			pgcache_add_count(1);
		}
		smp_unlock_irq(&space->tree_lock);
		radix_tree_preload_end();
	}

	return error;
}

/**
 * 从页高速缓存中删除页描述符
 * 调用者确保页面缓存空间存在并且持有其基树锁
 */
void __remove_from_page_cache(struct page_frame *page)
{
	struct file_cache_space *cache_space = page->cache_space;

	/**
	 * 从基树中移除页面
	 */
	radix_tree_delete(&cache_space->page_tree, page->index);
	page->cache_space= NULL;
	cache_space->page_count--;
	pgcache_sub_count(1);
}

/**
 * 从页高速缓存中删除页描述符
 */
void remove_from_page_cache(struct page_frame *page)
{
	struct file_cache_space *cache_space = page->cache_space;

	ASSERT(pgflag_locked(page));

	smp_lock_irq(&cache_space->tree_lock);
	/**
	 * __remove_from_page_cache真正从树中删除节点。
	 */
	__remove_from_page_cache(page);
	smp_unlock_irq(&cache_space->tree_lock);
}

/**
 * 在页缓存中查找特定页面
 * 如果不存在就分配一个
 */
struct page_frame *pgcache_find_alloc_lock(struct file_cache_space *space,
		unsigned long index, unsigned int paf_mask)
{
	struct page_frame *page, *cached_page = NULL;
	int err;

repeat:
	page = pgcache_find_lock_page(space, index);
	if (!page) {
		if (!cached_page) {
			cached_page = alloc_page_frame(paf_mask);
			if (!cached_page)
				return ERR_PTR(-ENOMEM);
		}
		err = add_to_page_cache(cached_page, space,
					index, paf_mask);
		if (!err) {
			page = cached_page;
			cached_page = NULL;
		} else if (err == -EEXIST)
			goto repeat;
		else
			page = ERR_PTR(err);
	}

	if (cached_page)
		loosen_page_cache(cached_page);

	return page;
}

/**
 * 当回写缓存页面完成时调用
 */
void page_finish_writeback(struct page_frame *page)
{
	if (!test_clear_page_writeback(page))
			BUG();

	smp_mb__after_clear_bit();
	wake_up_page(page, PG_writeback);
}

/**
 * 等待页面读取完成
 */
static struct page_frame *wait_on_page_read(struct page_frame *page)
{
	if (!IS_ERR(page)) {
		wait_on_page_locked(page);
		if (!pgflag_uptodate(page)) {
			loosen_page_cache(page);
			page = ERR_PTR(-EIO);
		}
	}

	return page;
}

static struct page_frame *
__read_cache_page(struct file_cache_space *space,
	unsigned long index, void *data)
{
	struct page_frame *page;
	int err;

	/**
	 * 在页面缓存中查找面
	 */
	page = pgcache_find_alloc_lock(space, index,
		cache_space_get_allocflags(space) | __PAF_COLD);
	if (IS_ERR(page))
		return ERR_PTR(-ENOMEM);

	err = space->ops->readpage(data, page);
	if (err < 0) {
		loosen_page_cache(page);
		page = ERR_PTR(err);
	}
	else
		page = wait_on_page_read(page);

	return page;
}

/**
 * 将文件特定页面的内容读取到缓存中
 * space:		页所属的缓存空间
 * index:		所请求页的索引号
 * data:		传递给readpage回调函数的指针，通常为NULL
 */
struct page_frame *read_cache_page(struct file_cache_space *space,
				unsigned long index, void *data)
{
	struct page_frame *page;
	int err;

retry:
	page = __read_cache_page(space, index, data);
	if (IS_ERR(page))
		goto out;

	/**
	 * 用户将文件映射为可写
	 * 为防止别名将DCACHE数据刷新一下
	 */
	if (fnode_mapped_writeble(space))
		flush_dcache_page(page);

	mark_page_accessed(page);
	/**
	 * 查看页是否为最新
	 */
	if (pgflag_uptodate(page))
		goto out;

	/**
	 * 页不是最新的
	 * 锁住页面后调用readpage从磁盘读取页
	 */
	lock_page(page);
	/**
	 * 文件被载断了，重试
	 */
	if (!page->cache_space) {
		unlock_page(page);
		loosen_page_cache(page);
		goto retry;
	}

	if (pgflag_uptodate(page)) {
		unlock_page(page);
		goto out;
	}

	err = space->ops->readpage(data, page);
	if (err < 0) {
		loosen_page_cache(page);
		page = ERR_PTR(err);
	}

 out:
	return page;
}

/**
 * 将页中的数据复制到用户态缓冲区.
 */
static int read_fill_user(read_descriptor_t *desc, struct page_frame *page,
			unsigned long offset, unsigned long size)
{
	unsigned long left, count = desc->remain_count;
	char *kaddr;

	if (size > count)
		size = count;

	/**
	 * kmap为处于高端内存中的页建立永久的内核映射.
	 */
	kaddr = kmap(page);
	/**
	 * 复制页中的数据到用户态空间.
	 */
	left = copy_to_user(desc->arg.buf, kaddr + offset, size);
	/**
	 * 释放页的永久内核映射
	 */
	kunmap(page);

	if (left) {
		size -= left;
		desc->error = -EFAULT;
	}
	/**
	 * 更新desc的字段.
	 */
	desc->remain_count = count - size;
	desc->written += size;
	desc->arg.buf += size;

	return size;
}

/**
 * 从磁盘读入所请求的页
 * 并复制到用户态缓冲区
 */
static void
do_read(struct file_cache_space *space, struct file_ra_state *_ra,
	struct file *file, loff_t *ppos, read_descriptor_t *desc,
	read_actor_t actor)
{
	struct file_node *fnode = space->fnode;
	struct file_ra_state ra = *_ra;
	unsigned long end_index;
	unsigned long index;
	unsigned long offset;
	loff_t file_size;
	unsigned long bytes, ret;

	/**
	 * 第一个页面的索引号及偏移
	 */
	index = *ppos >> PAGE_CACHE_SHIFT;
	offset = *ppos & ~PAGE_CACHE_MASK;

	file_size = fnode_size(fnode);
	if (!file_size)
		goto out;

	end_index = (file_size - 1) >> PAGE_CACHE_SHIFT;
	/**
	 * 循环处理每一页
	 */
	do {
		struct page_frame *page;

		if (index > end_index)
			goto out;

		/**
		 * bytes是当前页面需要读取的字节数
		 */
		bytes = PAGE_CACHE_SIZE;
		if (index == end_index) {
			/**
			 * 超过了索引节点对象的文件大小字段,则从循环中退出.
			 */
			bytes = ((file_size - 1) & PAGE_OFFSET_MASK) + 1;
			if (bytes <= offset) {
				goto out;
			}
		}
		bytes = bytes - offset;

		page = read_cache_page(space, index, file);
		if (IS_ERR(page)) {
			desc->error = PTR_ERR(page);
			goto out;
		}

		/**
		 * 将数据复制给用户，返回值是成功复制的数量
		 */
		ret = actor(desc, page, offset, bytes);
		offset += ret;
		index += offset >> PAGE_CACHE_SHIFT;
		offset &= ~PAGE_CACHE_MASK;

		/**
		 * 减少页描述符的引用计数器.
		 */
		loosen_page_cache(page);
	/**
	 * 复制成功并且还有数据
	 */
	}while (ret == bytes && desc->remain_count);

out:
	*_ra = ra;

	/**
	 * 更新文件位置
	 */
	*ppos = ((loff_t) index << PAGE_CACHE_SHIFT) + offset;
	/**
	 * 把当前时间存放在文件索引节点的i_atime字段中,并把它标记为脏
	 */
	if (file)
		file_accessed(file);
}

/**
 * 所有文件系统实现同步和异步读操作使用的通用例程。
 * aio:		异步读写控制参数
 * io_seg:	描述等待接收数据的用户态缓冲区。
 * seg_count:	缓冲区数组长度。
 * ppos:		文件当前指针变量。
 */
static ssize_t __generic_file_aio_read(struct async_io_desc *aio,
	const struct io_segment *io_seg, unsigned long seg_count, loff_t *ppos)
{
	struct file *file = aio->file;
	unsigned long seg;
	size_t count;
	ssize_t ret = 0;

	count = 0;
	/**
	 * 调用access_ok来检查iovec描述符所描述的用户态缓冲区是否有效。
	 */
	for (seg = 0; seg < seg_count; seg++) {
		count += io_seg[seg].len;

		if ((count < 0 || io_seg[seg].len < 0))
			return -EINVAL;

		if (access_ok(VERIFY_WRITE, io_seg[seg].base, io_seg[seg].len))
			continue;

		return -EFAULT;
	}

	if (count == 0)
		return 0;

	/**
	 * 遍历处理每一个段
	 */
	for (seg = 0; seg < seg_count; seg++) {
		read_descriptor_t desc;

		if (io_seg[seg].len == 0)
			continue;

		desc.written = 0;
		desc.arg.buf = io_seg[seg].base;
		desc.remain_count = io_seg[seg].len;
		desc.error = 0;
		/**
		 * 从磁盘读入所请求的页并把它们拷贝到用户态缓冲区。
		 */
		do_read(file->cache_space,
			&file->readahead,
			file,
			ppos,
			&desc,
			read_fill_user);
		ret += desc.written;
		if (!ret) {
			ret = desc.error;
			break;
		}
	}

	return ret;
}

/**
 * read回调的实现
 * file:	文件对象的地址。
 * buf:	用户态线性区的线性地址。
 * count:	要读取的字符个数。
 * ppos:	指向一个变量的指针，该变量存放读操作开始处的文件偏移量。
 *		通常是pos字段。
 */
ssize_t
generic_file_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct io_segment io_seg = { .base = buf, .len = count };
	struct async_io_desc aio;
	ssize_t ret;

	init_async_io(&aio, file);
	ret = __generic_file_aio_read(&aio, &io_seg, 1, ppos);
	if (ret == -EIOCBQUEUED)
		ret = wait_on_async_io(&aio);

	return ret;
}

ssize_t generic_file_readv(struct file *file, const struct io_segment *io_seg,
			unsigned long seg_count, loff_t *ppos)
{
	struct async_io_desc aio;
	ssize_t ret;

	init_async_io(&aio, file);
	ret = __generic_file_aio_read(&aio, io_seg, seg_count, ppos);
	if (-EIOCBQUEUED == ret)
		ret = wait_on_async_io(&aio);

	return ret;
}

ssize_t
generic_file_aio_read(struct async_io_desc *aio, char __user *buf, size_t count, loff_t pos)
{
	struct io_segment io_seg = { .base = buf, .len = count };

	ASSERT(aio->pos == pos);

	return __generic_file_aio_read(aio, &io_seg, 1, &aio->pos);
}

/*
 * 对写操作进行一些简单的检查
 */
static int check_write(struct file *file, loff_t *pos, size_t *count, int isblk)
{
	struct file_node *inode = file->cache_space->fnode;
	unsigned long limit = RLIM_INFINITY;

	if (unlikely(*pos < 0))
                return -EINVAL;

	if (!isblk) {
		/**
		 * 如果不是块设备(即普通文件)
		 * 并且指定了O_APPEND，就将ppos设为文件尾。从而将新数据追加到文件的后面。
		 */
		if (file->flags & O_APPEND)
                        *pos = fnode_size(inode);

		/**
		 * 对文件大小进行检查。
		 * 此处是检查其限制值不能超过ulimit的限制。
		 */
		if (limit != RLIM_INFINITY) {
			if (*pos >= limit)
				return -EFBIG;

			if (*count > limit - (typeof(limit))*pos) {
				*count = limit - (typeof(limit))*pos;
			}
		}
	}

	/**
	 * 如果文件没有O_LARGEFILE标志，那么就不能超过2G。
	 */
	if (unlikely(*pos + *count > MAX_NON_LFS &&
				!(file->flags & O_LARGEFILE))) {
		if (*pos >= MAX_NON_LFS)
			return -EFBIG;

		if (*count > MAX_NON_LFS - (unsigned long)*pos)
			*count = MAX_NON_LFS - (unsigned long)*pos;
	}

	/**
	 * 不能把一个普通文件增大到超过文件系统的上限。
	 */
	if (likely(!isblk)) {
		if (unlikely(*pos >= inode->super->max_file_size)) {
			if (*count || *pos > inode->super->max_file_size)
				return -EFBIG;
		}

		if (unlikely(*pos + *count > inode->super->max_file_size))
			*count = inode->super->max_file_size - *pos;
	} else {
		loff_t file_size;

		if (blkdev_read_only(inode->blkdev))
			return -EPERM;

		file_size = fnode_size(inode);
		if (*pos >= file_size) {
			if (*count || *pos > file_size)
				return -ENOSPC;
		}

		if (*pos + *count > file_size)
			*count = file_size - *pos;
	}

	return 0;
}

static inline void
advance_segment(const struct io_segment **pseg,
	size_t *basep, size_t bytes)
{
	const struct io_segment *io_seg = *pseg;
	size_t base = *basep;

	while (bytes) {
		int len = min(bytes, io_seg->len - base);

		bytes -= len;
		base += len;
		if (io_seg->len == base) {
			io_seg++;
			base = 0;
		}
	}

	*pseg = io_seg;
	*basep = base;
}

/**
 * 从用户态复制数据到页缓存中
 */
static inline size_t
copy_user_buf(struct page_frame *page, unsigned long offset,
			const char __user *buf, unsigned bytes)
{
	char *kaddr;
	int left;

	kaddr = kmap(page);
	left = copy_from_user(kaddr + offset, buf, bytes);
	kunmap(page);

	return bytes - left;
}

static inline size_t
copy_user_bulk(struct page_frame *page, unsigned long offset,
			const struct io_segment *io_seg, size_t base, size_t bytes)
{
	char *kaddr;
	size_t ret;
	size_t copied = 0, left = 0;
	char *vaddr;

	kaddr = kmap(page);
	vaddr = kaddr + offset;

	while (bytes) {
		char __user *buf = io_seg->base + base;
		int copy = min(bytes, io_seg->len - base);

		base = 0;
		left = copy_from_user(vaddr, buf, copy);
		copied += copy;
		bytes -= copy;
		vaddr += copy;
		io_seg++;

		if (unlikely(left)) {
			/**
			 * 复制失败了，将剩余的字节清0
			 */
			if (bytes)
				memset(vaddr, 0, bytes);
			break;
		}
	}
	ret = copied - left;
	kunmap(page);

	return ret;
}

int __writeback_submit_data(struct file_cache_space *space, struct writeback_control *control)
{
	if (control->remain_page_count <= 0)
		return 0;

	/**
	 * 块设备的回调函数是writepages_journal
	 */
	if (space->ops->writepages)
		return space->ops->writepages(space, control);

	return writepages_journal(space, control);
}

/**
 * 回写文件节点的数据到磁盘
 */
int writeback_submit_data(struct file_cache_space *space,
	loff_t start, loff_t end, int sync_mode)
{
	int ret;

	struct writeback_control control = {
		.sync_mode = sync_mode,
		.remain_page_count = space->page_count * 2,
		.start = start,
		.end = end,
	};

	/**
	 * 内存设备，没有必要回写
	 */
	if (space->blkdev_infrast->mem_device)
		return 0;

	ret = __writeback_submit_data(space, &control);

	return ret;
}

/**
 * 等待缓存页面回写完毕
 */
static int writeback_wait_data_range(struct file_cache_space *space,
				pgoff_t start, pgoff_t end)
{
	struct pagevec pvec;
	int page_count;
	pgoff_t index;
	int ret = 0;

	if (end < start)
		return 0;

	pagevec_init(&pvec, 0);
	index = start;
	while (index <= end) {
		unsigned i;

		page_count = pgcache_collect_pages_tag(&pvec, space, &index, PAGECACHE_TAG_WRITEBACK,
					min(end - index + 1, (pgoff_t)PAGEVEC_SIZE));

		if (page_count <= 0)
			break;

		for (i = 0; i < page_count; i++) {
			struct page_frame *page = pvec.pages[i];

			if (page->index > end)
				continue;

			wait_on_page_writeback(page);
			if (pgflag_error(page))
				ret = -EIO;
		}

		pagevec_release(&pvec);
	}

	if (atomic_test_and_clear_bit(__CS_ENOSPC, &space->flags))
		ret = -ENOSPC;
	if (atomic_test_and_clear_bit(__CS_EIO, &space->flags))
		ret = -EIO;

	return ret;
}

/**
 * 等待文件回写完成
 */
int writeback_wait_data(struct file_cache_space *space)
{
	loff_t file_size = fnode_size(space->fnode);

	if (file_size == 0)
		return 0;

	return writeback_wait_data_range(space, 0,
				(file_size - 1) >> PAGE_CACHE_SHIFT);
}

/* 回写文件 */
static int writeback_submit_wait_data(struct file_cache_space *space)
{
	int ret = 0;

	if (space->page_count) {/* 文件长度不为0 */
		/* 回写数据 */
		ret = writeback_submit_data(space, 0, 0, WB_SYNC_WAIT);
		if (ret == 0)
			/* 等待回写完成 */
			ret = writeback_wait_data(space);
	}
	return ret;
}

/**
 * 将特定文件节点的脏数据同步到磁盘
 * 文件写操作可以调用此函数刷新数据到磁盘
 *
 *    OSYNC_DATA:     刷新脏数据
 *    OSYNC_METADATA: 刷新数据相关的元数据，如间接块
 *    OSYNC_INODE:    刷新文件节点数据
 *
 */
static int sync_file_node(struct file_node *fnode,
	struct file_cache_space *space, int what)
{
	int err = 0;
	int need_write_inode_now = 0;
	int err2;

	current->flags |= TASKFLAG_SYNCWRITE;
	if (what & OSYNC_DATA) {
		err = writeback_submit_data(space, 0, 0, WB_SYNC_WAIT);
		err2 = writeback_wait_data(space);
		if (!err)
			err = err2;
	}
	current->flags &= ~TASKFLAG_SYNCWRITE;

	smp_lock(&filenode_lock);
	if ((fnode->state & FNODE_DIRTY) &&
	    ((what & OSYNC_INODE) || (fnode->state & FNODE_DIRTY_DATASYNC)))
		need_write_inode_now = 1;
	smp_unlock(&filenode_lock);

	if (need_write_inode_now) {
		err2 = writeback_fnode_sumbit_wait(fnode, 1);
		if (!err)
			err = err2;
	}
	else
		writeback_fnode_wait(fnode);

	return err;
}

/*
 * 将特定范围内的脏页回写到磁盘
 */
static int sync_page_data(struct file_node *fnode, struct file_cache_space *space,
			loff_t pos, size_t count, bool lock)
{
	pgoff_t start = pos >> PAGE_CACHE_SHIFT;
	pgoff_t end = (pos + count - 1) >> PAGE_CACHE_SHIFT;
	int ret;

	if (space->blkdev_infrast->mem_device || !count)
		return 0;
	/**
	 * 调用cache_space对象的writepages方法或者mpage_writepages函数
	 * 来开始脏页的传输。
	 */
	ret = writeback_submit_data(space, pos, pos + count - 1, WB_SYNC_WAIT);
	if (ret == 0) {
		if (lock)
			down(&fnode->sem);

		/**
		 * 将数据相关的元数据，如间接块刷新到磁盘。
		 */
		ret = sync_file_node(fnode, space, OSYNC_METADATA);

		if (lock)
			up(&fnode->sem);
	}

	/**
	 * 挂起当前进程一直到全部所刷新页的PG_writeback标志清0.
	 */
	if (ret == 0)
		ret = writeback_wait_data_range(space, start, end);

	return ret;
}

static ssize_t
cached_write(struct async_io_desc *aio, const struct io_segment *io_seg,
		unsigned long seg_count, loff_t pos, loff_t *ppos,
		size_t count, ssize_t written)
{
	const struct io_segment *cur_iov;
	struct file_cache_space *space;
	struct cache_space_ops *ops;
	struct file *file = aio->file;
	struct page_frame	*page;
	struct file_node *fnode;
	size_t base = 0;
	char __user *buf;
	long status = 0;
	size_t bytes;

	space = file->cache_space;
	ops = space->ops;
	fnode = space->fnode;
	cur_iov = io_seg;

	advance_segment(&cur_iov, &base, written);
	buf = io_seg->base + base;

	/**
	 * 循环处理每一页。
	 */
	do {
		unsigned long index;
		unsigned long offset;
		size_t copied;

		offset = (pos & (PAGE_CACHE_SIZE -1));
		index = pos >> PAGE_CACHE_SHIFT;
		bytes = PAGE_CACHE_SIZE - offset;
		if (bytes > count)
			bytes = count;

		/**
		 * 在缓存页面中查找特定页面
		 * 如果没有就分配一个
		 */
		page = pgcache_find_alloc_lock(space, index,
			cache_space_get_allocflags(space));
		if (IS_ERR(page)) {
			status = PTR_ERR(page);
			break;
		}

		/**
		 * 调用索引节点的prepare_write
		 * 分配块描述符，并在磁盘上分配块
		 */
		status = ops->prepare_write(file, page, offset, offset + bytes);
		if (unlikely(status)) {
			loff_t fsize = fnode_size(fnode);

			unlock_page(page);
			loosen_page_cache(page);
			/**
			 * 写入不成功，强制截断文件
			 */
			if (pos + bytes > fsize)
				vmtruncate(fnode, fsize);
			break;
		}

		/**
		 * 复制用户数据到页面缓存中
		 */
		if (likely(seg_count == 1))
			copied = copy_user_buf(page, offset,
							buf, bytes);
		else
			copied = copy_user_bulk(page, offset,
						cur_iov, base, bytes);
		flush_dcache_page(page);
	
		/**
		 * 写完一页，将其标记为脏
		 */
		status = ops->commit_write(file, page, offset, offset+bytes);
		if (likely(copied > 0)) {
			if (!status)
				status = copied;

			if (status >= 0) {
				written += status;
				count -= status;
				pos += status;
				buf += status;
				advance_segment(&cur_iov, &base, status);
			}
		}
		if (unlikely(copied != bytes))
			if (status >= 0)
				status = -EFAULT;

		/**
		 * 页面已经写入完毕，解锁
		 */
		unlock_page(page);
		/**
		 * 设置页面访问标志。这为内存回收算法所使用。
		 */
		mark_page_accessed(page);
		/**
		 * 减少页引用计数。
		 */
		loosen_page_cache(page);

		if (status < 0)
			break;

		/**
		 * 检查页调整缓存中脏页比例是否超过一个固定的阀值
		 * 一般为系统中页的40%
		 * 如果这样，则调用writeback_inodes来刷新几十页到磁盘。
		 */
		balance_dirty_pages_ratelimited(space);
	} while (count);
	*ppos = pos;

	/**
	 * 成功写入缓存，进行文件同步处理
	 */
	if (likely(status >= 0)) {
		if (file_is_sync(file)) {
			if (!ops->writepage || !is_sync_kiocb(aio))
				status = sync_file_node(fnode, space,
						OSYNC_METADATA | OSYNC_DATA);
		}
  	}

	/**
	 * 回写数据并等待数据回写完毕
	 */
	if (unlikely(file->flags & O_DIRECT) && written)
		status = writeback_submit_wait_data(space);

	return written ? written : status;
}

static ssize_t
__aio_write(struct async_io_desc *aio, const struct io_segment *io_seg,
				unsigned long seg_count, loff_t *ppos)
{
	struct file_cache_space *space;
	struct file_node *fnode;
	unsigned long	seg;
	struct file *file;
	ssize_t written;
	size_t count;
	ssize_t err;
	loff_t pos;

	file = aio->file;
	space = file->cache_space;
	fnode = space->fnode;

	count = 0;
	for (seg = 0; seg < seg_count; seg++) {
		count += io_seg[seg].len;
		/**
		 * 不能整形溢出，单个请求也不能是负数
		 */
		if ((count < 0) || (io_seg[seg].len < 0))
			return -EINVAL;
	}

	pos = *ppos;
	written = 0;

	/**
	 * 一些常规检查。
	 */
	err = check_write(file, &pos, &count, S_ISBLK(fnode->mode));
	if (err)
		goto out;

	if (count == 0)
		goto out;

	/**
	 * 更新文件时间
	 */
	fnode_update_time(fnode, 1);

	/**
	 * 循环处理，以更新写操作中的所有文件页。
	 */
	written = cached_write(aio, io_seg, seg_count,
			pos, ppos, count, written);
out:
	return written ? written : err;
}

static ssize_t common_file_write(struct file *file, const struct io_segment *io_seg,
				unsigned long seg_count, loff_t *ppos, bool lock, bool wait)
{
	struct file_cache_space *space = file->cache_space;
	struct file_node *fnode = space->fnode;
	struct async_io_desc aio;
	loff_t pos = *ppos;
	ssize_t ret;

	init_async_io(&aio, file);

	if (lock)
		down(&fnode->sem);

	ret = __aio_write(&aio, io_seg, seg_count, ppos);
	if (wait && (ret == -EIOCBQUEUED))
		ret = wait_on_async_io(&aio);

	if (lock)
		up(&fnode->sem);

	if (ret > 0 && file_is_sync(file)) {
		int err;

		err = sync_page_data(fnode, space, pos, ret, 0);
		if (err < 0)
			ret = err;
	}

	return ret;
}

ssize_t __generic_file_aio_write(struct async_io_desc *aio,
	const struct io_segment *io_seg, unsigned long seg_count, loff_t *ppos)
{
	struct file *file = aio->file;

	return common_file_write(file, io_seg, 1, ppos, 0, 0);
}

ssize_t generic_file_aio_write(struct async_io_desc *aio, 
	const char __user *buf, size_t count, loff_t pos)
{
	struct io_segment io_seg = { .base = (void __user *)buf,
					.len = count };
	struct file *file = aio->file;

	ASSERT(aio->pos == pos);

	return common_file_write(file, &io_seg, 1, &aio->pos, 1, 0);
}

/**
 * 块设备文件的write实现
 * 无锁
 */
ssize_t
__generic_file_write(struct file *file, const struct io_segment *io_seg,
				unsigned long seg_count, loff_t *ppos)
{
	return common_file_write(file, io_seg, seg_count, ppos, 0, 1);
}

/**
 * 文件系统默认的writev实现
 */
ssize_t generic_file_writev(struct file *file, const struct io_segment *io_seg,
			unsigned long seg_count, loff_t *ppos)
{
	return common_file_write(file, io_seg, seg_count, ppos, 1, 1);
}

/**
 * 许多文件系统(如Ext2和JFS、ramfs)通过本函数来实现文件对象的write方法。
 * ext3则间接调用generic_file_aio_write实现write调用
 * file:		文件对象指针
 * buf:		用户态地址空间中的地址。
 * count:		要写入的字符个数。
 * ppos:		存放文件偏移量的变量地址，必须从这个偏移量处开始写入。
 */
ssize_t generic_file_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	struct io_segment io_seg = { .base = (void __user *)buf, .len = count };

	return generic_file_writev(file, &io_seg, 1, ppos);
}

int generic_file_mmap(struct file * file, struct vm_area_struct * vma)
{
	/* TO-DO */
	return -ENOSYS;
}

ssize_t generic_file_sendfile(struct file *in_file, loff_t *ppos,
			 size_t count, read_actor_t actor, void *target)
{
	/* TO-DO */
	return -ENOSYS;
}

void init_pagecache(void)
{
	approximate_counter_init(&pagecache_count);
}
