#ifndef _DIM_SUM_VIRT_SPACE_H
#define _DIM_SUM_VIRT_SPACE_H

#include <dim-sum/init.h>

#define VM_IOREMAP		BIT(0)
#define VM_NO_GUARD	BIT(1)

#ifndef IOREMAP_MAX_ORDER
#define IOREMAP_MAX_ORDER	(7 + PAGE_SHIFT)	/* 128 pages */
#endif

#define VIRT_SPACE_IOREMAP	0

struct vm_struct {
	void			*addr;
	unsigned long		size;
	phys_addr_t		phys_addr;
};

void __init init_virt_space(void);
struct vm_struct *
get_virt_space(unsigned int space_id, unsigned long size,
				unsigned long align, unsigned long flags);

#endif /* _DIM_SUM_VIRT_SPACE_H */
