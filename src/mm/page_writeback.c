#include <dim-sum/fs.h>
#include <dim-sum/mm.h>

int set_page_dirty(struct page_frame *page)
{
	struct file_cache_space *space = page_cache_space(page);

	if (likely(space)) {
		int (*spd)(struct page_frame *) = space->ops->set_page_dirty;
		if (spd)
			return (*spd)(page);
		return __set_page_dirty_buffers(page);
	}
	if (!pgflag_dirty(page))
		set_pageflag_dirty(page);
	return 0;
}
