#ifndef __DIM_SUM_PAGE_NUM_H
#define __DIM_SUM_PAGE_NUM_H

#ifndef __ASSEMBLY__
#include <dim-sum/types.h>
#include <dim-sum/kernel.h>
#include <dim-sum/board_config.h>

#include <asm/page.h>

struct page_frame;

/**
 * 物理地址与页面索引之间的转换
 */
#define PAGE_NUM_ROUND_UP(x)	(((x) + PAGE_SIZE - 1) >> PAGE_SHIFT)
#define PAGE_NUM_ROUND_DOWN(x)	((x) >> PAGE_SHIFT)
#define PAGE_NUM_TO_PHYS(x)	((phys_addr_t)(x) << PAGE_SHIFT)

/**
 * 所有物理地址段空间宽度
 */
#define MEM_SECTIONS_WIDTH	(MAX_PHYSMEM_BITS - SECTION_SIZE_BITS)
#define MEM_SECTIONS_COUNT	(1UL << MEM_SECTIONS_WIDTH)
#define MEM_SECTIONS_MASK		(MEM_SECTIONS_COUNT - 1)
#define MEM_SECTIONS_RSHIFT	(NODE_PG_AREA_SHIFT - MEM_SECTIONS_WIDTH)

/**
 * 页面对象的section ID
 */
static inline unsigned long sectid_of_page(const struct page_frame *page)
{
	/**
	 * 从flags标志中取出最左边的位
	 * 这些位表示该页的内存段编号
	 */
	return (page->flags >> MEM_SECTIONS_RSHIFT) & MEM_SECTIONS_MASK;
}

/**
 * page_info的后两位
 *	MEM_SECTION_PRESENT:是否已经初始化
 *	MEM_SECTION_PRESENT:是否已经映射
 */
#define MEM_SECTION_PRESENT	BIT(0)
#define MEM_SECTION_MAPPED		BIT(1)
#define MEM_SECTION_FLAG_MASK			GENMASK(1, 0)
struct mem_section_desc {
	/**
	 * 最后两位是状态标志
	 * 其余位表示页面描述符地址
	 */
	unsigned long page_info;
	/**
	 * 内存段所在的NUMA节点
	 * 可以优化一下，将两个字段合并
	 * 不过我觉得那属于过度设计
	 */
	u16 node_id;
};

#define PAGE_NUM_SECTION_SHIFT	(SECTION_SIZE_BITS - PAGE_SHIFT)
/**
 * 页面编号转换为内存段编号
 */
#define page_num_to_section_nr(num) ((num) >> PAGE_NUM_SECTION_SHIFT)
/**
 * 内存段编号转换为页面编号
 */
#define section_nr_to_page_num(nr) ((nr) << PAGE_NUM_SECTION_SHIFT)

/**
 * 所有可能的内存段描述符表
 */
extern struct mem_section_desc __mem_section[MEM_SECTIONS_COUNT];

/**
 * 找到内存段的第一个页面描述符
 */
static inline struct page_frame *__page_of_section(struct mem_section_desc *section)
{
	unsigned long map = section->page_info;
	map &= (~MEM_SECTION_FLAG_MASK);

	return (struct page_frame *)map;
}

/**
 * 找到内存段编号对应的内存段描述符
 */
static inline struct mem_section_desc *__nr_to_mem_section(unsigned long nr)
{
	if (nr >= MEM_SECTIONS_COUNT)
		return NULL;

	return &__mem_section[nr];
}

/**
 * 页面号转换为段描述符
 */
static inline struct mem_section_desc *page_num_to_section(unsigned long num)
{
	return __nr_to_mem_section(page_num_to_section_nr(num));
}

/**
 * 获得页面的编号
 */
static inline unsigned long number_of_page(struct page_frame * pg)
{
	/**
	 * 取出页面的段编号
	 * 这里为什么不直接保存页编号?
	 */
	int __sec = sectid_of_page(pg);

	/**
	 * 注意:段描述符里面保存的内容
	 */
	return (unsigned long)(pg - __page_of_section(__nr_to_mem_section(__sec)));
}

/**
 * 页帧号转换为页面描述符
 */
static inline struct page_frame *number_to_page(unsigned long num)
{
	/**
	 * 找到页面所在的段描述符
	 */
	struct mem_section_desc *__sec = page_num_to_section(num);

	/**
	 * 这里有一个小技巧
	 * 描述符里面保存的并不是第一个页面的描述符
	 * 需要留意初始化时保存在描述符中的值
	 */
	return __page_of_section(__sec) + num;
}

/**
 * 获得页面所在的NUMA节点编号
 * 首先获得页面的段编号
 * 根据段编号获得其NUMA节点编号
 */
extern int node_id_of_page(const struct page_frame *page);

#endif

#endif /* __DIM_SUM_PAGE_NUM_H */
