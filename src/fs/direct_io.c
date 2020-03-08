#include <dim-sum/aio.h>
#include <dim-sum/beehive.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/block_buf.h>
#include <dim-sum/err.h>
#include <dim-sum/fs.h>
#include <dim-sum/highmem.h>
#include <dim-sum/journal.h>
#include <dim-sum/rbtree.h>
#include <dim-sum/uio.h>
#include <dim-sum/pagemap.h>
#include <dim-sum/timer.h>

#include <asm/cacheflush.h>

/**
 * 暂时不实现direct-io
 */
ssize_t __blockdev_direct_IO(int rw, struct async_io_desc *aio,
	struct file_node *fnode, struct block_device *bdev, const struct io_segment *iov,
	loff_t offset, unsigned long nr_segs, get_blocks_f get_blocks,
	dio_iodone_t end_io, int dio_lock_type)
{
	return -EINVAL;
}
