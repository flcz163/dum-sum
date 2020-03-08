/**
 * boot内存分配器
 * 有点奇怪的是:
 *       这个分配器只有分配函数，没有释放函数
 */
#ifndef __DIM_SUM_BOOT_ALLOTTER_H
#define __DIM_SUM_BOOT_ALLOTTER_H

/**
 * 分配临时性boot内存
 * 一旦调用free_all_bootmem，即不能再使用
 */
void* alloc_boot_mem_temporary(int size, int align);
/**
 * 分配永久性boot内存
 * 调用free_all_bootmem后，仍然能够使用
 */
void* alloc_boot_mem_permanent(int size, int align);
/**
 * 分配永久性内存
 * 可以根据系统内存，弹性的为其分配空间，可大可小
 * 主要用于初始化时，分配大的哈希表
 */
void *alloc_boot_mem_stretch(unsigned long bucket_size,
					    unsigned long max_orders,
					    unsigned int *real_order);

/**
 * 初始化boot内存区域
 */
void init_boot_mem_area(unsigned long start, unsigned long end);

#endif /* __DIM_SUM_BOOT_ALLOTTER_H */
