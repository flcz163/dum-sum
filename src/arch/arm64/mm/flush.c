#include <dim-sum/bitops.h>

#include <asm/cacheflush.h>

void flush_dcache_page(struct page_frame *page)
{
	if (test_bit(PG_dcache_clean, &page->flags))
		atomic_clear_bit(PG_dcache_clean, &page->flags);
}