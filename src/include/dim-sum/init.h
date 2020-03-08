#ifndef _LINUX_INIT_H
#define _LINUX_INIT_H

#include <linux/compiler.h>
#include <dim-sum/linkage.h>

#define __init		__section(.init.text) __cold notrace
#define __initdata	__section(.init.data)
#define __initconst	__section(.init.rodata)

#ifndef __ASSEMBLY__

/**
 * 系统启动状态
 */
enum boot_states {
	/**
	 * 正在开始启动
	 */
	BOOTING,
	/**
	 * 早期启动
	 */
	EARLY_INIT,
	/**
	 * 内核启动
	 */
	KERN_MALLOC_READY,
	/**
	 * 主体初始化完毕
	 * 准备正式运行
	 */
	KERN_PREPARE_RUN,
	/**
	 * 启动完毕，正常运行
	 */
	KERN_RUNNING,
};
extern enum boot_states boot_state;

extern int __init init_quirk_device(void);
extern int __init pdflush_init(void);
extern void __init init_buffer_module(void);
extern void __init init_blkdev(void);
extern void __init init_chrdev_early(void);
extern void init_lext3(void);
extern int __init init_block_layer(void);
extern int __init init_iosched_noop(void);
int init_journal(void);
extern int __init amba_init(void);
int __init init_disk_early(void);
int __init virtio_blk_init(void);
extern void arch_timer_secondary_init(void);
extern int gic_v2_init(void);
extern void gic_secondary_init(void);
int __init virtio_net_driver_init(void);
extern int __init init_tty(void);
extern int __init init_simple_console(char *buf);
int __init pl011_init(void);
int virtio_init(void);
int __init virtio_mmio_init(void);
extern int init_lwip(void);
void start_arch(void);
void __init init_time(void);

extern int boot_step;
#endif

#endif /* _LINUX_INIT_H */
