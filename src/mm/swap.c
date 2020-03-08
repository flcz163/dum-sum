#include <dim-sum/mm.h>
#include <dim-sum/pagemap.h>
#include <dim-sum/pagevec.h>
#include <dim-sum/percpu.h>
#include <dim-sum/swap.h>

/* TO-DO */
void hold_page(struct page_frame *page)
{
	accurate_inc(&page->ref_count);
}

static void __page_cache_release(struct page_frame *page)
{
	if (page_ref_count(page) == 0)
		free_hot_page_frame(page);
}

void loosen_page(struct page_frame *page)
{
	if (!pgflag_ghost(page) && loosen_page_testzero(page))
		__page_cache_release(page);
}

void fastcall mark_page_accessed(struct page_frame *page)
{
	/* TO-DO */
}
