#include <dim-sum/boot_allotter.h>
#include <dim-sum/bug.h>
#include <dim-sum/ioremap.h>
#include <dim-sum/mm_types.h>
#include <dim-sum/errno.h>
#include <dim-sum/sched.h>
#include <dim-sum/string.h>
#include <dim-sum/mem.h>
#include <dim-sum/mm.h>
#include <dim-sum/mmu.h>
#include <dim-sum/virt_space.h>

#include <asm/cacheflush.h>
#include <asm/memory.h>
#include <asm/io.h>

void __sync_icache_dcache(pt_l4_val_t pte, unsigned long addr)
{
//
}

#if 0
static inline pt_l2_t *pud_alloc_one(struct memory_map_desc *mm, unsigned long addr)
{
	void *pud = alloc_boot_mem_permanent(0, PAGE_SIZE, PAGE_SIZE);
	memset(pud, 0, PAGE_SIZE);
	
	return (pt_l2_t *)pud;
}

static inline void pud_free(struct memory_map_desc *mm, pt_l2_t *pud)
{
//	BUG_ON((unsigned long)pud & (PAGE_SIZE-1));
//	free_page_memory((unsigned long)pud);
}

int __pud_alloc(struct memory_map_desc *mm, pt_l1_t *pt_l1, unsigned long address)
{
	pt_l2_t *new = pud_alloc_one(mm, address);
	if (!new)
		return -ENOMEM;

	smp_wmb(); /* See comment in __pte_alloc */

	smp_lock(&mm->page_table_lock);
	if (pgd_present(*pt_l1))		/* Another has populated it */
		pud_free(mm, new);
	else
		pgd_populate(mm, pt_l1, new);
	smp_unlock(&mm->page_table_lock);
	return 0;
}
#endif

static pt_l3_t *pmd_alloc_one(struct memory_map_desc *mm, unsigned long addr)
{
	void *pmd = (void *)alloc_zeroed_page_memory(PAF_KERNEL);
	if (pmd)
		memset(pmd, 0, PAGE_SIZE);
	
	return (pt_l3_t *)pmd;
}

static inline void pmd_free(struct memory_map_desc *mm, pt_l3_t *pmd)
{
//	BUG_ON((unsigned long)pmd & (PAGE_SIZE-1));
//	free_page_memory((unsigned long)pmd);
}

int __pmd_alloc(struct memory_map_desc *mm, pt_l2_t *pud, unsigned long address)
{
	pt_l3_t *new = pmd_alloc_one(mm, address);
	if (!new)
		return -ENOMEM;

	smp_wmb(); /* See comment in __pte_alloc */

	smp_lock(&mm->page_table_lock);
	if (!pud_present(*pud)) {
		//mm_inc_nr_pmds(mm);
		attach_to_pt_l2(pud, new);
	} else	/* Another has populated it */
		pmd_free(mm, new);
	smp_unlock(&mm->page_table_lock);
	return 0;
}

static inline pt_l4_val_t *
pte_alloc_one_kernel(struct memory_map_desc *mm, unsigned long addr)
{
	void *pte = (void *)alloc_zeroed_page_memory(PAF_KERNEL);
	memset(pte, 0, PAGE_SIZE);
	
	return (pt_l4_val_t *)pte;
}

static inline void pte_free_kernel(struct memory_map_desc *mm, pt_l4_val_t *pte)
{
#if 0
	if (pte)
		free_page_memory((unsigned long)pte);
#endif
}

int __pte_alloc_kernel(pt_l3_t *pmd, unsigned long address)
{
	pt_l4_val_t *new = pte_alloc_one_kernel(&kern_memory_map, address);
	if (!new)
		return -ENOMEM;

	smp_wmb(); /* See comment in __pte_alloc */

	smp_lock(&kern_memory_map.page_table_lock);
	if (likely(pt_l3_is_empty(*pmd))) {	/* Has another populated it ? */
		attach_to_pt_l3(pmd, new);
		new = NULL;
	}
	smp_unlock(&kern_memory_map.page_table_lock);
	if (new)
		pte_free_kernel(&kern_memory_map, new);
	return 0;
}

static int ioremap_pte_range(pt_l3_t *pmd, unsigned long addr,
		unsigned long end, phys_addr_t phys_addr, page_attr_t prot)
{
	pt_l4_val_t *pte;
	u64 pfn;

	pfn = phys_addr >> PAGE_SHIFT;
	pte = pte_alloc_kernel(pmd, addr);
	if (!pte)
		return -ENOMEM;
	do {
		BUG_ON(!pte_none(*pte));
		set_pte_at(&kern_memory_map, addr, pte, pfn_pte(pfn, prot));
		pfn++;
	} while (pte++, addr += PAGE_SIZE, addr != end);
	return 0;
}

static inline int ioremap_pmd_range(pt_l2_t *pud, unsigned long addr,
		unsigned long end, phys_addr_t phys_addr, page_attr_t prot)
{
	pt_l3_t *pmd;
	unsigned long next;

	phys_addr -= addr;
	pmd = pmd_alloc(&kern_memory_map, pud, addr);
	if (!pmd)
		return -ENOMEM;
	do {
		next = pt_l3_promote(addr, end);

		if (ioremap_pte_range(pmd, addr, next, phys_addr + addr, prot))
			return -ENOMEM;
	} while (pmd++, addr = next, addr != end);
	return 0;
}

#define pud_alloc(mm, pt_l1, address)	(pt_l1)
#define pt_l2_promote(addr, end)		(end)

static inline int ioremap_pud_range(pt_l1_t *pt_l1, unsigned long addr,
		unsigned long end, phys_addr_t phys_addr, page_attr_t prot)
{
	pt_l2_t *pud;
	unsigned long next;

	phys_addr -= addr;
	pud = (pt_l2_t *)pud_alloc(&kern_memory_map, pt_l1, addr);
	if (!pud)
		return -ENOMEM;
	do {
		next = pt_l2_promote(addr, end);

		if (ioremap_pmd_range(pud, addr, next, phys_addr + addr, prot))
			return -ENOMEM;
	} while (pud++, addr = next, addr != end);
	return 0;
}

int ioremap_page_range(unsigned long addr,
		       unsigned long end, phys_addr_t phys_addr, page_attr_t prot)
{
	pt_l1_t *pt_l1;
	unsigned long start;
	unsigned long next;
	int err;

	BUG_ON(addr >= end);

	start = addr;
	phys_addr -= addr;
	pt_l1 = pt_l1_ptr(kern_memory_map.pt_l1, addr);
	do {
		next = pt_l1_promote(addr, end);
		err = ioremap_pud_range(pt_l1, addr, next, phys_addr+addr, prot);
		if (err)
			break;
	} while (pt_l1++, addr = next, addr != end);

	flush_cache_vmap(start, end);

	return err;
}
