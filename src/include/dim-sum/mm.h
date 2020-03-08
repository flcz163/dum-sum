#ifndef __DIM_SUM_MM_H_
#define __DIM_SUM_MM_H_

#include <dim-sum/kernel.h>
#include <dim-sum/init.h>
#include <dim-sum/mm_types.h>
#include <dim-sum/page_area.h>
#include <dim-sum/page_flags.h>
#include <dim-sum/page_num.h>
#include <asm/mmu.h>
#include <asm/memory.h>

struct writeback_control;
struct file_node;

void __init mem_init(void);

#define PAGE_ALIGN(addr) ALIGN(addr, PAGE_SIZE)

int __pte_alloc_kernel(pt_l3_t *pmd, unsigned long address);

#define pte_alloc_kernel(pmd, address)			\
	((unlikely(pt_l3_is_empty(*(pmd))) && __pte_alloc_kernel(pmd, address))? \
		NULL: pt_l4_ptr(pmd, address))

int __pmd_alloc(struct memory_map_desc *mm, pt_l2_t *pud, unsigned long address);
static inline pt_l3_t *pmd_alloc(struct memory_map_desc *mm, pt_l2_t *pud, unsigned long address)
{
	return (unlikely(pt_l2_is_empty(*pud)) && __pmd_alloc(mm, pud, address))?
		NULL: pt_l3_ptr(pud, address);
}


#define offset_in_page(p)	((unsigned long)(p) & ~PAGE_MASK)

struct page_area;
extern struct page_area *page_area_nodes[];
/**
 * 接收一个页描述符的地址作为它的参数
 * 它读取页描述符的flags字段的高位，并通过zone_table数组来确定相应管理区描述符的地址
 */
static inline struct page_area *page_to_pgarea(struct page_frame *page)
{
	return page_area_nodes[page->flags >> NODE_PG_AREA_SHIFT];
}

#define set_page_ref_count(p,v) 	accurate_set(&(p)->ref_count, v - 1)
/**
 * 页框使用者数目。
 * 注意_count为0表示有一个使用在使用该页框。
 */
#define page_ref_count(p)		(accurate_read(&(p)->ref_count) + 1)

extern void hold_page(struct page_frame *page);
#define loosen_page_testzero(p)				\
	({	/* TO-DO */						\
		/* BUG_ON(page_ref_count(p) == 0);	*/	\
		accurate_add_test_negative(-1, &(p)->ref_count);	\
	})

static inline struct page_frame *linear_virt_beehive(const void *addr)
{
	struct page_frame *page;

	page = linear_virt_to_page(addr);
	if (pgflag_beehive(page) && pgflag_additional(page))
		page = page->first_page;

	return page;
}

/**
 * 初始化页面共享计数
 * 注意初值是-1
 */
static inline void init_page_share_count(struct page_frame *page)
{
	accurate_set(&page->share_count, -1);
}

/**
 * 页面被匿名映射
 */
#define PAGE_MAPPING_ANON	1
static inline int page_mapped_anon(struct page_frame *page)
{
	return ((unsigned long)page->cache_space & PAGE_MAPPING_ANON) != 0;
}

/**
 * 页面是否映射到用户态地址空间
 */
static inline int page_mapped_user(struct page_frame *page)
{
	return accurate_read(&(page)->share_count) >= 0;
}

/**
 * 接收页描述符地址，返回_mapcount+1
 * 这样，如果返回为1，表明是某个进程的用户态址空间中存放的一个非共享页。
 */
static inline int page_mapcount(struct page_frame *page)
{
	return accurate_read(&(page)->share_count) + 1;
}

#define page_address(page) linear_phys_to_virt(page_to_phys_addr(page))

void loosen_page(struct page_frame *page);

extern void truncate_inode_pages(struct file_cache_space *, loff_t);

int set_page_dirty(struct page_frame *page);

static inline struct file_cache_space *page_cache_space(struct page_frame *page)
{
	struct file_cache_space *space = page->cache_space;

	if (unlikely((unsigned long)space & PAGE_MAPPING_ANON))
		space = NULL;
	return space;
}

static inline pgoff_t page_index(struct page_frame *page)
{
	return page->index;
}

int __set_page_dirty_buffers(struct page_frame *page);
int redirty_page_for_writepage(struct writeback_control *wbc,
				struct page_frame *page);
extern int vmtruncate(struct file_node * file_node, loff_t offset);
int clear_page_dirty_for_io(struct page_frame *page);

int __set_page_dirty_nobuffers(struct page_frame *page);

extern void init_pagecache(void);

#endif /* __DIM_SUM_MM_H_ */
