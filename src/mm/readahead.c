#include <dim-sum/mm_types.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/blk_infrast.h>
#include <dim-sum/fs.h>
#include <dim-sum/pagemap.h>

static void
default_push_io(struct blkdev_infrast *infrast, struct page_frame *page)
{
}

struct blkdev_infrast default_blkdev_infrast = {
	.max_ra_pages	= BLK_MAX_READAHEAD >> PAGE_CACHE_SHIFT,
	.state		= 0,
	.mem_device	= 0,
	.push_io	= default_push_io,
};

void
file_ra_state_init(struct file_ra_state *ra, struct file_cache_space *space)
{
	ra->max_ra_pages = space->blkdev_infrast->max_ra_pages;
	ra->prev_page = -1;
}
