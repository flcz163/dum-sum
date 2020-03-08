#ifndef __ASM_CACHEFLUSH_H
#define __ASM_CACHEFLUSH_H

#include <dim-sum/mm.h>

#define PG_dcache_clean PG_arch_1

extern void flush_cache_all(void);
extern void __flush_dcache_area(void *addr, size_t len);
/**
 * lazy模式
 * 此时仅仅清除clean标志
 * 在真正进行页面映射时再刷新
 */
extern void flush_dcache_page(struct page_frame *);
extern void flush_icache_range(unsigned long start, unsigned long end);

static inline void flush_cache_vmap(unsigned long start, unsigned long end)
{
}

static inline void flush_cache_vunmap(unsigned long start, unsigned long end)
{
}

#endif
