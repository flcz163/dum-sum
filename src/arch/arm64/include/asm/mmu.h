#ifndef __ASM_MMU_H
#define __ASM_MMU_H

/*
 * 软件定义的PTE标志
 * 硬件不用，软件这里借用
 */
#define PTE_VALID		(_AT(pt_l4_val_t, 1) << 0)
#define PTE_DIRTY		(_AT(pt_l4_val_t, 1) << 55)
#define PTE_SPECIAL		(_AT(pt_l4_val_t, 1) << 56)
#define PTE_WRITE		(_AT(pt_l4_val_t, 1) << 57)
#define PTE_PROT_NONE		(_AT(pt_l4_val_t, 1) << 58) /* only when !PTE_VALID */

#ifndef __ASSEMBLY__

#include <dim-sum/smp_lock.h>
#include <asm/page.h>

struct memory_map_desc;

extern pt_l1_t global_pg_dir[PTRS_PER_PT_L1];
extern pt_l1_t idmap_pg_dir[PTRS_PER_PT_L1];
extern struct   memory_map_desc kern_memory_map;

/**
 * ARM64体系结构特定的内存映射描述符
 */
struct arch_memory_map {
	/**
	 * 保护ASID的自旋锁
	 */
	struct smp_lock id_lock;
	/**
	 * ASID，目前未用
	 * 但是以后支持用户态进程时需要
	 */
	unsigned int asid;
};

#define pt_l1_index(addr)		(((addr) >> PT_L1_SHIFT) & (PTRS_PER_PT_L1 - 1))
static inline  pt_l1_t * pt_l1_ptr(pt_l1_t *pt_l1, unsigned long addr)
{
	return pt_l1 + pt_l1_index(addr);
}

#define pt_l3_index(addr)		(((addr) >> PT_L3_SHIFT) & (PTRS_PER_PT_L3 - 1))

static inline pt_l3_t *pt_l2_page_vaddr(pt_l2_t pt_l2)
{
	return linear_phys_to_virt(pt_l2_val(pt_l2) & PHYS_MASK & (s32)PAGE_MASK);
}

static inline pt_l3_t *pt_l3_ptr(pt_l2_t *pt_l2, unsigned long addr)
{
	return (pt_l3_t *)pt_l2_page_vaddr(*pt_l2) + pt_l3_index(addr);
}

extern pt_l2_t *
follow_pt_l2(struct memory_map_desc *desc, unsigned long addr);

extern pt_l3_t *
follow_pt_l3(struct memory_map_desc *desc, unsigned long addr);

extern pt_l4_val_t *
follow_pt_l4(struct memory_map_desc *desc, unsigned long addr);


/*************************分割线************************/

#define INIT_MM_CONTEXT(name) \
	.context.id_lock = SMP_LOCK_UNLOCKED(name.context.id_lock),

#define ASID(mm)	((mm)->context.id & 0xffff)

extern void init_linear_mapping(void);

#define PMD_SECT_VALID		(_AT(pt_l3_val, 1) << 0)
#define PMD_SECT_PROT_NONE	(_AT(pt_l3_val, 1) << 58)
#define PMD_SECT_USER		(_AT(pt_l3_val, 1) << 6)		/* AP[1] */
#define PMD_SECT_RDONLY		(_AT(pt_l3_val, 1) << 7)		/* AP[2] */
#define PMD_SECT_S		(_AT(pt_l3_val, 3) << 8)
#define PMD_SECT_AF		(_AT(pt_l3_val, 1) << 10)
#define PMD_SECT_NG		(_AT(pt_l3_val, 1) << 11)
#define PMD_SECT_PXN		(_AT(pt_l3_val, 1) << 53)
#define PMD_SECT_UXN		(_AT(pt_l3_val, 1) << 54)

#define PMD_ATTRINDX(t)		(_AT(pt_l3_val, (t)) << 2)
#define PMD_ATTRINDX_MASK	(_AT(pt_l3_val, 7) << 2)

#define PROT_DEFAULT		(PTE_TYPE_PAGE | PTE_AF | PTE_SHARED)
#define PROT_SECT_DEFAULT	(PMD_TYPE_SECT | PMD_SECT_AF | PMD_SECT_S)

#define PROT_DEVICE_nGnRE	(PROT_DEFAULT | PTE_PXN | PTE_UXN | PTE_ATTRINDX(MT_DEVICE_nGnRE))
#define PROT_NORMAL_NC		(PROT_DEFAULT | PTE_PXN | PTE_UXN | PTE_ATTRINDX(MT_NORMAL_NC))
#define PROT_NORMAL		(PROT_DEFAULT | PTE_PXN | PTE_UXN | PTE_ATTRINDX(MT_NORMAL))

#define PROT_SECT_DEVICE_nGnRE	(PROT_SECT_DEFAULT | PMD_SECT_PXN | PMD_SECT_UXN | PMD_ATTRINDX(MT_DEVICE_nGnRE))
#define PROT_SECT_NORMAL	(PROT_SECT_DEFAULT | PMD_SECT_PXN | PMD_SECT_UXN | PMD_ATTRINDX(MT_NORMAL))
#define PROT_SECT_NORMAL_EXEC	(PROT_SECT_DEFAULT | PMD_SECT_UXN | PMD_ATTRINDX(MT_NORMAL))

#define PAGE_DEFAULT		(PROT_DEFAULT | PTE_ATTRINDX(MT_NORMAL))

#define PAGE_ATTR_KERNEL		page_attr(PAGE_DEFAULT | PTE_PXN | PTE_UXN | PTE_DIRTY | PTE_WRITE)
#define PAGE_ATTR_KERNEL_EXEC	page_attr(PAGE_DEFAULT | PTE_UXN | PTE_DIRTY | PTE_WRITE)
#define PAGE_ATTR_KERNEL_NOCACHE		(PROT_DEFAULT | PTE_PXN | PTE_UXN | PTE_ATTRINDX(MT_NORMAL_NC))

#define PAGE_ATTR_KERNEL_IO	page_attr(PROT_DEVICE_nGnRE)

#define pte_pfn(pte)		((pt_l4_val(pte) & PHYS_MASK) >> PAGE_SHIFT)

#define pfn_pte(pfn,prot)	(pt_l4(((phys_addr_t)(pfn) << PAGE_SHIFT) | page_attr_val(prot)))

#define pte_none(pte)		(!pt_l4_val(pte))
#define invalidate_pt_l4(mm,addr,ptep)	set_pt_l4(ptep, pt_l4(0))
#define pte_page(pte)		(number_to_page(pte_pfn(pte)))

#define pte_index(addr)		(((addr) >> PAGE_SHIFT) & (PTRS_PER_PT_L4 - 1))

#define pt_l4_ptr(dir,addr)	(pmd_page_vaddr(*(dir)) + pte_index(addr))

#define pte_present(pte)	(!!(pt_l4_val(pte) & (PTE_VALID | PTE_PROT_NONE)))
#define pte_dirty(pte)		(!!(pt_l4_val(pte) & PTE_DIRTY))
#define pte_young(pte)		(!!(pt_l4_val(pte) & PTE_AF))
#define pte_special(pte)	(!!(pt_l4_val(pte) & PTE_SPECIAL))
#define pte_write(pte)		(!!(pt_l4_val(pte) & PTE_WRITE))
#define pte_exec(pte)		(!(pt_l4_val(pte) & PTE_UXN))

#define pte_valid_user(pte) \
	((pt_l4_val(pte) & (PTE_VALID | PTE_USER)) == (PTE_VALID | PTE_USER))
#define pte_valid_not_user(pte) \
	((pt_l4_val(pte) & (PTE_VALID | PTE_USER)) == PTE_VALID)

static inline void set_pt_l4(pt_l4_val_t *pt_l4, pt_l4_val_t val)
{
	*pt_l4 = val;

	/*
	 * 如果PTE无效
	 * 或者是用户态的页表
	 * 则需要屏障
	 */
	if (pte_valid_not_user(val)) {
		dsb(ishst);
		isb();
	}
}

extern void __sync_icache_dcache(pt_l4_val_t pteval, unsigned long addr);

static inline void set_pte_at(struct memory_map_desc *mm, unsigned long addr,
			      pt_l4_val_t *ptep, pt_l4_val_t pte)
{
	if (pte_valid_user(pte)) {
		if (!pte_special(pte) && pte_exec(pte))
			__sync_icache_dcache(pte, addr);
		if (pte_dirty(pte) && pte_write(pte))
			pt_l4_val(pte) &= ~PTE_RDONLY;
		else
			pt_l4_val(pte) |= PTE_RDONLY;
	}

	set_pt_l4(ptep, pte);
}

/**
 * 三级页表直接映射时
 * 构建section映射的属性
 */
static inline page_attr_t mk_sect_prot(page_attr_t prot)
{
	return page_attr(page_attr_val(prot) & ~PTE_TABLE_BIT);
}

#define pmd_write(pmd)		pte_write(pmd_pte(pmd))

#define pmd_pfn(pmd)		(((pt_l3_val(pmd) & PT_L3_MASK) & PHYS_MASK) >> PAGE_SHIFT)
#define pfn_pmd(pfn,prot)	(pt_l3(((phys_addr_t)(pfn) << PAGE_SHIFT) | page_attr_val(prot)))
#define mk_pmd(page,prot)	pfn_pmd(number_of_page(page),prot)

#define pud_write(pud)		pte_write(pud_pte(pud))
#define pt_l2_page_num(pud)		(((pt_l2_val(pud) & PUD_MASK) & PHYS_MASK) >> PAGE_SHIFT)

#define __pgprot_modify(prot,mask,bits) \
	page_attr((page_attr_val(prot) & ~(mask)) | (bits))

/**
 * 修改页表属性
 */
#define pgprot_noncached(prot) \
	__pgprot_modify(prot, PTE_ATTRINDX_MASK, PTE_ATTRINDX(MT_DEVICE_nGnRnE) | PTE_PXN | PTE_UXN)
#define pgprot_writecombine(prot) \
	__pgprot_modify(prot, PTE_ATTRINDX_MASK, PTE_ATTRINDX(MT_NORMAL_NC) | PTE_PXN | PTE_UXN)
#define pgprot_device(prot) \
	__pgprot_modify(prot, PTE_ATTRINDX_MASK, PTE_ATTRINDX(MT_DEVICE_nGnRE) | PTE_PXN | PTE_UXN)

#define pt_l3_is_empty(pmd)		(!pt_l3_val(pmd))
#define pmd_present(pmd)	(pt_l3_val(pmd))

#define pt_l3_is_invalid(pmd)		(!(pt_l3_val(pmd) & 2))

#define pmd_table(pmd)		((pt_l3_val(pmd) & PMD_TYPE_MASK) == \
				 PT_L3_TYPE_TABLE)
#define pt_l3_is_section(pmd)		((pt_l3_val(pmd) & PMD_TYPE_MASK) == \
				 PMD_TYPE_SECT)

#define pt_l2_is_section(pud)		((pt_l2_val(pud) & PUD_TYPE_MASK) == \
				 PUD_TYPE_SECT)
#define pud_table(pud)		((pt_l2_val(pud) & PUD_TYPE_MASK) == \
				 PUD_TYPE_TABLE)

static inline void set_pt_l3(pt_l3_t *pmdp, pt_l3_t pmd)
{
	*pmdp = pmd;
	dsb(ishst);
	isb();
}

static inline pt_l4_val_t *pmd_page_vaddr(pt_l3_t pmd)
{
	return linear_phys_to_virt(pt_l3_val(pmd) & PHYS_MASK & (s32)PAGE_MASK);
}

#define pmd_page(pmd)		number_to_page(__phys_to_pgnum(pt_l3_val(pmd) & PHYS_MASK))

#define pt_l2_is_empty(pud)		(!pt_l2_val(pud))
#define pt_l2_is_invalid(pud)		(!(pt_l2_val(pud) & 2))
#define pud_present(pud)	(pt_l2_val(pud))

static inline void set_pt_l2(pt_l2_t *pudp, pt_l2_t pud)
{
	*pudp = pud;
	dsb(ishst);
	isb();
}

static inline void pud_clear(pt_l2_t *pudp)
{
	set_pt_l2(pudp, pt_l2(0));
}

#define pud_page(pud)		number_to_page(__phys_to_pgnum(pt_l2_val(pud) & PHYS_MASK))

#include <asm-generic/mmu.h>

#endif /* !__ASSEMBLY__ */

#endif /* __ASM_MMU_H */
