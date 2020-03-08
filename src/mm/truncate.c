#include <dim-sum/block_buf.h>
#include <dim-sum/fs.h>
#include <dim-sum/mm.h>
#include <dim-sum/pagevec.h>
#include <dim-sum/pagemap.h>

/**
 * 使页面缓存失效
 */
static int
invalidate_complete_page(struct file_cache_space *mapping, struct page_frame *page)
{
	if (page->cache_space!= mapping)
		return 0;

	if (pgflag_private(page) && !try_to_release_page(page, 0))
		return 0;

	smp_lock_irq(&mapping->tree_lock);
	if (pgflag_dirty(page)) {
		smp_unlock_irq(&mapping->tree_lock);
		return 0;
	}

	BUG_ON(pgflag_private(page));
	__remove_from_page_cache(page);
	smp_unlock_irq(&mapping->tree_lock);
	clear_page_uptodate(page);
	loosen_page(page);

	return 1;
}

/**
 * invalidate_mapping_pages - Invalidate all the unlocked pages of one inode
 * @mapping: the address_space which holds the pages to invalidate
 * @start: the offset 'from' which to invalidate
 * @end: the offset 'to' which to invalidate (inclusive)
 *
 * This function only removes the unlocked pages, if you want to
 * remove all the pages of one inode, you must call truncate_inode_pages.
 *
 * invalidate_mapping_pages() will not block on IO activity. It will not
 * invalidate pages which are dirty, locked, under writeback or mapped into
 * pagetables.
 */
unsigned long invalidate_mapping_pages(struct file_cache_space *mapping,
				pgoff_t start, pgoff_t end)
{
	struct pagevec pvec;
	pgoff_t next = start;
	unsigned long ret = 0;
	int i;

	pagevec_init(&pvec, 0);
	while (next <= end &&
			pgcache_collect_pages(&pvec, mapping, next, PAGEVEC_SIZE)) {
		for (i = 0; i < pagevec_count(&pvec); i++) {
			struct page_frame *page = pvec.pages[i];

			if (pgflag_test_and_set_locked(page)) {
				next++;
				continue;
			}
			if (page->index > next)
				next = page->index;
			next++;
			if (pgflag_dirty(page) || pgflag_writeback(page))
				goto unlock;
			if (page_mapped_user(page))
				goto unlock;
			ret += invalidate_complete_page(mapping, page);
unlock:
			unlock_page(page);
			if (next > end)
				break;
		}
		pagevec_release(&pvec);
	}

	return ret;
}

unsigned long invalidate_page_cache(struct file_cache_space *space)
{
	return invalidate_mapping_pages(space, 0, ~0UL);
}

void truncate_inode_pages(struct file_cache_space *space, loff_t lstart)
{
	/* TO-DO */
}
