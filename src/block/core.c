#include <dim-sum/beehive.h>
#include <dim-sum/blkio.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/disk.h>
#include <dim-sum/fs.h>
#include <dim-sum/irq.h>
#include <dim-sum/pagemap.h>
#include <dim-sum/sched.h>
#include <dim-sum/stacktrace.h>
#include <dim-sum/timer.h>

static struct beehive_allotter *request_allotter;
static struct beehive_allotter *queue_allotter;

static struct workqueue_struct *block_workqueue; 

/**
 * 一些设备无法处理那些跨越特定大小内存边界的请求。
 * 此函数告诉内存设备的特定边界。
 * 如果设备不能处理跨越4MB边界的请求，则mask的值为0x3fffff。
 * 默认的mask是0xffffffff。
 */
void blk_set_request_boundary(struct blk_request_queue *queue, unsigned long mask)
{
	if (mask < PAGE_CACHE_SIZE - 1) {
		mask = PAGE_CACHE_SIZE - 1;
		printk(KERN_INFO "%s: set to minimum %lx\n", __FUNCTION__, mask);
	}

	queue->request_settings.addr_boundary = mask;
}

void blk_set_request_max_sectors(struct blk_request_queue *queue, unsigned long max_sectors)
{
	int min_sectors = (PAGE_CACHE_SIZE >> 9);

	if (max_sectors < min_sectors) {
		max_sectors = min_sectors;
		printk("%s: set to minimum %d\n", __FUNCTION__, max_sectors);
	}

	queue->request_settings.max_sectors = max_sectors;
	queue->request_settings.max_hw_sectors = max_sectors;
}

void blk_set_request_sector_size(struct blk_request_queue *queue, unsigned short size)
{
	queue->request_settings.sector_size = size;
}

void blk_set_request_max_hw_pages(struct blk_request_queue *queue, unsigned short max_segment_count)
{
	if (!max_segment_count) {
		max_segment_count = 1;
		printk("%s: set to minimum %d\n", __FUNCTION__, max_segment_count);
	}

	queue->request_settings.max_hw_segment_count = max_segment_count;
}

void blk_set_request_max_size(struct blk_request_queue *queue, unsigned int max_size)
{
	if (max_size < PAGE_CACHE_SIZE) {
		max_size = PAGE_CACHE_SIZE;
		printk("%s: set to minimum %d\n", __FUNCTION__, max_size);
	}

	queue->request_settings.max_size = max_size;
}

static void blk_set_request_dma_alignment(struct blk_request_queue *queue, int mask)
{
	queue->request_settings.dma_alignment = mask;
}

/**
 * 告诉内核驱动程序执行DMA所使用的最高物理内存。
 * 可以把任何可行的物理地址作为参数，或者使用如下预定义的参数。
 *		BLKDEV_LIMIT_HIGH:		对高端内存页使用回弹缓冲区。
 *		BLKDEV_LIMIT_ANY:		可以在任何地址执行DMA。
 * 默认值是BLKDEV_LIMIT_HIGH。
 */
void blk_set_dma_limit(struct blk_request_queue *q, u64 dma_addr)
{
	unsigned long pgnum = dma_addr >> PAGE_SHIFT;

	if (pgnum <= max_dma_pgnum) {
		q->alloc_flags = PAF_NOIO | PAF_DMA;
	} else
		q->alloc_flags = PAF_NOIO;

	q->max_pgnum = pgnum;
}

/**
 * 为读或者写请求获得一个请求描述符配额
 */
static bool
__consume_request_quota(struct blk_request_queue *queue, bool write)
{
	ASSERT(queue->request_pools[write].request_count >= 0);
	if (queue->request_pools[write].request_count < queue->max_requests) {
		queue->request_pools[write].request_count++;
		return true;
	}

	return false;
}

/**
 * 当释放请求描述符时，更新请求描述符计数
 * 并且唤醒等待任务。
 */
static void
__produce_request_quota(struct blk_request_queue *queue, bool write)
{
	ASSERT(queue->request_pools[write].request_count > 0);
	queue->request_pools[write].request_count--;

	if (queue->request_pools[write].request_count < queue->max_requests) {
		smp_mb();
		if (waitqueue_active(&queue->request_pools[write].wait))
			wake_up(&queue->request_pools[write].wait);
	}

	if (!queue->request_pools[READ].request_count && !queue->request_pools[WRITE].request_count) {
		smp_mb();
		if (unlikely(waitqueue_active(&queue->wait_drain)))
			wake_up(&queue->wait_drain);
	}
}

/**
 * 当硬件完成一些BIO请求后
 * 更新请求描述符中的计数值
 */
static void blk_recalc_request(struct blk_request *req, int sector_count)
{
	struct blk_request_queue *queue = req->queue;
	struct block_io_desc *bio;
	int segments;

	if (req->flags & BLKREQ_DATA) {
		req->remain_sector_start += sector_count;
		req->remain_sector_count -= sector_count;

		if ((req->sector_count >= req->remain_sector_count) &&
		    (req->start_sector <= req->remain_sector_start)) {
			req->start_sector = req->remain_sector_start;
			req->sector_count = req->remain_sector_count;
			req->sectors_seg = bio_fire_sectors(req->bio_head);
			req->sectors_seg_drv = req->sectors_seg;
			req->buffer = bio_data(req->bio_head);
		}

		ASSERT(req->sector_count >= req->sectors_seg);
	}

	segments = 0;
	if (req->bio_head) {
		rq_for_each_bio(bio, req) {
			__clear_bit(__BIOFLAG_RECOUNT, &bio->bi_flags);
			segments += blkio_hw_segments(queue, bio);
		}
	}
	req->segcount_hw = segments;
}

void blk_hold_queue(struct blk_request_queue *queue)
{
	if (queue)
		hold_object(&queue->object);
}

void blk_loosen_queue(struct blk_request_queue *queue)
{
	if (queue)
		loosen_object(&queue->object);
}

static void kblockd_flush(void)
{
	flush_workqueue(block_workqueue);
}

void blk_sync_queue(struct blk_request_queue *queue)
{
	synchronize_timer_del(&queue->push_timer);
	kblockd_flush();
}

/**
 * 当队列引用计数变为0时，调用此函数执行清理工作。
 */
static void __release_queue(struct object *obj)
{
	struct blk_request_queue *queue = 
		container_of(obj, struct blk_request_queue, object);

	if (queue->scheduler)
		iosched_exit(queue->scheduler);

	blk_sync_queue(queue);

	beehive_free(queue_allotter, queue);
}

/**
 * 释放请求描述符
 */
static void blk_free_request(struct blk_request_queue *queue, struct blk_request *req)
{
	iosched_uninit_request(queue, req);
	beehive_free(request_allotter, req);
}

/**
 * 释放请求描述符的引用计数
 */
static void blk_loosen_request(struct blk_request_queue *queue, struct blk_request *req)
{
	if (unlikely(!queue))
		return;

	if (unlikely(--req->ref_count))
		return;

	if (req->queue) {
		bool write= blk_request_is_write(req);

		iosched_request_release(queue, req);
		BUG_ON(!list_is_empty(&req->list));
		__produce_request_quota(queue, write);
	}

	/**
	 * 释放请求描述符
	 */
	blk_free_request(queue, req);
}

/**
 * blk_update_request用于块设备驱动的中断处理程序。
 * 当设备完成一个IO请求的部分或者全部扇区时，调用此函数通知块设备子系统。
 * nr_sectors：DMA传送的扇区数
 * uptodate：传送成功的标志
 */
int blk_update_request(struct blk_request *req, int uptodate, int sector_count)
{
	int remain_bytes = (sector_count << 9);
	int total_bytes = 0;
	int bytes_bio = 0;
	int next_idx = 0;
	struct block_io_desc *bio;
	int slice_bytes;
	int error = 0;

	if (end_io_error(uptodate))
		error = !uptodate ? -EIO : uptodate;

	if (!uptodate) {
		if ((req->flags & BLKREQ_DATA) && (req->flags & BLKREQ_VERBOSE))
			printk(KERN_INFO "I/O error, dev %s, sector %llu\n",
				req->disk ? req->disk->name : "?",
				(u64)req->start_sector);
	}

	/**
	 * 扫描请求中的BIO结构及每个BIO字段。
	 */
	while ((bio = req->bio_head) && (remain_bytes >= bio->remain_size)) {
		/**
		 * 修改BIO字段，使其指向请求中下一个未完成的BIO字段。
		 */
		req->bio_head = bio->bi_next;
		slice_bytes = bio->remain_size;
		/**
		 * 通知文件系统，IO已经完成
		 */
		blkio_finished(bio, slice_bytes, error);

		total_bytes += slice_bytes;
		remain_bytes -= slice_bytes;
	}

	bio = req->bio_head;
	/**
	 * 数据已经传送完，返回0
	 */
	if (!bio)
		return 0;

	if (remain_bytes == 0) {
		blk_recalc_request(req, total_bytes >> 9);
		return 1;
	}

	ASSERT(remain_bytes <= bio->remain_size);
	while (remain_bytes > 0) {
		int idx = bio->bi_idx + next_idx;

		ASSERT(idx < bio->item_count);
		slice_bytes = bio_iovec_idx(bio, idx)->length;

		/*
		 * 当前bio的当前段还没有完成
		 */
		if (unlikely(slice_bytes > remain_bytes)) {
			bytes_bio += remain_bytes;
			total_bytes += remain_bytes;
			break;
		}

		next_idx++;
		bytes_bio += slice_bytes;
		total_bytes += slice_bytes;
		remain_bytes -= slice_bytes;

		/**
		 * 一定是出现了什么异常才会出现这种情况
		 */
		if (next_idx >= bio->item_count - 1)
			break;
	};


	/**
	 * 在已经完成数据传送的BIO结构上调用bio_endio函数。
	 */
	blkio_finished(bio, bytes_bio, error);
	/**
	 * 修改未完成BIO结构的bi_idx字段，使其指向第一个未完成的段。
	 */
	bio->bi_idx += next_idx;
	/**
	 * 修改未完成段的bv_offset和bv_len两个字段，使其指向仍需传递的数据。
	 */
	bio_iovec(bio)->bv_offset += remain_bytes;
	bio_iovec(bio)->length -= remain_bytes;

	blk_recalc_request(req, total_bytes >> 9);

	/**
	 * 数据还没有传送完，返回1。
	 */
	return 1;
}

/**
 * 当整个请求全部结束后调用此函数
 */
void blk_finish_request(struct blk_request *req)
{
	struct disk_device *disk = req->disk;
	struct semaphore *waiting = req->waiting;

	if (disk && (req->flags & BLKREQ_DATA))
		disk->request_count--;

	/**
	 * 释放引用计数
	 */
	blk_loosen_request(req->queue, req);

	/**
	 * 唤醒等待此请求的任务
	 */
	if (waiting)
		up(waiting);
}

/**
 * 当驱动完成当前请求时调用此函数
 */
void blk_end_request(struct blk_request *req, int uptodate)
{
	if (!blk_update_request(req, uptodate, req->sectors_seg)) {
		blkdev_dequeue_request(req);
		blk_finish_request(req);
	}
}

/**
 * 禁止attach队列
 */
int blk_disable_attach(struct blk_request_queue *queue)
{
	/**
	 * 清除ATTACHED标志。
	 * 如果没有attach则直接退出。
	 */
	if (!atomic_test_and_clear_bit(__BLKQUEUE_ATTACHED, &queue->state))
		return 0;

	/**
	 * 清除push_timer动态定时器的执行。
	 */
	timer_remove(&queue->push_timer);

	return 1;
}

/**
 * 将队列中的请求推送到驱动设备
 */
void __blk_generic_push_queue(struct blk_request_queue *queue)
{
	/**
	 * 首先检测块设备是否仍然活跃
	 */
	if (test_bit(__BLKQUEUE_STOPPED, &queue->state))
		return;

	/**
	 * 禁止attach队列
	 */
	if (!blk_disable_attach(queue))
		return;

	/**
	 * 队列中有请求
	 */
	if (iosched_get_first_request(queue))
		/**
		 * 执行engorge_queue来处理处理掉所有请求
		 */
		queue->engorge_queue(queue);
}

/**
 * 将队列中的请求推送到驱动设备
 */
void blk_generic_push_queue(struct blk_request_queue *queue)
{
	smp_lock_irq(queue->queue_lock);
	__blk_generic_push_queue(queue);
	smp_unlock_irq(queue->queue_lock);
}

/**
 * 请求队列的unplug_work实现函数。
 */
static void blk_push_work(void *data)
{
	struct blk_request_queue *queue = data;

	/**
	 * 通常push_queue函数是由blk_generic_push_queue函数实现的。
	 * 它的功能是将队列中的请求推送到驱动设备
	 */
	queue->push_queue(queue);
}

int kblockd_schedule_work(struct work_struct *work)
{
	return queue_work(block_workqueue, work);
}

/**
 * 定期向设备推送请求的定时器回调函数
 */
static int blk_push_timeout(void *data)
{
	struct blk_request_queue *queue = (struct blk_request_queue *)data;

	/**
	 * kblockd执行blk_unplug_work函数，这个函数存放在q->unplug_work中。
	 * 该函数会调用请求队列中的q->unplug_fn方法，通常该方法是由generic_unplug_device函数实现的。
	 * generic_unplug_device函数的功能是拨出块设备：首先检查请求队列是否仍然活跃。
	 * 然后，调用blk_remove_plug函数。最后，执行策略例程request_fn来开始处理请求队列中的下一个请求。
	 */
	kblockd_schedule_work(&queue->push_work);

	return 0;
}

/**
 * 将队列与其设备绑定
 */
void blk_attach_device(struct blk_request_queue *queue)
{
	/**
	 * 驱动已经禁止接受请求
	 * 不要再向驱动push请求了。
	 * 除非驱动再次调用blkdev_start_quest()。
	 */
	if (test_bit(__BLKQUEUE_STOPPED, &queue->state))
		return;

	/**
	 * 设置ATTACHED标志
	 * 然后重启push_timer定时器，定期推送请求
	 */
	if (!atomic_test_and_set_bit(__BLKQUEUE_ATTACHED, &queue->state))
		timer_rejoin(&queue->push_timer, jiffies + queue->push_delay);
}

/**
 * 检查当前正在使用的IO调度程序是否可以被动态取代。
 * 如果可以，则让当前进程睡眠直到启动一个新的IO调度程序。
 */
static void blk_wait_queue_ready(struct blk_request_queue *queue)
{
	DEFINE_WAIT(wait);

	while (queue->state & BLKQUEUE_DRAINING) {
		prepare_to_wait_exclusive(&queue->wait_drain, &wait,
				TASK_UNINTERRUPTIBLE);
		smp_unlock_irq(queue->queue_lock);	
		io_schedule();
		smp_lock_irq(queue->queue_lock);
		finish_wait(&queue->wait_drain, &wait);
	}
}

/**
 * 从beehive内存分配器中分配内存
 */
static struct blk_request *
alloc_request(struct blk_request_queue *queue, int alloc_flags)
{
	struct blk_request *req = beehive_alloc(request_allotter, alloc_flags);

	if (!req)
		return NULL;

	if (!iosched_init_request(queue, req, alloc_flags))
		return req;

	kfree(req);

	return NULL;
}

/**
 * 从磁盘队列的内存池中分配一个请求描述符
 */
static struct blk_request *get_request(struct blk_request_queue *queue, bool write, int alloc_flags)
{
	struct blk_request *req = NULL;
	int may_queue;

	smp_lock_irq(queue->queue_lock);
	/**
	 * 看看调度器是否允许分配新的请求描述符
	 * 一般是可以的
	 */
	may_queue = iosched_may_queue(queue, write);
	if (may_queue == IOSCHED_NO_QUEUE) {
		smp_unlock_irq(queue->queue_lock);
		goto out;
	}

	/**
	 * 获得内存配额失败则退出
	 */
	if (!__consume_request_quota(queue, write)) {
		smp_unlock_irq(queue->queue_lock);
		goto out;
	}
	smp_unlock_irq(queue->queue_lock);

	req = alloc_request(queue, alloc_flags);
	/**
	 * 虽然很难出现内存紧张导致的失败，但是不能完全排除
	 */
	if (!req) {
		/**
		 * 归还配额
		 */
		smp_lock_irq(queue->queue_lock);
		__produce_request_quota(queue, write);
		smp_unlock_irq(queue->queue_lock);
		goto out;
	}

	list_init(&req->list);
	req->ref_count = 1;
	req->queue = queue;
	if (write)
		req->flags |= BLKQUEUE_WRITE;
out:
	return req;
}

/**
 * 当前队列没有可用内存
 * 推送请求并等待内存可用
 */
static struct blk_request *
get_request_wait(struct blk_request_queue *queue, bool write)
{
	struct blk_request *req;
	DEFINE_WAIT(wait);

	/**
	 * 向驱动推送磁盘请求
	 */
	blk_generic_push_queue(queue);

	do {
		/**
		 * 将任务挂到磁盘队列的等待队列中
		 */
		prepare_to_wait_exclusive(&queue->request_pools[write].wait, &wait,
				TASK_UNINTERRUPTIBLE);
		io_schedule();
		finish_wait(&queue->request_pools[write].wait, &wait);

		/**
		 * 再次试图分配请求描述符
		 */
		req = get_request(queue, write, PAF_NOIO | __PAF_ZERO);
	} while (!req);

	return req;
}

/**
 * 新请求到达时，分配一个请求描述符
 */
static struct blk_request *
grab_request(struct blk_request_queue *queue, bool write, bool wait)
{
	struct blk_request *ret = NULL;

	/**
	 * 分配一个新请求
	 */
	if ((ret = get_request(queue, write, PAF_ATOMIC | __PAF_ZERO)) == NULL) {
		/**
		 * 不能直接分配一个请求，那么等待内存可用，并分配一个请求描述短信超人 。
		 */
		if (!ret && wait)
			ret = get_request_wait(queue, write);
	}

	return ret;
}

/**
 * 请求队列的默认合并回调函数
 */
static int merge_requests(struct blk_request_queue *queue,
	struct blk_request *req, struct blk_request *next)
{
	int seg_count = req->segcount_hw + next->segcount_hw;

	if (req->special || next->special)
		return 0;

	/**
	 * 太大了，不能合并
	 */
	if ((req->sector_count + next->sector_count) > queue->request_settings.max_sectors)
		return 0;

	if (seg_count > queue->request_settings.max_hw_segment_count)
		return 0;

	/**
	 * 可以合并，更新段计数
	 */
	req->segcount_hw = seg_count;

	return 1;
}

static int try_merge_requests(struct blk_request_queue *queue,
	struct blk_request *req, struct blk_request *next)
{
	if (!rq_mergeable(req) || !rq_mergeable(next))
		return 0;

	/**
	 * 两个请求的扇区不连续
	 */
	if (req->start_sector + req->sector_count != next->start_sector)
		return 0;

	if (blk_request_is_write(req) != blk_request_is_write(next)
	    || req->disk != next->disk
	    || next->waiting || next->special)
		return 0;

	/**
	 * 由队列的回调函数来合并请求
	 * 更新相应的计数
	 */
	if (!queue->merge_requests(queue, req, next))
		return 0;

	/*
	 * 更新请求开始时间，dead-line算法会使用
	 */
	if (time_after(req->start_time, next->start_time))
		req->start_time = next->start_time;

	req->bio_tail->bi_next = next->bio_head;
	req->bio_tail = next->bio_tail;
	req->sector_count = req->remain_sector_count += next->remain_sector_count;

	iosched_merge_requests(queue, req, next);

	if (req->disk)
		req->disk->request_count--;

	blk_loosen_request(queue, next);

	return 1;
}

static int merge_bio_head(struct blk_request_queue *queue, struct blk_request *req, 
			     struct block_io_desc *bio)
{
	 sector_t start_sector = bio->start_sector;
	int fire_sectors = bio_fire_sectors(bio);
	int nr_hw_segs = blkio_hw_segments(queue, bio);
	struct blk_request *prev = iosched_get_front_request(queue, req);

	if ((req->segcount_hw + nr_hw_segs > queue->request_settings.max_hw_segment_count)
	    || (req->sector_count + bio_sectors(bio) > queue->request_settings.max_sectors)) {
		req->flags |= BLKREQ_NOMERGE;
		if (req == queue->preferred_merge)
			queue->preferred_merge = NULL;

		return 0;
	}

	/**
	 * 合并到该bio的前面
	 */
	bio->bi_next = req->bio_head;
	req->bio_head = bio;
	req->buffer = bio_data(bio);
	req->sectors_seg_drv = fire_sectors;
	req->sectors_seg = fire_sectors;
	req->start_sector = req->remain_sector_start = start_sector;
	req->sector_count = req->remain_sector_count += bio_sectors(bio);
	req->segcount_hw += nr_hw_segs;

	/**
	 * 检查是否可以与上面的bio进行进一步的合并。
	 */
	if (prev && try_merge_requests(queue, prev, req))
		iosched_merge_post(queue, req);

	return 1;
}

/**
 * 将BIO请求插入到请求尾部
 */
static int merge_bio_tail(struct blk_request_queue *queue,
	struct blk_request *req, struct block_io_desc *bio)
{
	int nr_hw_segs = blkio_hw_segments(queue, bio);
	struct blk_request *next = iosched_get_behind_request(queue, req);

	/**
	 * 合并在后面会超过限制
	 */
	if ((req->sector_count + bio_sectors(bio) > queue->request_settings.max_sectors)
	    || (req->segcount_hw + nr_hw_segs > queue->request_settings.max_hw_segment_count)) {
		req->flags |= BLKREQ_NOMERGE;
		if (req == queue->preferred_merge)
			queue->preferred_merge = NULL;

		return 0;
	}

	/**
	 * 将该请求合并到bio的末尾。
	 */
	req->bio_tail->bi_next = bio;
	req->bio_tail = bio;
	req->sector_count = req->remain_sector_count += bio_sectors(bio);
	req->segcount_hw += nr_hw_segs;

	/**
	 * 检查是否可以与后面的请求合并。
	 */
	if (next && try_merge_requests(queue, req, next))
		iosched_merge_post(queue, req);

	return 1;
}

/**
 * 如果驱动不特别指定
 * 通用块层调用此函数，获得IO调度层的服务。
 */
static int
blk_generic_sumit_request(struct blk_request_queue *queue, struct block_io_desc *bio)
{
	sector_t start_sector = bio->start_sector;
	int req_sectors = bio_sectors(bio);
	bool write = bio_is_write(bio);
	bool barrier = bio_is_barrier(bio);
	int fire_sectors = bio_fire_sectors(bio);
	struct blk_request *req= NULL;
	struct blk_request *new_req = NULL;
	/**
	 * 在内存紧张时，预读请求可以不用等待
	 */
	bool wait = !bio_rw_ahead(bio);
	int merge_pos;
	int err = -EWOULDBLOCK;

	if (barrier) {
		new_req = grab_request(queue, write, wait);
		if (!new_req)
			goto finished;
		else {
			req->flags |= (BLKREQ_BARRIER | BLKREQ_NOMERGE);
			goto new;
		}
	}

try:
	smp_lock_irq(queue->queue_lock);

	/**
	 * 等待IO调度就绪。
	 */
	blk_wait_queue_ready(queue);

	/**
	 * 检查请求队列中是否存在待处理请求。
	 */
	if (iosched_is_empty(queue)) {
		blk_attach_device(queue);
		goto no_merge;
	} 

	merge_pos = iosched_get_merge_pos(queue, &req, bio);

	/**
	 * 请求队列中包含待处理请求。调用elv_merge检查新bio结构是否可以并入已有请求中。
	 */
	switch (merge_pos) {
	/**
	 * 可以添加到队列末尾
	 */
	case IOSCHED_BACK_MERGE:
		ASSERT(rq_mergeable(req));
		/**
		 * 检查是否可以将请求合并到bio的末尾。
		 */
		if (!queue->merge_bio_tail(queue, req, bio))
			goto no_merge;

		goto out;

	case IOSCHED_FRONT_MERGE:/* 可以插到某个请求的前面 */
		ASSERT(rq_mergeable(req));

		/**
		 * 检查是否可以合并到该请求的前面
		 */
		if (!queue->merge_bio_head(queue, req, bio))
			goto no_merge;

		goto out;

	/**
	 * 未合并，申请新请求。
	 */
	case IOSCHED_NO_MERGE:
		goto no_merge;

	default:
		printk("io-scheduler returned failure (%d)\n", merge_pos);
		BUG();
	}

no_merge:
	if (!new_req) {
		smp_unlock_irq(queue->queue_lock);
		new_req = grab_request(queue, write, wait);
		if (!new_req)
			goto finished;
		else
			goto try;
	}

new:
	req = new_req;
	new_req = NULL;

	/**
	 * 一次标准的读或写操作标志
	 */
	req->flags |= BLKREQ_DATA;

	/**
	 * 是一次预读。
	 * 或者调用者不希望重试。
	 * 那么，失败时就直接返回不用重试。
	 */
	if (bio_rw_ahead(bio) || bio_noretry(bio))
		req->flags |= BLKREQ_NORETRY;

	/**
	 * 初始化请求描述符中的字段。
	 */
	req->remain_sector_start = req->start_sector = start_sector;
	req->remain_sector_count = req->sector_count = req_sectors;
	req->sectors_seg_drv = req->sectors_seg = fire_sectors;
	req->segcount_hw = blkio_hw_segments(queue, bio);
	req->buffer = bio_data(bio);
	req->bio_head = req->bio_tail = bio;
	req->disk = bio->bi_bdev->disk;
	req->start_time = jiffies;

	/**
	 * 将bio插入请求链表。
	 */
	iosched_add_request(queue, req, IOSCHED_INSERT_ORDERED);

out:
	if (new_req) {
		blk_loosen_request(queue, new_req);
		new_req = NULL;
	}
	/**
	 * 如果是同步BIO
	 * 则调用__blk_generic_push_queue摘除块设备
	 * 并向驱动推送所有请求
	 */
	if (bio_sync(bio))
		__blk_generic_push_queue(queue);

	smp_unlock_irq(queue->queue_lock);

	return 0;
/**
 * 遇到错误了，直接终止BIO，向调用者发送通知
 */
finished:
	blkio_finished(bio, req_sectors << 9, err);
	return 0;
}

/**
 * 重新映射IO请求
 * 如果是分区设备，则转换为磁盘设备的扇区编号
 */
static void blk_remap_request(struct block_io_desc *bio)
{
	struct block_device *bdev = bio->bi_bdev;

	/**
	 * 检查一个块设备是否指的是一个磁盘分区
	 */
	if (bdev != bdev->container) {
		/**
		 * 获得分区的hd_struct描述符
		 */
		struct partition_desc *p = bdev->partition;

		/**
		 * 更新计数值
		 */
		if (bio->bi_flags & BIOFLAG_WRITE) {
			p->write_sectors += bio_sectors(bio);
			p->write_times++;
		} else {
			p->read_sectors += bio_sectors(bio);
			p->read_times++;
		}
		/**
		 * 调整bi_sector，把相对于分区的起始扇区号转变为相对于整个磁盘的扇区号。
		 */
		bio->start_sector += p->start_sector;
		/**
		 * 将bio->bi_bdev设置为整个磁盘的块设备描述符
		 * 这样，下次就不会再次调整上面的值了
		 */
		bio->bi_bdev = bdev->container;
	}
}

/**
 * 上层向通用块层提交请求的接口函数
 */
void blk_submit_request(bool write, struct block_io_desc *bio)
{
	struct blk_request_queue *queue;
	sector_t bd_sectors;
	int ret, req_sectors = bio_sectors(bio);

	ASSERT(bio->remain_size > 0);
	might_sleep();

	/**
	 * 计算块设备的最大扇区数。
	 */
	bd_sectors = bd_get_sectors(bio->bi_bdev);
	if (bd_sectors) {
		sector_t sector = bio->start_sector;

		/**
		 * 请求的起始扇区号大于最大扇区号，或者扇区号溢出。
		 */
		if (bd_sectors < req_sectors || bd_sectors - req_sectors < sector) {
			char dev_name[BDEVNAME_SIZE];

			format_block_devname(bio->bi_bdev, dev_name);
			printk(KERN_INFO "attempt to access beyond end of device\n");
			printk(KERN_INFO "%s: flags=%ld, want=%Lu, limit=%Lu\n",
				dev_name, bio->bi_flags,
				(u64)bio->start_sector + req_sectors,
				bd_sectors);
			dump_stack();
			bio->bi_flags |= BIOFLAG_EOF;
			goto err;
		}
	}

	/**
	 * 记录读写方式
	 */
	if (write)
		bio->bi_flags |= BIOFLAG_WRITE;
	/**
	 * 统计读写数量
	 */
	if (write)
		update_page_statistics(bio_write, req_sectors);
	else
		update_page_statistics(bio_read, req_sectors);

	do {
		/**
		 * 获取与块设备相关的请求队列。
		 */
		queue = blkdev_get_queue(bio->bi_bdev);
		/**
		 * 如果为空，说明遇到异常情况
		 */
		if (!queue) {
			char dev_name[BDEVNAME_SIZE];

			format_block_devname(bio->bi_bdev, dev_name);
			printk(KERN_ERR "__submit_bio: Trying to access "
				"nonexistent block-device %s (%Lu)\n",
				dev_name, (u64) bio->start_sector);
			dump_stack();
			goto err;
		}

		/**
		 * 请求的扇区数量太大，错误
		 */
		if (unlikely(bio_sectors(bio) > queue->request_settings.max_hw_sectors)) {
			char dev_name[BDEVNAME_SIZE];

			format_block_devname(bio->bi_bdev, dev_name);
			printk("bio too big device %s (%u > %u)\n", 
				format_block_devname(bio->bi_bdev, dev_name),
				bio_sectors(bio),
				queue->request_settings.max_hw_sectors);
			goto err;
		}

		/**
		 * 如果是分区，则将分区的扇区位置转换成设备的扇区位置。
		 * 并对分区进行一些计数。
		 */
		blk_remap_request(bio);

		/**
		 * 请BIO请求传递给IO调度层。
		 * 常见块设备的回调函数是blk_generic_sumit_request
		 */
		ret = queue->sumit_request(queue, bio);
	/**
	 * 如果ret不这0，表示bio已经被修改
	 * 需要将请求提交给另外的设备，主要处理栈式块设备
	 */
	} while (ret);

	return;
err:
	blkio_finished(bio, bio->remain_size, -EIO);
}

/**
 * 文件系统层向块设备层推送请求的方法
 */
static void blkdev_infrast_push(struct blkdev_infrast *infrast,
				   struct page_frame *page)
{
	struct blk_request_queue *queue = infrast->push_io_data;

	if (queue->push_queue)
		queue->push_queue(queue);
}

/**
 * 获得块设备抽象对象
 */
struct blkdev_infrast *blk_get_infrastructure(struct block_device *bdev)
{
	struct blk_request_queue *queue = blkdev_get_queue(bdev);
	struct blkdev_infrast *ret = NULL;

	if (queue)
		ret = &queue->blkdev_infrast;

	return ret;
}

/**
 * 为磁盘创建一个请求队列，并将其中许多字段初始化为缺省值。
 * 	engorge:	设备驱动程序的策略例程，此例程从队列中接收并处理请求
 *	lock:	设备描述符的锁地址，如果为NULL则使用队列的锁
 * 该函数也初始化IO调度算法。
 * 如果驱动程序想用其他调度算法，可以修改相应字段。
 */
struct blk_request_queue *blk_create_queue(blkdev_engorge_queue_f engorge, struct smp_lock *lock)
{
	/**
	 * 分配队列描述符内存
	 */
	struct blk_request_queue *queue =
		beehive_alloc(queue_allotter, PAF_KERNEL | __PAF_ZERO);

	/**
	 * 分配失败
	 */
	if (!queue)
		return NULL;

	if (lock)
		queue->queue_lock		= lock;
	else {
		smp_lock_init(&queue->default_lock);
		queue->queue_lock = &queue->default_lock;
	}
	queue->engorge_queue		= engorge;

	init_object(&queue->object, NULL, __release_queue);
	queue->merge_bio_tail = merge_bio_tail;
	queue->merge_bio_head = merge_bio_head;
	queue->merge_requests = merge_requests;

	queue->state = BLKQUEUE_COMBINED;
	queue->blkdev_infrast.push_io = blkdev_infrast_push;
	queue->blkdev_infrast.push_io_data = queue;
	queue->blkdev_infrast.max_ra_pages = BLK_MAX_READAHEAD >> PAGE_CACHE_SHIFT;
	queue->blkdev_infrast.state = 0;
	queue->blkdev_infrast.mem_device = 0;

	queue->max_requests = BLKDEV_MAX_RQ;
	queue->request_settings.max_hw_segment_count = MAX_HW_SEGMENTS;
	blk_set_request_max_sectors(queue, MAX_SECTORS);
	blk_set_request_max_hw_pages(queue, MAX_HW_SEGMENTS);
	blk_set_request_boundary(queue, 0xffffffff);
	blk_set_request_sector_size(queue, 512);
	blk_set_request_dma_alignment(queue, 511);
	blk_set_dma_limit(queue, BLKDEV_LIMIT_HIGH);

	queue->sumit_request = blk_generic_sumit_request;

	queue->busy_thresh = 4;
	queue->push_delay = (3 * HZ) / 1000;
	if (queue->push_delay == 0)
		queue->push_delay = 1;
	timer_init(&queue->push_timer);
	queue->push_timer.handle = blk_push_timeout;
	queue->push_timer.data = (void *)queue;
	queue->push_queue = blk_generic_push_queue;
	INIT_WORK(&queue->push_work, blk_push_work, queue);

	/**
	 * 当需要清空队列时，新请求放入此队列
	 */
	list_init(&queue->delayed_list);

	/**
	 * 等待队列
	 *	1、等待读请求
	 *	2、等待写请求
	 *	3、等待请求队列被清空
	 */
	init_waitqueue(&queue->request_pools[READ].wait);
	init_waitqueue(&queue->request_pools[WRITE].wait);
	init_waitqueue(&queue->wait_drain);

	/**
	 * 初始化IO调度器
	 */
	if (!iosched_init(queue, NULL))
		return queue;

	/**
	 * 失败后，释放引用计数，这里会释放内存
	 */
	blk_loosen_queue(queue);
	return NULL;
}

/**
 * 初始化块设备层
 */
int __init init_block_layer(void)
{
	block_workqueue = create_workqueue("kblockd");
	if (!block_workqueue)
		panic("Failed to create kblockd\n");
	
	request_allotter = beehive_create("request_allotter",
			sizeof(struct blk_request), 0, BEEHIVE_PANIC, NULL);

	queue_allotter = beehive_create("queue_allotter",
			sizeof(struct blk_request_queue), 0, BEEHIVE_PANIC, NULL);

	return 0;
}
