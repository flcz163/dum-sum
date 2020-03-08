#include <dim-sum/boot_allotter.h>
#include <dim-sum/cache.h>
#include <dim-sum/cpu.h>
#include <dim-sum/percpu.h>

#include <asm/memory.h>
#include <asm/sections.h>

u64 idmap_t0sz = TCR_T0SZ(VA_BITS);

/**
 * 起始物理地址
 * 该地址是内核镜像的起始地址
 */
phys_addr_t phys_addr_origin = 0;

/**
 * 启动内存
 *      boot_memory_start:	起始地址
 *      boot_memory_end:	结束地址
 */
unsigned long boot_memory_start;
unsigned long boot_memory_end;

/**
 * DMA能读写的最大限制
 */
unsigned long phys_addr_dma_max = GENMASK(32, 32);
unsigned long phys_addr_kernel_max = GENMASK(48, 48);

void __init init_per_cpu_offsets(void)
{
	unsigned long size, i;
	char *ptr;

	/* 静态per-cpu变量的区间长度 */
	size = ALIGN(per_cpu_var_end - per_cpu_var_start, SMP_CACHE_BYTES);

	/* 为per-cpu分配内存 */
	ptr = alloc_boot_mem_permanent(size * (MAX_CPUS - 1), /* -1!! */
					SMP_CACHE_BYTES);

	/* 设置per-cpu的偏移值 */
	per_cpu_var_offsets[0] = 0;
	for (i = 1; i < MAX_CPUS; i++, ptr += size) {
		per_cpu_var_offsets[i] = ptr - per_cpu_var_start;
		memcpy(ptr, per_cpu_var_start, per_cpu_var_end - per_cpu_var_start);
	}
}