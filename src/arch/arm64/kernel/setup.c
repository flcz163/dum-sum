#include <dim-sum/board_config.h>
#include <dim-sum/boot_allotter.h>
#include <dim-sum/init.h>
#include <dim-sum/irq.h>
#include <dim-sum/percpu.h>
#include <dim-sum/smp_lock.h>
#include <clocksource/arm_arch_timer.h>

#include <asm/alternative.h>
#include <asm/cpufeature.h>
#include <asm/cputype.h>
#include <asm/early_map.h>
#include <asm/irqflags.h>
#include <asm/memory.h>
#include <asm/processor.h>

void __init init_IRQ(void)
{
	init_irq_controller();
}

void __init init_time(void)
{
	init_time_arch();
}

void start_arch(void)
{
	/**
	 * 先设置CPU，避免CPU BUG引起故障
	 */
	init_arch_cpu();
	parse_device_configs();
	boot_state = EARLY_INIT;

	/**
	 * 设置boot内存区间
	 * 终于可以分配内存了:)
	 */
	init_boot_mem_area(boot_memory_start, boot_memory_end);

	/**
	 * 固定映射初始化
	 * 在init_mm中创建页表目录项
	 */
	init_early_map_early();
	/**
	 * 为pl011创建简单控制台
	 * 需要进行固定映射，因此放在init_fixmap_early之后
	 */
	init_simple_console("pl011");	
	
	/**
	 * 创建简单控制台后，可以用打印函数进行调试了
	 * Sounds Good:)
	 */

	/**
	 * 此时已经注册了简单控制台
	 * 可以打开async异常
	 * 在异常时可以打印调试
	 */
	enable_async();

	/**
	 * 为per-cpu分配每个CPU上的内存
	 * 并记下每个CPU上的偏移量
	 */
	init_per_cpu_offsets();
	/**
	 * 每个CPU启动时，都应当调用
	 * percpu变量需要
	 */
	set_this_cpu_offset(per_cpu_offset(smp_processor_id()));
}
