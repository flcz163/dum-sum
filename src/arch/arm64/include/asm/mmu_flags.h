#ifndef __ASM_MMU_FLAGS_H
#define __ASM_MMU_FLAGS_H

#include <linux/const.h>
#include <asm/page.h>

#define UL(x) _AC(x, UL)

/**
 * 虚拟地址位数
 */
#define VA_BITS			(CONFIG_ARM64_VA_BITS)

/**
 * ARM64内存类型
 */
#define MT_DEVICE_nGnRnE	0
#define MT_DEVICE_nGnRE	1
#define MT_DEVICE_GRE		2
#define MT_NORMAL_NC		3
#define MT_NORMAL			4

#define PT3_TYPE_MASK		(_AT(pt_l3_val_t, 3) << 0)
#define PT3_TYPE_TABLE		(_AT(pt_l3_val_t, 3) << 0)
#define PT3_TYPE_SECT		(_AT(pt_l3_val_t, 1) << 0)

#define PT3_SECT_AF		(_AT(pt_l3_val_t, 1 << 10))
#define PT3_SECT_S		(_AT(pt_l3_val_t, 3 << 8))

#define EARLY_MMUFLAGS (MT_NORMAL << 2) \
						| PT3_TYPE_SECT \
						| PT3_SECT_AF \
						| PT3_SECT_S

#define PTRS_PER_PT_L4		(1 << (PAGE_SHIFT - 3))

#define PT_L1_SHIFT		((PAGE_SHIFT - 3) * CONFIG_PGTABLE_LEVELS + 3)
#define PGDIR_SIZE		(_AC(1, UL) << PT_L1_SHIFT)
#define PGDIR_MASK		(~(PGDIR_SIZE-1))
#define PTRS_PER_PT_L1		(1 << (VA_BITS - PT_L1_SHIFT))

#define PUD_TYPE_TABLE		(_AT(pt_l2_val_t, 3) << 0)
#define PUD_TABLE_BIT		(_AT(pt_l1_val_t, 1) << 1)
#define PUD_TYPE_MASK		(_AT(pt_l1_val_t, 3) << 0)
#define PUD_TYPE_SECT		(_AT(pt_l1_val_t, 1) << 0)

#define PT_L3_SHIFT		((PAGE_SHIFT - 3) * 2 + 3)
#define PT_L3_SIZE		(_AC(1, UL) << PT_L3_SHIFT)
#define PT_L3_MASK		(~(PT_L3_SIZE-1))
#define PMD_TYPE_MASK		(_AT(pt_l3_val_t, 3) << 0)
#define PMD_TYPE_FAULT		(_AT(pt_l3_val_t, 0) << 0)
#define PT_L3_TYPE_TABLE		(_AT(pt_l3_val_t, 3) << 0)
#define PMD_TYPE_SECT		(_AT(pt_l3_val_t, 1) << 0)
#define PMD_TABLE_BIT		(_AT(pt_l3_val_t, 1) << 1)
#define PTRS_PER_PT_L3		PTRS_PER_PT_L4

#define PTE_TYPE_MASK		(_AT(pt_l4_val_t, 3) << 0)
#define PTE_TYPE_FAULT		(_AT(pt_l4_val_t, 0) << 0)
#define PTE_TYPE_PAGE		(_AT(pt_l4_val_t, 3) << 0)
#define PTE_TABLE_BIT		(_AT(pt_l4_val_t, 1) << 1)
#define PTE_USER		(_AT(pt_l4_val_t, 1) << 6)		/* AP[1] */
#define PTE_RDONLY		(_AT(pt_l4_val_t, 1) << 7)		/* AP[2] */
#define PTE_SHARED		(_AT(pt_l4_val_t, 3) << 8)		/* SH[1:0], inner shareable */
#define PTE_AF			(_AT(pt_l4_val_t, 1) << 10)	/* Access Flag */
#define PTE_NG			(_AT(pt_l4_val_t, 1) << 11)	/* nG */
#define PTE_PXN			(_AT(pt_l4_val_t, 1) << 53)	/* Privileged XN */
#define PTE_UXN			(_AT(pt_l4_val_t, 1) << 54)	/* User XN */

#define PTE_ATTRINDX(t)		(_AT(pt_l4_val_t, (t)) << 2)
#define PTE_ATTRINDX_MASK	(_AT(pt_l4_val_t, 7) << 2)

#define SECTION_SHIFT		PT_L3_SHIFT
#define SECTION_SIZE		(_AC(1, UL) << SECTION_SHIFT)
#define SECTION_MASK		(~(SECTION_SIZE-1))

#define MMU_BLOCK_SHIFT	SECTION_SHIFT
#define MMU_BLOCK_SIZE	SECTION_SIZE


#define GLOBAL_PGTABLE_LEVELS	(CONFIG_PGTABLE_LEVELS - 1)

#define GLOBAL_DIR_SIZE	(GLOBAL_PGTABLE_LEVELS * PAGE_SIZE)

#define IDMAP_DIR_SIZE		(3 * PAGE_SIZE)

#define TCR_T0SZ_OFFSET		0
#define TCR_T1SZ_OFFSET		16
#define TCR_T0SZ(x)		((UL(64) - (x)) << TCR_T0SZ_OFFSET)
#define TCR_T1SZ(x)		((UL(64) - (x)) << TCR_T1SZ_OFFSET)
#define TCR_TxSZ_WIDTH		6
#define TCR_TG0_4K		(UL(0) << 14)
#define TCR_TG1_4K		(UL(2) << 30)
#define TCR_TxSZ(x)		(TCR_T0SZ(x) | TCR_T1SZ(x))

#define TCR_SHARED		((UL(3) << 12) | (UL(3) << 28))

#define TCR_IRGN_WBWA		((UL(1) << 8) | (UL(1) << 24))
#define TCR_ORGN_WBWA		((UL(1) << 10) | (UL(1) << 26))
#define TCR_ASID16		(UL(1) << 36)
#define TCR_TBI0		(UL(1) << 37)

#define TCR_CACHE_FLAGS	TCR_IRGN_WBWA | TCR_ORGN_WBWA
#define TCR_TG_FLAGS	TCR_TG0_4K | TCR_TG1_4K
#define TCR_SMP_FLAGS	TCR_SHARED

#define MAIR(attr, mt)	((attr) << ((mt) * 8))

/*
 * Highest possible physical address supported.
 */
#define PHYS_MASK_SHIFT		(48)
#define PHYS_MASK		((UL(1) << PHYS_MASK_SHIFT) - 1)

#endif /* __ASM_MMU_FLAGS_H */
