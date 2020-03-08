#ifndef __ASM_PAGE_H
#define __ASM_PAGE_H

#include <dim-sum/types.h>

#define PAGE_SHIFT		12
#define PAGE_SIZE		(_AC(1,UL) << PAGE_SHIFT)
#define PAGE_MASK		(~(PAGE_SIZE-1))
#define PAGE_OFFSET_MASK	(PAGE_SIZE - 1)

#include <asm/mmu_flags.h>

#ifndef __ASSEMBLY__

typedef u64 pt_l4_val_t;
typedef u64 pt_l3_val_t;
typedef u64 pt_l2_val_t;
typedef u64 pt_l1_val_t;

typedef pt_l4_val_t pt_l4_t;
#define pt_l4_val(x)	(x)
#define pt_l4(x)	(x)

typedef pt_l3_val_t pt_l3_t;
#define pt_l3_val(x)	(x)
#define pt_l3(x)	(x)

#if CONFIG_PGTABLE_LEVELS > 3
typedef pt_l2_val_t pt_l2_t;
#define pt_l2_val(x)	(x)
#define pt_l2(x)	(x)
#endif

typedef pt_l1_val_t pt_l1_t;
#define pt_l1_val(x)	(x)
#define __pt_l1(x)	(x)

typedef pt_l4_val_t page_attr_t;
#define page_attr_val(x)	(x)
#define page_attr(x)	(x)


#if CONFIG_PGTABLE_LEVELS == 2
#include <asm-generic/pgtable-nopmd.h>
#elif CONFIG_PGTABLE_LEVELS == 3
#include <asm-generic/pgtable-nopud.h>
#endif

extern int phys_addr_is_valid(phys_addr_t addr);
#define pgnum_is_valid(pfn) phys_addr_is_valid(pfn << PAGE_SHIFT)

#define clear_page(page)	memset((void *)(page), 0, PAGE_SIZE)

#include <asm/memory.h>

#endif /* !__ASSEMBLY__ */

#include <asm-generic/getorder.h>

#endif
