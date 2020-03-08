#ifndef __DIM_SUM_SWAP_H
#define __DIM_SUM_SWAP_H

struct page_frame;

extern void mark_page_accessed(struct page_frame *);
extern void lru_cache_add(struct page_frame *);

#endif /* __DIM_SUM_SWAP_H */
