#ifndef __ASM_MEMORY_H
#define __ASM_MEMORY_H

#include <linux/compiler.h>
#include <linux/const.h>
#include <dim-sum/types.h>
#include <asm/sizes.h>

#define UL(x) _AC(x, UL)

/**
 * 内核镜像起始虚拟地址
 */
#define KERNEL_VA_START		(UL(0xffffffffffffffff) << (VA_BITS - 1))
/**
 * 早期映射所占用的虚拟地址空间长度
 */
#define EARLY_MAP_SIZE SZ_2M
/**
 * 早期映射所占用的虚拟地址空间起始址
 */
#define EARLY_MAP_VA_START		(KERNEL_VA_START - EARLY_MAP_SIZE)
#define EARLY_MAP_VA_END	(EARLY_MAP_VA_START + EARLY_MAP_SIZE)
/**
 * IO重映射所需要的地址空间
 */
#define IOREMAP_SIZE		SZ_1G
#define IOREMAP_START		(EARLY_MAP_VA_START - IOREMAP_SIZE - SZ_4K)
#define IOREMAP_END		(IOREMAP_START + IOREMAP_SIZE)

/**
 * ARM64内存类型
 */
#define MT_DEVICE_nGnRnE	0
#define MT_DEVICE_nGnRE		1
#define MT_DEVICE_GRE		2
#define MT_NORMAL_NC		3
#define MT_NORMAL		4

#ifndef __ASSEMBLY__
struct memory_map_desc;

/**
 * 起始物理地址
 * 该地址被映射到内核虚拟地址的开始处
 * 一般情况下为0
 */
extern phys_addr_t		phys_addr_origin;

/**
 * 启动内存
 *      boot_memory_start:	起始地址
 *      boot_memory_end:	结束地址
 */
extern unsigned long boot_memory_start;
extern unsigned long boot_memory_end;

/**
 * DMA能读写的最大限制
 */
extern unsigned long phys_addr_dma_max;
/**
 * 内核能读写的最大限制
 */
extern unsigned long phys_addr_kernel_max;

/**
 * 最小的物理地址，映射到内核虚拟地址开始处
 */
#define START_PHYS_ADDR		({ phys_addr_origin; })

/**
 * 针对内核线性映射地址空间
 * 进行物理地址与虚拟地址之间的转换
 */
#define __linear_virt_to_phys(x)	(((phys_addr_t)(x) - KERNEL_VA_START + START_PHYS_ADDR))
#define __linear_phys_to_virt(x)	((unsigned long)((x) - START_PHYS_ADDR + KERNEL_VA_START))

/**
 * 物理地址与页帧号之间的转换
 */
#define	__phys_to_pgnum(paddr)	((unsigned long)((paddr) >> PAGE_SHIFT))
#define	__pgnum_to_phys(pfn)	((phys_addr_t)(pfn) << PAGE_SHIFT)

/**
 * 内核线程地址与物理地址之间的转换
 */
#define linear_virt_to_phys(x)			__linear_virt_to_phys((unsigned long)(x))
#define linear_phys_to_virt(x)			((void *)__linear_phys_to_virt((phys_addr_t)(x)))
/**
 * 内核线性地址转换为页面描述符
 */
#define linear_virt_to_page(kaddr)	number_to_page(linear_virt_to_phys(kaddr) >> PAGE_SHIFT)
/**
 * 内核线性地址是否有效
 */
#define linear_virt_addr_is_valid(kaddr)	pgnum_is_valid(linear_virt_to_phys(kaddr) >> PAGE_SHIFT)

/**
 * 物理地址与页面之间的转换
 */
#define page_to_phys_addr(page)	(__pgnum_to_phys(number_of_page(page)))
#define phys_addr_to_page(phys)	(number_to_page(__phys_to_pgnum(phys)))

/**
 * 设置per-cpu变量的偏移值
 */
extern void init_per_cpu_offsets(void);

#endif

#include <asm/mmu.h>

#endif
