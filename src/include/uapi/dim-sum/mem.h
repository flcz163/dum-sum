#ifndef __UAPI_DIM_SUM_MEM_H__
#define __UAPI_DIM_SUM_MEM_H__

struct page_frame;

enum page_alloc_flag {
	/**
	 * 所请求的页框必须处于DMA内存区
	 */
	__PAGE_ALLOC_DMA,
	/**
	 * 分配的页用于用户进程映射
	 * 因此可以使用高端内存
	 */
	__PAGE_ALLOC_USER,
	/**
	 * 内存区标志位，注意位置
	 */
	__PAGE_ALLOC_AREAS = __PAGE_ALLOC_USER,
	/**
	 * 分配用于beehive分配器的内存
	 */
	__PAGE_ALLOC_BEEHIVE,
	/**
	 * 任何返回的页框必须被填满0
	 */
 	__PAGE_ALLOC_ZERO,
 	/**
	 * 分配可回收的页，主要是页缓存
	 */
 	__PAGE_ALLOC_RECLAIMABLE,
	/**
	 * 允许执行IO操作来回收内存
	 */
	__PAGE_ALLOC_IO,
	/**
	 * 允许执行文件系统操作来回收内存
	 */
	__PAGE_ALLOC_FS,
	/**
	 * 允许内核对等待空闲页框的当前进程进行阻塞
	 */
	__PAGE_ALLOC_WAIT,
	/**
	 * 紧急内存分配，允许内核访问保留的页框池
	 */
	__PAGE_ALLOC_EMERG,
	/**
	 * 所请求的页可能为"冷"的。
	 * 可能是文件页并且内核并不直接访问。
	 */
	__PAGE_ALLOC_COLD,
	/**
	 * 分配内存必须成功，不能失败
	 */
	__PAGE_ALLOC_NOFAIL,
	__PAGE_ALLOC_BITS_COUNT,
};

/**
 * 用于构建后备内存池
 * 注意与前两个标志的关系
 */
#define PAF_AREATYPES	(1UL << __PAGE_ALLOC_AREAS)
#define PAF_AREAMASK	(PAF_AREATYPES - 1)

#define __PAF_DMA	(1UL << __PAGE_ALLOC_DMA)
#define __PAF_USER	(1UL << __PAGE_ALLOC_USER)

#define __PAF_BEEHIVE	(1UL << __PAGE_ALLOC_BEEHIVE)
#define __PAF_ZERO	(1UL << __PAGE_ALLOC_ZERO)
#define __PAF_RECLAIMABLE	(1UL << __PAGE_ALLOC_RECLAIMABLE)
#define __PAF_IO	(1UL << __PAGE_ALLOC_IO)
#define __PAF_FS	(1UL << __PAGE_ALLOC_FS)
#define __PAF_WAIT	(1UL << __PAGE_ALLOC_WAIT)
#define __PAF_EMERG	(1UL << __PAGE_ALLOC_EMERG)
#define __PAF_COLD	(1UL << __PAGE_ALLOC_COLD)
#define __PAF_NOFAIL	(1UL << __PAGE_ALLOC_NOFAIL)

#define __PAF_BITS_COUNT __PAGE_ALLOC_BITS_COUNT
#define __PAF_BITS_MASK ((1 << __PAF_BITS_COUNT) - 1)

#define PAF_DMA		__PAF_DMA
#define PAF_KERNEL	(__PAF_WAIT | __PAF_IO | __PAF_FS)
#define PAF_USER	(__PAF_WAIT | __PAF_IO | __PAF_FS | __PAF_USER)

#define PAF_ATOMIC	(__PAF_EMERG)
#define PAF_NOIO	(__PAF_WAIT)
#define PAF_NOFS	(__PAF_WAIT | __PAF_IO)
#define PAF_NOWAIT	(PAF_ATOMIC & ~__PAF_EMERG)

extern struct page_frame *
__alloc_page_frames(int nid, unsigned int paf_mask, unsigned int order);
/**
 * 用于获得一个单独页框的宏
 * 它返回所分配页框描述符的地址，如果失败，则返回NULL
 */
#define alloc_page_frame(paf_mask) alloc_page_frames(paf_mask, 0)
/**
 * 分配2^order个连续的页框。它返回第一个所分配页框描述符的地址或者返回NULL
 */
#define alloc_page_frames(paf_mask, order) \
		__alloc_page_frames(numa_node_id(), paf_mask, order)
extern void free_page_frames(struct page_frame *page, unsigned int order);
#define free_page_frame(page) free_page_frames((page), 0)
void fastcall free_hot_page_frame(struct page_frame *page);
void fastcall free_cold_page_frame(struct page_frame *page);

extern unsigned long alloc_pages_memory(unsigned int paf_mask, unsigned int order);
/**
 * 用于获得一个单独页框的宏。
 */
#define alloc_page_memory(paf_mask) \
		alloc_pages_memory((paf_mask),0)
extern unsigned long alloc_zeroed_page_memory(unsigned int paf_mask);
/**
 * 释放整页的内存
 */
extern void free_pages_memory(unsigned long addr, unsigned int order);
/**
 * 释放线性地址addr对应的页框。
 */
#define free_page_memory(addr) free_pages_memory((addr), 0)

void *kmalloc_app(int size);
void kfree_app(void *addr);

#endif /* __UAPI_DIM_SUM_MEM_H__ */
