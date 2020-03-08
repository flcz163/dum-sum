#include <dim-sum/boot_allotter.h>
#include <dim-sum/cache.h>
#include <dim-sum/init.h>
#include <dim-sum/memory_regions.h>
#include <dim-sum/mm_types.h>
#include <dim-sum/mmu.h>
#include <dim-sum/string.h>

#include <asm/early_map.h>
#include <asm/memory.h>
#include <asm/mmu_context.h>

static void break_pt_l3_sections(pt_l3_t *pt_l3, pt_l4_val_t *pt_l4)
{
	unsigned long page_num = pmd_pfn(*pt_l3);
	int i = 0;

	for (i = 0; i < PTRS_PER_PT_L4; i++) {
		set_pt_l4(pt_l4, pfn_pte(page_num, PAGE_ATTR_KERNEL_EXEC));

		page_num++;
		pt_l4++;
	}
}

/**
 * 处理四级页表映射，类似，不注释
 */
static void alloc_and_set_pt_l4(pt_l3_t *pt_l3, unsigned long addr,
				  unsigned long end, unsigned long page_num,
				  page_attr_t prot,
				  void *(*alloc)(unsigned long size))
{
	pt_l4_val_t *pt_l4;

	if (pt_l3_is_empty(*pt_l3) || pt_l3_is_section(*pt_l3)) {
		pt_l4 = alloc(PTRS_PER_PT_L4 * sizeof(pt_l4_val_t));
		
		if (pt_l3_is_section(*pt_l3))
			break_pt_l3_sections(pt_l3, pt_l4);

		__attach_to_pt_l3(pt_l3, linear_virt_to_phys(pt_l4), PT_L3_TYPE_TABLE);
		flush_tlb_all();
	}
	BUG_ON(pt_l3_is_invalid(*pt_l3));

	pt_l4 = pt_l4_ptr(pt_l3, addr);
	do {
		set_pt_l4(pt_l4, pfn_pte(page_num, prot));

		page_num++;
		pt_l4++;
		addr += PAGE_SIZE;
	} while (addr != end);
}

static void break_pt_l2_sections(pt_l2_t *old_pt_l2, pt_l3_t *pt_l3)
{
	unsigned long addr = pt_l2_page_num(*old_pt_l2) << PAGE_SHIFT;
	page_attr_t prot = page_attr(pt_l2_val(*old_pt_l2) ^ addr);
	int i = 0;

	for (i = 0; i < PTRS_PER_PT_L3; i++) {
		set_pt_l3(pt_l3, pt_l3(addr | page_attr_val(prot)));
		addr += PT_L3_SIZE;
		pt_l3++;
	}
}

static void alloc_and_set_pt_l3(struct memory_map_desc *mm, pt_l2_t *pt_l2,
				  unsigned long addr, unsigned long end,
				  phys_addr_t phys, page_attr_t prot,
				  void *(*alloc)(unsigned long size))
{
	pt_l3_t *pt_l3;
	unsigned long next;

	/**
	 * 该二级页表项是空的
	 * 或者旧的二级页表项是section 描述符，映射了1G的地址块
	 */
	if (pt_l2_is_empty(*pt_l2) || pt_l2_is_section(*pt_l2)) {
		/* 分配PMD表 */
		pt_l3 = alloc(PTRS_PER_PT_L3 * sizeof(pt_l3_t));
		if (pt_l2_is_section(*pt_l2)) /* 原来映射了1G */
			/* 将原来的映射分拆 */
			break_pt_l2_sections(pt_l2, pt_l3);

		/* 指向新的三级页表内存*/
		attach_to_pt_l2(pt_l2, pt_l3);
		/* flush tlb的内容 */
		flush_tlb_all();
	}
	BUG_ON(pt_l2_is_invalid(*pt_l2));

	pt_l3 = pt_l3_ptr(pt_l2, addr);
	do {
		next = pt_l3_promote(addr, end);

		/* 处理2M段映射 */
		if (((addr | next | phys) & ~PT_L3_MASK) == 0) {
			pt_l3_t old_pt_l3 =*pt_l3;
			set_pt_l3(pt_l3, pt_l3(phys |page_attr_val(mk_sect_prot(prot))));
			/**
			 * 如果在boot阶段映射了该区域
			 * 则刷新tlb
			 */
			if (!pt_l3_is_empty(old_pt_l3))
				flush_tlb_all();
		} else {/* 处理四级页表映射 */
			alloc_and_set_pt_l4(pt_l3, addr, next, __phys_to_pgnum(phys),
				       prot, alloc);
		}

		phys += next - addr;
		pt_l3++;
		addr = next;
	} while (addr != end);
}

static void alloc_and_set_pt_l2(struct memory_map_desc *mm,
				pt_l1_t *pt_l1, unsigned long addr, unsigned long end,
				phys_addr_t phys, page_attr_t prot,
				void *(*alloc)(unsigned long size))
{
	pt_l2_t *pt_l2;
	unsigned long next;

	/* 当前一级页表项是全0 */
	if (pt_l1_is_empty(*pt_l1)) {
		/* 进行二级页表内存的分配 */
		pt_l2 = alloc(PTRS_PER_PT_L2 * sizeof(pt_l2_t));
		/* 建立一级页表项和二级页表内存的关系 */
		attach_to_pt_l1(pt_l1, pt_l2);
	}
	BUG_ON(pt_l1_is_invalid(*pt_l1));

	/* addr地址对应的二级页表描述符内存 */
	pt_l2 = pt_l2_ptr(pt_l1, addr);
	/**
	 * 循环，逐一填充二级页表项
	 * 同时分配并初始化下一级页表
	 */
	do {
		/**
		 * 下一个二级页表项对应的地址
		 * 或者结束地址
		 */
		next = pt_l2_promote(addr, end);

		/**
		 * 分配三级页表项
		 * 并且依次处理每一个三级页表项
		 */
		alloc_and_set_pt_l3(mm, pt_l2, addr, next, phys, prot, alloc);

		phys += next - addr;
		pt_l2++;
		addr = next; 
	} while (addr != end);
}

/**
 * 为物理页面建立线性映射
 * 如果有必要，还会为其分配页表
 */
static void  __linear_mapping(struct memory_map_desc *mm, pt_l1_t *pt_l1,
				    phys_addr_t phys, unsigned long virt,
				    phys_addr_t size, page_attr_t prot,
				    void *(*alloc)(unsigned long size))
{
	unsigned long addr, length, end, next;

	/* 地址和长度都需要对齐到页面 */
	addr = virt & PAGE_MASK;
	length = PAGE_ALIGN(size + (virt & ~PAGE_MASK));
	end = addr + length;

	/* 循环处理多个二级页表 */
	do {
		/**
		 * next是下一个一级页表指向的地址
		 * 或者是结束地址
		 */
		next = pt_l1_promote(addr, end);
		/**
		 * 分配二级页表项
		 * 并依次处理每一个二级页表
		 */
		alloc_and_set_pt_l2(mm, pt_l1, addr, next, phys, prot, alloc);
		phys += next - addr;
		pt_l1++;
		addr = next;
	} while (addr != end);
}

/**
 * 在创建映射时，分配各级页表
 * 页表需要按照页面大小对齐
 */
static void __init *alloc_page_table(unsigned long sz)
{
	void *ptr = alloc_boot_mem_permanent(sz, sz);

	BUG_ON(!ptr);
	memset(ptr, 0, sz);

	return ptr;
}

static void __init linear_mapping(phys_addr_t phys, unsigned long virt,
				  phys_addr_t size, page_attr_t prot)
{
	pt_l1_t *pt_l1 = pt_l1_ptr(kern_memory_map.pt_l1, virt & PAGE_MASK);
	/**
	 * 内核的虚拟地址空间从KERNEL_VA_START开始
	 * 低于这个地址就不对了
	 */
	if (virt < KERNEL_VA_START) {
		pr_warn("BUG: failure to create linear-space for %pa at 0x%016lx.\n",
					&phys, virt);
		return;
	}

	__linear_mapping(&kern_memory_map, pt_l1, phys, virt,
			 size, prot, alloc_page_table);
}

void *empty_zero_page;
/**
 * 建立所有物理内存页面的映射
 */
void __init init_linear_mapping(void)
{
	int i;

	/**
	 * 分配0页
	 */
	empty_zero_page = alloc_boot_mem_permanent(PAGE_SIZE, PAGE_SIZE);
	/**
	 * 将identify映射指向0页
	 * 使其失效
	 */
	set_ttbr0(linear_virt_to_phys(empty_zero_page));

	/**
	 * 遍历所有内存块
	 */
	for (i = 0; i < all_memory_regions.cnt; i++) {
		unsigned long start;
		unsigned long end;

		start = all_memory_regions.regions[i].base;
		end = start + all_memory_regions.regions[i].size;

		if (start >= end)
			break;

		/**
		 * 为每一块内存创建线性映射
		 */
		linear_mapping(start, (unsigned long)linear_phys_to_virt(start),
				end - start, PAGE_ATTR_KERNEL_EXEC);
	}
	
	flush_tlb_all();
	cpu_set_default_tcr_t0sz();
}
