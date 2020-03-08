#include <dim-sum/beehive.h>
#include <dim-sum/boot_allotter.h>
#include <dim-sum/kernel.h>
#include <dim-sum/bug.h>
#include <dim-sum/init.h>
#include <dim-sum/mm.h>
#include <dim-sum/virt_space.h>

#include <asm/memory.h>

struct virt_space {
	unsigned long base;
	unsigned long end;
	unsigned long free_base;
	unsigned long free_end;
};

#define MAX_SPACES	16
struct virt_space virt_spaces[MAX_SPACES];

/**
 * 这里实现一个很简单的版本，需要完全重写。
 */
struct vm_struct *
get_virt_space(unsigned int space_id, unsigned long size,
			unsigned long align, unsigned long flags)
{
	struct vm_struct *area;

	if (flags & VM_IOREMAP)
		align = 1ul << clamp_t(int, fls_long(size),
				       PAGE_SHIFT, IOREMAP_MAX_ORDER);

	size = PAGE_ALIGN(size);
	if (unlikely(!size))
		return NULL;

	/**
	 * 这里应当将area链入到内部链表中
	 * 在释放虚拟地址空间时删除
	 * 以后重构
	 */
	area = kmalloc(sizeof(*area), PAF_KERNEL);
	if (unlikely(!area))
		return NULL;

	if (!(flags & VM_NO_GUARD))
		size += PAGE_SIZE;

	virt_spaces[space_id].free_base = 
		ALIGN(virt_spaces[space_id].free_base, align);
	area->addr = (void *)virt_spaces[space_id].free_base;
	area->size = size;
	virt_spaces[space_id].free_base += size;

	BUG_ON(virt_spaces[space_id].free_base >
				virt_spaces[space_id].free_end);

	return area;
}

void __init init_virt_space(void)
{
	virt_spaces[VIRT_SPACE_IOREMAP].free_base = IOREMAP_START;
	virt_spaces[VIRT_SPACE_IOREMAP].free_end = IOREMAP_END;
	virt_spaces[VIRT_SPACE_IOREMAP].base = IOREMAP_START;
	virt_spaces[VIRT_SPACE_IOREMAP].end = IOREMAP_END;
}
