#include <dim-sum/board_config.h>
#include <dim-sum/boot_allotter.h>
#include <dim-sum/memory_regions.h>
#include <dim-sum/cpu.h>

#include <asm/memory.h>

#include <asm-generic/sections.h>

/**
 * FDT的物理地址，在汇编中设置其值
 * 目前暂时未使用设备树
 */
phys_addr_t __initdata device_tree_phys;

/***************************华丽的分割线*****************************/

void parse_device_configs(void)
{
	/**
	 * 实际上，我们应当从配置文件或者寄存器中读取此值
	 */
	nr_existent_cpus = 4;
	
	boot_memory_start = (unsigned long)kernel_text_end + SZ_128K;
	boot_memory_end = boot_memory_start + SZ_128M;
	add_memory_regions(SZ_1G, 0x4UL * SZ_1G);
}