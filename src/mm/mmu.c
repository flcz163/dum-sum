#include <dim-sum/boot_allotter.h>
#include <dim-sum/cache.h>
#include <dim-sum/init.h>
#include <dim-sum/memory_regions.h>
#include <dim-sum/mm_types.h>
#include <dim-sum/mmu.h>
#include <dim-sum/sched.h>
#include <dim-sum/string.h>

#include <asm/early_map.h>
#include <asm/mmu_context.h>
#include <asm/memory.h>

pt_l2_t * follow_pt_l2(struct memory_map_desc *desc, unsigned long addr)
{
	pt_l1_t *pt_l1 = pt_l1_ptr(desc->pt_l1, addr);

	BUG_ON(pt_l1_is_empty(*pt_l1) || pt_l1_is_invalid(*pt_l1));

	return pt_l2_ptr(pt_l1, addr);
}

pt_l3_t * follow_pt_l3(struct memory_map_desc *desc, unsigned long addr)
{
	pt_l2_t *pt_l2 = follow_pt_l2(desc, addr);
	BUG_ON(pt_l2_is_empty(*pt_l2) || pt_l2_is_invalid(*pt_l2));

	return pt_l3_ptr(pt_l2, addr);
}

pt_l4_val_t * follow_pt_l4(struct memory_map_desc *desc, unsigned long addr)
{
	pt_l3_t *pt_l3 = follow_pt_l3(desc, addr);

	BUG_ON(pt_l3_is_empty(*pt_l3) || pt_l3_is_invalid(*pt_l3));

	return pt_l4_ptr(pt_l3, addr);
}
