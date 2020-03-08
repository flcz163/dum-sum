#ifndef _DIM_SUM_PAGEMAP_H
#define _DIM_SUM_PAGEMAP_H

#include <dim-sum/approximate_counter.h>
#include <dim-sum/fs.h>
#include <dim-sum/mem.h>
#include <dim-sum/mm.h>
#include <dim-sum/page_flags.h>
#include <dim-sum/uaccess.h>

#define PAGE_CACHE_SHIFT	PAGE_SHIFT
#define PAGE_CACHE_SIZE		PAGE_SIZE
#define PAGE_CACHE_MASK		PAGE_MASK
#define PAGE_CACHE_ALIGN(addr)	(((addr)+PAGE_CACHE_SIZE-1)&PAGE_CACHE_MASK)

enum {
	/**
	 * 异步写时，出现IO错误
	 */
	__CS_EIO,
	/**
	 * 写的时候，出现空间不足
	 */
	__CS_ENOSPC,
};

#define CS_EIO			(1UL << __CS_EIO)
#define CS_ENOSPC		(1UL << __CS_ENOSPC)

#define page_cache_hold(page)		hold_page(page)
#define loosen_page_cache(page)	loosen_page(page)
void release_pages(struct page_frame **pages, int nr, int cold);

static inline void cache_space_set_allocflags(struct file_cache_space *space, int mask)
{
	space->alloc_flags = mask;
}

static inline int cache_space_get_allocflags(struct file_cache_space *space)
{
	return space->flags;
}

void lock_page(struct page_frame *page);
extern void unlock_page(struct page_frame *page);

extern void wait_on_page_bit(struct page_frame *page, int bit_nr);

static inline void wait_on_page_writeback(struct page_frame *page)
{
	if (pgflag_writeback(page))
		wait_on_page_bit(page, PG_writeback);
}

void wake_up_page(struct page_frame *page, int bit);

extern void page_finish_writeback(struct page_frame *page);

extern struct page_frame * pgcache_find_alloc_lock(struct file_cache_space *space,
				unsigned long index, unsigned int paf_mask);

/*
 * Returns locked page_frame at given index in given cache, creating it if needed.
 */
static inline struct page_frame *grab_page_cache(struct file_cache_space *space, unsigned long index)
{
	return pgcache_find_alloc_lock(space, index, cache_space_get_allocflags(space));
}

static inline void wait_on_page_locked(struct page_frame *page)
{
	if (pgflag_locked(page))
		wait_on_page_bit(page, PG_locked);
}

static inline struct page_frame *page_cache_alloc(struct file_cache_space *x)
{
	return alloc_page_frames(cache_space_get_allocflags(x), 0);
}

int add_to_page_cache(struct page_frame *page, struct file_cache_space *space,
		pgoff_t offset, int paf_mask);

extern struct approximate_counter pagecache_count;
/**
 * 近似的页缓存计数
 */
static inline void pgcache_add_count(int count)
{
	approximate_counter_mod(&pagecache_count, count);
}

static inline void pgcache_sub_count(int count)
{
	approximate_counter_mod(&pagecache_count, -count);
}

unsigned pgcache_find_pages(struct file_cache_space *space, pgoff_t start,
			unsigned int nr_pages, struct page_frame **pages);

static inline struct page_frame *page_cache_alloc_cold(struct file_cache_space *x)
{
	return alloc_page_frames(cache_space_get_allocflags(x) | __PAF_COLD, 0);
}

extern struct page_frame * pgcache_find_page(struct file_cache_space *space,
				unsigned long index);
int cache_space_tagged(struct file_cache_space *space, int tag);

typedef int filler_t(struct file *, struct page_frame *);
extern struct page_frame * read_cache_page(struct file_cache_space *space,
				unsigned long index, void *data);
unsigned pgcache_find_pages_tag(struct file_cache_space *space, pgoff_t *index,
			int tag, unsigned int nr_pages, struct page_frame **pages);

#endif /* _DIM_SUM_PAGEMAP_H */
