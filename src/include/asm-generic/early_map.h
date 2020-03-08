#ifndef __ASM_GENERIC_EARLY_MAP_H
#define __ASM_GENERIC_EARLY_MAP_H

#include <dim-sum/init.h>
#include <dim-sum/bug.h>

#ifndef __ASSEMBLY__

#define __early_map_to_virt(x)	(EARLY_MAP_VA_START + ((x) << PAGE_SHIFT))
#define __early_map_to_idx(x)	((EARLY_MAP_VA_START + ((x)&PAGE_MASK)) >> PAGE_SHIFT)

static inline unsigned long early_map_to_virt(const unsigned int idx)
{
	BUILD_BUG_ON(idx >= EARLY_MAP_EARLYCON);
	return __early_map_to_virt(idx);
}

static inline unsigned long early_map_to_idx(const unsigned long vaddr)
{
	BUG_ON(vaddr >= EARLY_MAP_VA_END || vaddr < EARLY_MAP_VA_START);
	return __early_map_to_idx(vaddr);
}

#ifndef EARLY_MAP_PAGEATTR_KERNEL
#define EARLY_MAP_PAGEATTR_KERNEL PAGE_ATTR_KERNEL
#endif
#ifndef EARLY_MAP_PAGEATTR_NOCACHE
#define EARLY_MAP_PAGEATTR_NOCACHE PAGE_ATTR_KERNEL_NOCACHE
#endif
#ifndef EARLY_MAP_PAGEATTR_IO
#define EARLY_MAP_PAGEATTR_IO PAGE_ATTR_KERNEL_IO
#endif
#ifndef EARLY_MAP_PAGEATTR_NONE
#define EARLY_MAP_PAGEATTR_NONE page_attr(0)
#endif

/**
 * 映射类型
 */
enum early_map_page_type {
	EARLY_MAP_IO,
	EARLY_MAP_KERNEL,
	EARLY_MAP_NOCACHE,
	EARLY_MAP_CLEAR,
};

/**
 * 建立早期映射
 * 在boot内存初始化完毕
 * 但是系统内存不可用时，可调用
 */
extern void early_mapping(int idx, phys_addr_t phys,
		enum early_map_page_type type);

#endif /* __ASSEMBLY__ */
#endif /* __ASM_GENERIC_EARLY_MAP_H */
