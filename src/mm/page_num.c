#include <dim-sum/board_config.h>
#include <dim-sum/boot_allotter.h>
#include <dim-sum/cache.h>
#include <dim-sum/errno.h>
#include <dim-sum/init.h>
#include <dim-sum/memory_regions.h>
#include <dim-sum/mm.h>
#include <dim-sum/numa.h>
#include <dim-sum/page_area.h>
#include <dim-sum/string.h>

#include "internal.h"

/**
 * 所有内存段描述符
 */
aligned_cacheline_in_smp
struct mem_section_desc __mem_section[MEM_SECTIONS_COUNT];

unsigned long total_pages;

/**
 * 段中的页面数量
 */
#define PAGES_PER_SECTION       (1UL << PAGE_NUM_SECTION_SHIFT)
#define PAGE_SECTION_MASK	(~(PAGES_PER_SECTION-1))

/**
 * 最大的页面编号
 * 超过此值即为非法
 */
#define MAX_PAGE_NUM (1UL << (MAX_PHYSMEM_BITS-PAGE_SHIFT))

/**
 * 获得页面所在NUMA节点号
 */
int node_id_of_page(const struct page_frame *page)
{
	int __sec = sectid_of_page(page);

	return __nr_to_mem_section(__sec)->node_id;
}

/**
 * 内存段是否存在
 */
static inline int mem_section_is_present(struct mem_section_desc *section)
{
	return (section && (section->page_info & MEM_SECTION_PRESENT));
}

static inline int mem_section_nr_is_present(unsigned long nr)
{
	return mem_section_is_present(__nr_to_mem_section(nr));
}

/**
 * 添加内存段
 * 在__mem_section中标记段的有效性
 */
static void __init add_sparse_memory(int node_id,
	unsigned long pg_num_start, unsigned long pg_num_end)
{
	unsigned long num;

	BUG_ON(pg_num_start > MAX_PAGE_NUM);
	BUG_ON(pg_num_end > MAX_PAGE_NUM);
	pg_num_start &= PAGE_SECTION_MASK;

	/**
	 * 循环处理每一个内存段
	 * 注意:内存块应当是内存段的整数倍!
	 */
	for (num = pg_num_start; num < pg_num_end; num += PAGES_PER_SECTION) {
		unsigned long sectid = page_num_to_section_nr(num);
		struct mem_section_desc *section;

		section = __nr_to_mem_section(sectid);
		if (!section->page_info) {
			section->page_info = MEM_SECTION_PRESENT;
			section->node_id = node_id;
		}
	}
}

/**
 * 为内存段中的所有页分配页描述符
 */
static struct page_frame __init *alloc_page_frames_desc(unsigned long sectid)
{
	struct page_frame *frames;
	struct mem_section_desc *section = __nr_to_mem_section(sectid);
	unsigned long size;

	size = PAGE_ALIGN(sizeof(struct page_frame) * PAGES_PER_SECTION);
	frames = alloc_boot_mem_permanent(size, PAGE_SIZE);
	if (frames)
		return frames;

	printk(KERN_ERR "%s: failure to alloc memory for section.\n", __func__);
	section->page_info = 0;
	return NULL;
}

static int init_one_section(unsigned long sectid, struct page_frame *frames)
{
	struct page_frame *fake_frame;
	struct mem_section_desc *section = __nr_to_mem_section(sectid);

	if (!mem_section_is_present(section))
		return -EINVAL;

	/**
	 * 这里 是一个小花招
	 * page_info里面保存的并不是第一页的描述符
	 * 而是一个偏移值，以减少计算量
	 */
	fake_frame = frames - section_nr_to_page_num(sectid);
	section->page_info &= MEM_SECTION_FLAG_MASK;
	section->page_info |= (unsigned long)fake_frame | MEM_SECTION_MAPPED;

	return 0;
}

/**
 * 以稀疏内存的方式，初始化page-num模块
 */
void init_sparse_memory(void)
{
	unsigned long sectid;
	struct page_frame *frames;
	int i;

	/**
	 * 遍历所有内存块
	 */
	for (i = 0; i < all_memory_regions.cnt; i++) {
		unsigned long pg_num_min, pg_num_max;
		unsigned long start, end;

		start = all_memory_regions.regions[i].base;
		end = all_memory_regions.regions[i].base
				+ all_memory_regions.regions[i].size;
		pg_num_min = PAGE_NUM_ROUND_UP(start);		/* DOWN */
		pg_num_max = PAGE_NUM_ROUND_DOWN(end);	/* UP */
		/**
		 * 将内存块添加到段描述符数组中
		 * 目前暂时不支持NUMA，因此将nid设置为0
		 * 哼哼，不久的将来会将NUMA支持起来
		 */
		add_sparse_memory(0, pg_num_min, pg_num_max);
	}

	BUG_ON(!is_power_of_2(sizeof(struct mem_section_desc)));

	/**
	 * 为所有存在的段
	 * 初始化其内存段
	 */
	for (sectid = 0; sectid < MEM_SECTIONS_COUNT; sectid++) {
		if (!mem_section_nr_is_present(sectid))
			continue;

		/**
		 * 为内存段分配页描述符
		 */
		frames = alloc_page_frames_desc(sectid);
		if (!frames)
			continue;

		init_one_section(sectid, frames);
	}

	total_pages = 256 * 1024;
}
