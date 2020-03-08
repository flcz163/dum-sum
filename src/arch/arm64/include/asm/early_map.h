#ifndef __ASM_ARM64_EARLY_MAP_H
#define __ASM_ARM64_EARLY_MAP_H

#ifndef __ASSEMBLY__
#include <dim-sum/kernel.h>
#include <asm/page.h>

/**
 * early-map索引
 */
enum {
	EARLY_MAP_BEGIN = 1, /* 1!! */
	/**
	 * 只能输出而不能输入的串口
	 */
	EARLY_MAP_EARLYCON,
	/**
	 * 和EARLY_MAP_BEGIN一起形成内存保护墙
	 */
	EARLY_MAP_END,
};

/**
 * early-map的页表属性
 * 一般也就是IO页面属性了
 */
#define EARLY_MAP_PAGEATTR_IO     page_attr(PROT_DEVICE_nGnRE)

#include <asm-generic/early_map.h>

/**
 * early-map模块初始化
 */
void __init init_early_map_early(void);

#endif /* !__ASSEMBLY__ */

#endif /* __ASM_ARM64_EARLY_MAP_H */
