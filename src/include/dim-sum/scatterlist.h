#ifndef _DIM_SUM_SCATTERLIST_H
#define _DIM_SUM_SCATTERLIST_H

#include <dim-sum/mm.h>
#include <asm/scatterlist.h>

static inline void sg_assign_page(struct scatterlist *sg, struct page_frame *page)
{
	unsigned long page_link = sg->page_link & 0x3;

	BUG_ON((unsigned long) page & 0x03);

	sg->page_link = page_link | (unsigned long) page;
}

static inline void sg_set_page(struct scatterlist *sg, struct page_frame *page,
			       unsigned int len, unsigned int offset)
{
	sg_assign_page(sg, page);
	sg->offset = offset;
	sg->length = len;
}

static inline void sg_set_buf(struct scatterlist *sg, const void *buf,
			      unsigned int buflen)
{
	sg_set_page(sg, linear_virt_to_page(buf), buflen, offset_in_page(buf));
}

#define sg_is_chain(sg)		((sg)->page_link & 0x01)
#define sg_is_last(sg)		((sg)->page_link & 0x02)
#define sg_chain_ptr(sg)	\
	((struct scatterlist *) ((sg)->page_link & ~0x03))

static inline void sg_clear_end(struct scatterlist *sg)
{
	sg->page_link &= ~0x02;
}

static inline void sg_mark_end(struct scatterlist *sg)
{
	sg->page_link |= 0x02;
	sg->page_link &= ~0x01;
}

static inline struct page_frame *sg_page(struct scatterlist *sg)
{
	return (struct page_frame *)((sg)->page_link & ~0x3);
}

static inline dma_addr_t sg_phys(struct scatterlist *sg)
{
	return page_to_phys_addr(sg_page(sg)) + sg->offset;
}

#define for_each_sg(sglist, sg, nr, __i)	\
	for (__i = 0, sg = (sglist); __i < (nr); __i++, sg = sg_next(sg))

int sg_nents(struct scatterlist *sg);
struct scatterlist *sg_next(struct scatterlist *);
struct scatterlist *sg_last(struct scatterlist *s, unsigned int);
void sg_init_table(struct scatterlist *, unsigned int);
void sg_init_one(struct scatterlist *, const void *, unsigned int);

#endif /* _DIM_SUM_SCATTERLIST_H */
