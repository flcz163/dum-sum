#ifndef __DIM_SUM_PAGEVEC_H
#define __DIM_SUM_PAGEVEC_H

/* 14 pointers + two long's align the pagevec structure to a power of two */
#define PAGEVEC_SIZE	14

struct page_frame;
struct file_cache_space;

struct pagevec {
	unsigned long nr;
	unsigned long cold;
	struct page_frame *pages[PAGEVEC_SIZE];
};

static inline void pagevec_init(struct pagevec *pvec, int cold)
{
	pvec->nr = 0;
	pvec->cold = cold;
}

static inline unsigned pagevec_count(struct pagevec *pvec)
{
	return pvec->nr;
}

static inline unsigned pagevec_space(struct pagevec *pvec)
{
	return PAGEVEC_SIZE - pvec->nr;
}

/*
 * Add a page to a pagevec.  Returns the number of slots still available.
 */
static inline unsigned pagevec_add(struct pagevec *pvec, struct page_frame *page)
{
	pvec->pages[pvec->nr++] = page;
	return pagevec_space(pvec);
}

static inline void pagevec_release(struct pagevec *pvec)
{
}

unsigned pgcache_collect_pages(struct pagevec *pvec, struct file_cache_space *space,
		pgoff_t start, unsigned nr_pages);
unsigned pgcache_collect_pages_tag(struct pagevec *pvec,
		struct file_cache_space *space, pgoff_t *index, int tag,
		unsigned nr_pages);

#endif /* __DIM_SUM_PAGEVEC_H */
