#include <dim-sum/init.h>
#include <dim-sum/types.h>

#include <asm/alternative.h>
#include <asm/cpufeature.h>
#include <asm/processor.h>

/**
 * CPU功能位图
 */
DECLARE_BITMAP(cpu_hwcaps, ARM64_NCAPS);

/**
 * 设置CPU相关选项
 * 例如探测CPU功能
 * 应用补丁等等
 */
void __init init_arch_cpu(void)
{
	/**
	 * 根据CPU特性，确定最终使用何种汇编指令
	 * 来实现特定的功能
	 * 相当于是给CPU打上动态补丁
	 */
	apply_alternatives_all();
}
