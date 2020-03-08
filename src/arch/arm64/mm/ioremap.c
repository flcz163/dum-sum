#include <dim-sum/bug.h>
#include <dim-sum/cache.h>
#include <dim-sum/ioremap.h>
#include <dim-sum/mm_types.h>
#include <dim-sum/mmu.h>
#include <dim-sum/virt_space.h>

#include <asm/memory.h>
#include <asm/io.h>

#include <dim-sum/mm.h>

void vunmap(const void *addr)
{
	/* TO-DO */
}

void iounmap(void __iomem *addr)
{
	/* TO-DO */
}

void __iomem *__ioremap(phys_addr_t phys_addr, size_t size, page_attr_t prot)
{
	unsigned long last_addr;
	unsigned long offset = phys_addr & ~PAGE_MASK;
	int err;
	unsigned long addr;
	struct vm_struct *area;

	/*
	 * Page align the space address and size, taking account of any
	 * offset.
	 */
	phys_addr &= PAGE_MASK;
	size = PAGE_ALIGN(size + offset);

	/*
	 * Don't allow wraparound, zero size or outside PHYS_MASK.
	 */
	last_addr = phys_addr + size - 1;
	if (!size || last_addr < phys_addr || (last_addr & ~PHYS_MASK))
		return NULL;

	/*
	 * 不能映射正常的RAM
	 */
	if (pgnum_is_valid(__phys_to_pgnum(phys_addr))) {
		BUG();
		return NULL;
	}

	area = get_virt_space(VIRT_SPACE_IOREMAP, size, 1, VM_IOREMAP);
	if (!area)
		return NULL;
	addr = (unsigned long)area->addr;
	area->phys_addr = phys_addr;


	err = ioremap_page_range(addr, addr + size, phys_addr, prot);
	if (err) {
		vunmap((void *)addr);
		return NULL;
	}

	return (void __iomem *)(offset + addr);
}
