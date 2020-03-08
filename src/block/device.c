#include <dim-sum/blk_dev.h>
#include <dim-sum/block_buf.h>
#include <dim-sum/err.h>
#include <dim-sum/errno.h>
#include <dim-sum/disk.h>
#include <dim-sum/fs.h>
#include <dim-sum/pagemap.h>
#include <dim-sum/scatterlist.h>
#include <dim-sum/wait.h>

void blkdev_set_flush_flags(struct blk_request_queue *q, unsigned int flush)
{
	q->flush_flags = flush & (BLKREQ_FLUSH | BLKREQ_NOCACHE);
}

/**
 * 设置驱动程序可以处理的段的最大值。
 */
void blkdev_set_hw_max_pages(struct blk_request_queue *q, unsigned short max_segments)
{
	ASSERT(max_segments > 0);

	q->request_settings.max_hw_segment_count = max_segments;
}

/**
 * 块设备是否只读
 */
int blkdev_read_only(struct block_device *dev)
{
	if (!dev)
		return 0;
	/**
	 * 设备是分区而不是磁盘
	 */
	else if (dev->container != dev)
		/**
		 * 返回分区描述符中的只读标志
		 */
		return dev->partition->read_only;
	else
		/**
		 * 否则返回磁盘描述符中的只读标志
		 */
		return dev->disk->read_only;
}

void blkdev_finish_request(struct blk_request *rq, int error)
{
	int uptodate = 1;
	if (error < 0)
		uptodate = error;

	/**
	 * 标志所有剩余扇区都已经处理完毕
	 */
	if (blk_update_request(rq, uptodate, rq->remain_sector_count))
		BUG();
	blk_finish_request(rq);
}

/**
 * 驱动重新允许接受请求
 */
void blkdev_start_quest(struct blk_request_queue *q)
{
	atomic_clear_bit(__BLKQUEUE_STOPPED, &q->state);

	if (!atomic_test_and_set_bit(__BLKQUEUE_REQUESTING, &q->state)) {
		q->engorge_queue(q);
		atomic_clear_bit(__BLKQUEUE_REQUESTING, &q->state);
	} else {
		blk_attach_device(q);
		kblockd_schedule_work(&q->push_work);
	}
}

/**
 * 如果驱动程序不能再处理更多命令
 * 则调用此函数通知块设备层。
 */
void blkdev_stop_request(struct blk_request_queue *q)
{
	/**
	 * 禁止attach队列
	 */
	blk_disable_attach(q);
	atomic_set_bit(__BLKQUEUE_STOPPED, &q->state);
}

/**
 * 从块设备中读取一个扇区的内容
 */
unsigned char *blkdev_read_sector(struct block_device *blk_dev,
		sector_t sector, struct page_frame **ret)
{
	struct file_cache_space *cache_space = blk_dev->fnode->cache_space;
	struct page_frame *page;
	unsigned int offset;

	/**
	 * 将扇区所在的页面读入到缓存中
	 */
	page = read_cache_page(cache_space, SECTOR_TO_PAGE(sector), NULL);
	if (!IS_ERR(page) ) {
		/**
		 * 读入过程中遇到了异常
		 */
		if (!pgflag_uptodate(page) || pgflag_error(page)) {
			/**
			 * 释放页面缓存并返回错误
			 */
			loosen_page_cache(page);
			goto fail;
		}

		/**
		 * 返回页面缓存及扇区起始数据的地址
		 */
		*ret = page;
		offset = SECOFF_IN_PAGE(sector);
		return (unsigned char *)page_address(page) + offset;
	}

/**
 * 运行到这里，说明出现了错误
 */
fail:
	*ret = NULL;
	return NULL;
}

int scsi_cmd_blk_ioctl(struct block_device *bd, fmode_t mode,
		       unsigned int cmd, void __user *arg)
{
	return -ENOSYS;
}

int blkdev_ioctl(struct file_node *file_node, struct file *file, unsigned cmd,
			unsigned long __user arg)
{
	return -ENOSYS;
}
