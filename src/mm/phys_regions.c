#include <dim-sum/boot_allotter.h>
#include <dim-sum/errno.h>
#include <dim-sum/init.h>
#include <dim-sum/mem.h>
#include <dim-sum/memory_regions.h>
#include <dim-sum/mm.h>
#include <dim-sum/page_area.h>

#include "internal.h"

struct phys_memory_regions all_memory_regions = {
		.cnt = 0,
	};

unsigned long max_dma_pgnum, max_pgnum;

#define MAX_DMA_PGNUM (GENMASK(31, 0) >> PAGE_SHIFT)

int add_memory_regions(unsigned long base, unsigned long size)
{
	unsigned long pgnum;

	if (all_memory_regions.cnt >= MAX_PHYS_REGIONS_COUNT)
		return -ENOSPC;

	pgnum = base + (size >> PAGE_SHIFT);
	if (pgnum > max_pgnum)
		max_pgnum = pgnum;
	if (pgnum >= MAX_DMA_PGNUM)
		max_dma_pgnum = MAX_DMA_PGNUM;
	else
		max_dma_pgnum = pgnum;

	all_memory_regions.regions[all_memory_regions.cnt].base = base;
	all_memory_regions.regions[all_memory_regions.cnt].size = size;
	all_memory_regions.cnt++;

	return 0;
}

int phys_addr_is_valid(phys_addr_t addr)
{
	int i;

	for (i = 0; i < all_memory_regions.cnt; i++) {
		unsigned long start;
		unsigned long end;

		start = all_memory_regions.regions[i].base;
		end = all_memory_regions.regions[i].base
				+ all_memory_regions.regions[i].size;

		if ((addr >= start) && (addr <= end))
			return 1;
	}

	return 0;
}

unsigned long min_phys_addr(void)
{
	return all_memory_regions.regions[0].base;
}

unsigned long max_phys_addr(void)
{
	int idx = all_memory_regions.cnt - 1;
	unsigned long ret = all_memory_regions.regions[idx].base
					+ all_memory_regions.regions[idx].size;

	return ret;
}

static void free_bootmem_bundle(unsigned long page_num, int order)
{
	struct page_frame *page;
	int i;

	page = number_to_page(page_num);
	for (i = 0; i < (1 << order); i++) {
		clear_page_ghost(page + i);
	}
	set_page_ref_count(page, 1);
	free_page_frames(page, order);
}

static __init unsigned long
free_all_bootmem_core(unsigned long page_num_start, unsigned long page_num_end)
{
	unsigned long page_num;

#if 0
	for (page_num = page_num_start; page_num < page_num_end; page_num++)
		free_bootmem_bundle(page_num, 0);
#else
	for (page_num = page_num_start;
		    page_num < round_up(page_num_start, 1 << (PG_AREA_MAX_ORDER - 1));
		    page_num++) {
		free_bootmem_bundle(page_num, 0);
	}

	for (page_num = round_down(page_num_end, 1 << (PG_AREA_MAX_ORDER - 1));
		    page_num < page_num_end;
		    page_num++) {
		free_bootmem_bundle(page_num, 0);
	}

	for (page_num = round_up(page_num_start, 1 << (PG_AREA_MAX_ORDER - 1));
		    page_num < round_down(page_num_end, 1 << PG_AREA_MAX_ORDER);
		    page_num += (1 << PG_AREA_MAX_ORDER)) {
		free_bootmem_bundle(page_num, (PG_AREA_MAX_ORDER - 1));
	}
#endif

	return 0;
}

unsigned long free_all_bootmem(void)
{
	unsigned long bootmem_phy = linear_virt_to_phys(boot_mem_allocated());
	int i;

	for (i = 0; i < all_memory_regions.cnt; i++) {
		unsigned long start;
		unsigned long end;

		if ((bootmem_phy >= all_memory_regions.regions[i].base) &&
			(bootmem_phy <= all_memory_regions.regions[i].base
							+ all_memory_regions.regions[i].size)) {
			start = round_up(bootmem_phy, PAGE_SIZE);
		} else {
			start = all_memory_regions.regions[i].base;
		}
		end = all_memory_regions.regions[i].base
				+ all_memory_regions.regions[i].size;

		free_all_bootmem_core(__phys_to_pgnum(start),
							__phys_to_pgnum(end));
	}

	return 0;
}
