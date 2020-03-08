#ifndef __DIM_SUM_MMU_H
#define __DIM_SUM_MMU_H

#include <asm/processor.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

#include <asm/memory.h>
#include <asm/mmu.h>

static inline void attach_to_pt_l2(pt_l2_t *pt_l2, pt_l3_t *pt_l3)
{
	set_pt_l2(pt_l2, pt_l2(linear_virt_to_phys(pt_l3) | PT_L3_TYPE_TABLE));
}

static inline void __attach_to_pt_l3(pt_l3_t *pmdp, phys_addr_t pte,
				  pt_l3_val_t prot)
{
	set_pt_l3(pmdp, pt_l3(pte | prot));
}

static inline void
attach_to_pt_l3(pt_l3_t *pmdp, pt_l4_val_t *ptep)
{
	__attach_to_pt_l3(pmdp, linear_virt_to_phys(ptep), PT_L3_TYPE_TABLE);
}

#endif /* __DIM_SUM_MMU_H */
