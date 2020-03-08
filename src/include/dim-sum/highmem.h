#ifndef __DIM_SUM_HIGHMEM_H
#define __DIM_SUM_HIGHMEM_H

#include <dim-sum/mm.h>
#include <dim-sum/string.h>

static inline void *kmap(struct page_frame *page)
{
	might_sleep();
	return page_address(page);
}

#define KM_USER0 0

#define kunmap(page) do { (void) (page); } while (0)

#define kmap_atomic(page, idx)		page_address(page)
#define kunmap_atomic(addr, idx)	do { } while (0)

static inline void clear_highpage(struct page_frame *page)
{
	void *kaddr = kmap_atomic(page, KM_USER0);
	clear_page(kaddr);
	kunmap_atomic(kaddr, KM_USER0);
}

#endif /* __DIM_SUM_HIGHMEM_H */
