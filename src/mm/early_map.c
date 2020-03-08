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

/**
 * 用于早期映射的3/4级页表内存
 */
static pt_l4_t *early_map_pt_l4;
static pt_l3_t *early_map_pt_l3;
static pt_l2_t *early_map_pt_l2;

/**
 * 设置early-map页面属性
 */
static void early_map_set_pt_l4(int idx, phys_addr_t phys, page_attr_t flags)
{
	unsigned long addr = early_map_to_virt(idx);
	pt_l4_val_t *pt_l4;

	BUG_ON(idx <= EARLY_MAP_BEGIN || idx >= EARLY_MAP_END);

	pt_l4= follow_pt_l4(&kern_memory_map, addr);

	if (page_attr_val(flags)) {
		set_pt_l4(pt_l4, pfn_pte(phys >> PAGE_SHIFT, flags));
	} else {
		invalidate_pt_l4(&kern_memory_map, addr, pt_l4);
		flush_tlb_kernel_range(addr, addr+PAGE_SIZE);
	}
	flush_cache_all();
}

void early_mapping(int idx, phys_addr_t phys,
		enum early_map_page_type type)
{
	page_attr_t attr;
	switch (type) {
	case EARLY_MAP_IO:
		attr = EARLY_MAP_PAGEATTR_IO;
		break;
	case EARLY_MAP_KERNEL:
		attr = EARLY_MAP_PAGEATTR_KERNEL;
		break;
	case EARLY_MAP_NOCACHE:
		attr = EARLY_MAP_PAGEATTR_NOCACHE;
		break;
	case EARLY_MAP_CLEAR:
		attr = EARLY_MAP_PAGEATTR_NONE;
		break;
	default:
		BUG();
	}

	early_map_set_pt_l4(idx, phys, attr);
}

void __init init_early_map_early(void)
{
	pt_l1_t *pt_l1;
	pt_l2_t *pt_l2;
	pt_l3_t *pt_l3;
	unsigned long addr = EARLY_MAP_VA_START;

	/**
	 * 注意
	 * 这里需要按照页面对齐
	 */
	early_map_pt_l4 = alloc_boot_mem_permanent(PAGE_SIZE, PAGE_SIZE);
	early_map_pt_l3 = alloc_boot_mem_permanent(PAGE_SIZE, PAGE_SIZE);
	early_map_pt_l2 = (pt_l2_t *)early_map_pt_l3;
	
	pt_l1 = pt_l1_ptr(kern_memory_map.pt_l1, addr);
	attach_to_pt_l1(pt_l1, early_map_pt_l2);
	pt_l2 = pt_l2_ptr(pt_l1, addr);
	attach_to_pt_l2(pt_l2, early_map_pt_l3);
	pt_l3 = pt_l3_ptr(pt_l2, addr);
	attach_to_pt_l3(pt_l3, early_map_pt_l4);

	/**
	 * 目前只用了一个页面来进行L4级映射
	 * 因此所有early-map必须位于一个L3级映射之内
	 */
	if ((early_map_to_virt(EARLY_MAP_BEGIN) >> PT_L3_SHIFT)
		     != (early_map_to_virt(EARLY_MAP_END) >> PT_L3_SHIFT)) {
		BUG();
	}

	if ((pt_l3 != follow_pt_l3(&kern_memory_map, early_map_to_virt(EARLY_MAP_BEGIN)))
	     || pt_l3 != follow_pt_l3(&kern_memory_map, early_map_to_virt(EARLY_MAP_END))) {
		BUG();
	}
}
