#include <dim-sum/mem.h>
#include <dim-sum/memory_regions.h>
#include <dim-sum/page_area.h>

#include "internal.h"

void __init init_memory_early(void)
{
	/**
	 * 初始化Beehive内存分配器
	 * 需要分配boot内存，放在伙伴系统之前
	 */
	init_beehive_early();
}

void __init init_memory(void)
{
	/**
	 * 以分散内存的方式 管理内存段
	 * 建议页描述符和页编号之间的关联
	 */
	init_sparse_memory();

	init_page_allotter();
	/**
	 * 将boot内存管理转换为伙伴内存管理
	 */
	free_all_bootmem();

	init_beehive_allotter();
}
