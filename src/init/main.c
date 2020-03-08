#include <dim-sum/bus.h>
#include <dim-sum/cache.h>
#include <dim-sum/device.h>
#include <dim-sum/fs.h>
#include <dim-sum/idle.h>
#include <dim-sum/init.h>
#include <dim-sum/irq.h>
#include <dim-sum/mem.h>
#include <dim-sum/mmu.h>
#include <dim-sum/radix-tree.h>
#include <dim-sum/sched.h>
#include <dim-sum/smp.h>
#include <dim-sum/syscall.h>
#include <dim-sum/timer.h>
#include <dim-sum/tty.h>
#include <dim-sum/usr_app_entry.h>
#include <dim-sum/virt_space.h>
#include <dim-sum/workqueue.h>

/**
 * BOOT传递给内核的4个参数地址
 */
u64 aligned_cacheline boot_params[4];
/**
 * 当前系统启动状态
 * 在不同的启动步骤，系统分配内存的行为有所不同
 */
enum boot_states boot_state;

/**
 * 标记主核已经启动
 */
static void __init smp_mark_master(void)
{
	int cpu = smp_processor_id();
	/* 主核总是可用的 */
	mark_cpu_online(cpu, true);
	mark_cpu_present(cpu, true);
	mark_cpu_possible(cpu, true);
}

static __maybe_unused int init_in_process(void *unused);
static void kick_rest(void)
{
	create_process(init_in_process,
			NULL,
			"init_in_process",
			5
		);
}

/**
 * 主核初始化
 */
asmlinkage void __init start_master(void)
{
	boot_state = BOOTING;

	disable_irq();

	/* 为主核设置其活动掩码 */
	smp_mark_master();

	/* 体系结构特定的初始化过程 */
	start_arch();

	init_memory_early();
	init_vfs_early();
	init_sched_early();

	init_linear_mapping();

	/**
	 * 初始化内存子系统
	 * 自此以后，可以调用内存分配API了^_^
	 */
	init_memory();

	boot_state = KERN_MALLOC_READY;

	init_pagecache();
	init_virt_space();
	init_radix_tree();
	init_IRQ();
	init_time();
	init_timer();
	init_sched();
	init_console();

	enable_irq();

	boot_state = KERN_PREPARE_RUN;

	kick_rest();

	cpu_idle();
	//不可能运行到这里来
	BUG();
}

extern void __init_klibc(void);
extern void dim_sum_test(void);

/**
 * 在进程上下文进行初始化工作。
 * 在开中断的情况下运行，此时可以睡眠。
 */
static __maybe_unused int init_in_process(void *unused)
{
	/**
	 * 初始化工作队列
	 * 可睡眠的延迟任务
	 */
	init_sleep_works();

	init_vfs();
	init_file_systems();

	init_bus();
	probe_devices();
	init_tty();

	mount_file_systems();
	/**
	 * 初始化lwip协议栈
	 */
	init_lwip();

	/**
	 * 启动所有从核
	 */
	launch_slave();

	/**
	 * 打开console设备
	 * 将其作为默认的输出设备
	 */
	if (sys_open("/dev/console", O_RDWR, 0) < 0)
		printk("Warning: unable to open an initial console.\n");

	/**
	 * 用于printf
	 */
	(void) sys_dup(0);
	(void) sys_dup(0);
	
	boot_state = KERN_RUNNING;

	__init_klibc();
	dim_sum_test();

	/**
	 * 将控制权交给用户线程
	 */
	usrAppInit();
	
	return 0;
}
