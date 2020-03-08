#ifndef __DIM_SUM_IOPORT_H
#define __DIM_SUM_IOPORT_H

#ifndef __ASSEMBLY__
#include <linux/compiler.h>
#include <dim-sum/types.h>

/**
 * 资源描述符
 */
struct resource {
	resource_size_t start;
	resource_size_t end;
	const char *name;
	unsigned long flags;
	struct resource *parent, *sibling, *child;
};

extern struct resource ioport_resource;
extern struct resource iomem_resource;

#define IORESOURCE_MEM		0x00000200


static inline struct resource * __request_region(struct resource *parent,
		unsigned long start, unsigned long n, const char *name)

{
	return NULL;
}

static inline resource_size_t resource_size(const struct resource *res)
{
	return res->end - res->start + 1;
}

/**
 * 允许驱动程序程序申明自己需要操作的端口。设备的所有IO地址树可以从/proc/ioports获得
 * 		first:		起始端口号
 *		n:			端口个数。
 *		name:		驱动名称。
 */
#define request_region(start,n,name)	__request_region(&ioport_resource, (start), (n), (name))
#define request_mem_region(start,n,name) __request_region(&iomem_resource, (start), (n), (name))

/* Compatibility cruft */
/**
 * 释放分配的IO端口范围。
 */
#define release_region(start,n)	__release_region(&ioport_resource, (start), (n))
#define check_mem_region(start,n)	__check_region(&iomem_resource, (start), (n))

extern int __check_region(resource_size_t, resource_size_t);
static inline void __release_region(struct resource *parent,
					unsigned long start, unsigned long n)
{
}

static inline int __deprecated check_region(unsigned long s, unsigned long n)
{
	return __check_region(s, n);
}
#define release_mem_region(start,n)	__release_region(&iomem_resource, (start), (n))

#endif

#endif	/* __DIM_SUM_IOPORT_H */
