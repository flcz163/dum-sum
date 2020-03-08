/**
 * boot内存分配器
 */
#include <dim-sum/boot_allotter.h>
#include <dim-sum/bug.h>
#include <dim-sum/init.h>
#include <dim-sum/log2.h>
#include <dim-sum/numa.h>
#include <dim-sum/string.h>

#include <asm/page.h>

#include "internal.h"

/**
 * boot内存分配起始和结束地址
 */
static unsigned long __start = 0, __end = 0;

/**
 * 可供分配的低位、高位地址
 */
static unsigned long __low, __high;

/**
 * 在boot内存区中分配临时的内存
 * 系统初始化完成后即返还给内核
 */
void* alloc_boot_mem_temporary(int size, int align)
{
	unsigned long start;

	if (boot_state >= KERN_MALLOC_READY)
		panic("alloc_boot_mem_temporary.\n");

	/* boot内存未初始化 */
	if ((__start == 0)
		&& (__end == 0))
	{
		BUG();
	}

	/**
	 * 处理默认对齐
	 */
	if (align == 0)
		align = sizeof(long);

	/* 预留boot内存分配所占内存空间 */
	start = __high - size;

	/* 按对齐方式，计算boot内存分配实际起始地址 */
	start = round_down(start, align);

	/* 移动boot临时内存分配地址指针 */
	__high = start;
	if (__high < __low)
	{
		BUG();
	}	

	memset((void *)start, 0, (size_t)size);
	
	return (void *)start;
}

/**
 * 在boot内存区中分配永久的内存
 * 系统初始化完成后不会返回给内核
 */
void* alloc_boot_mem_permanent(int size, int align)
{
	unsigned long start;

	if (boot_state >= KERN_MALLOC_READY)
		panic("alloc_boot_mem_permanent.\n");

	/* boot内存未初始化 */
	if ((__start == 0)
		&& (__end == 0))
	{
		BUG();
	}

	/**
	 * 处理默认对齐
	 */
	if (align == 0)
		align = sizeof(long);

	/* 预留boot内存块头所占内存空间 */
	start = __low;

	/* 按对齐方式，计算boot内存分配实际起始地址 */
	start = round_up(start, align);
	
	/* 移动boot永久内存分配地址指针 */
	__low = start + size;
	if (__low > __high)
	{
		BUG();
	}	

	memset((void *)start, 0, (size_t)size);

	return (void *)start;
}

/**
 * 分配永久性内存
 * 可以根据系统内存，弹性的为其分配空间，可大可小
 * 主要用于初始化时，分配大的哈希表
 */
void *__init alloc_boot_mem_stretch(unsigned long bucket_size,
				     unsigned long max_orders,
				     unsigned int *real_order)
{
	unsigned long size;
	void *ret = NULL;

	do {
		size = bucket_size << max_orders;
		ret = alloc_boot_mem_permanent(size, PAGE_SIZE);
	} while (!ret && size > PAGE_SIZE && --max_orders);

	if (!ret)
		panic("Failed to allocate stretch memory\n");

	if (real_order)
		*real_order = max_orders;

	return ret;
}

unsigned long boot_mem_allocated(void)
{
	return __low;
}

/**
 * 初始化Boot内存分配器
 */
void init_boot_mem_area(unsigned long start, unsigned long end)
{
	__start = start;
	__end = end;

	__low = start;
	__high = end;
}
