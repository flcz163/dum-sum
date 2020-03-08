#ifndef _DIM_SUM_MEMORY_REGIONS_H
#define _DIM_SUM_MEMORY_REGIONS_H

#include <asm/page.h>

#define MAX_PHYS_REGIONS_COUNT	16
/**
 * 系统内存区描述
 */
struct phys_memory_regions {
	unsigned long cnt;
	struct {
		unsigned long base;
		unsigned long size;
	} regions[MAX_PHYS_REGIONS_COUNT];
};

extern struct phys_memory_regions all_memory_regions;

int add_memory_regions(unsigned long base, unsigned long size);
unsigned long min_phys_addr(void);
unsigned long max_phys_addr(void);

#endif /* _DIM_SUM_MEMORY_REGIONS_H */
