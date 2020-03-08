#ifndef __DIM_SUM_MEM_H
#define __DIM_SUM_MEM_H

#include <dim-sum/init.h>
#include <dim-sum/numa.h>
#include <dim-sum/smp_lock.h>

#include <uapi/dim-sum/mem.h>

#include <asm/page.h>

struct page_frame;

#define VERIFY_READ	0
#define VERIFY_WRITE	1

extern unsigned long max_dma_pgnum, max_pgnum;

/**
 * 页面统计 计数
 * 注意:类型是long而不是unsigned long
 */
struct page_statistics_cpu {
	/**
	 * 文件系统脏页数量
	 */
	long fs_dirty;
	/**
	 * 正在被回写到磁盘的页面数量
	 */
	long fs_wb;

	/**
	 * 映射到进程页表的页面数量
	 */
	long proc_mapped;

	/**
	 * 空闲页面数量
	 */
	long free;
	/**
	 * 页面缓存数量
	 */
	long cache;

	/**
	 * 向块层提交的读写数量
	 */
	long bio_read;
	long bio_write;
};

extern unsigned long __approximate_page_statistics(int offset);
extern void __update_page_statistics(unsigned offset, int count);

#define approximate_page_statistics(member) \
	__approximate_page_statistics(offsetof(struct page_statistics_cpu, member))

#define update_page_statistics(member, count)	\
	__update_page_statistics(offsetof(struct page_statistics_cpu, member), (count))

#define add_page_statistics(member, count) update_page_statistics(member, (count))
#define sub_page_statistics(member, count) update_page_statistics(member, - (count))
#define inc_page_statistics(member)	update_page_statistics(member, 1)
#define dec_page_statistics(member)	update_page_statistics(member, -1)

extern unsigned long total_pages;

void __init init_memory_early(void);
void __init init_memory(void);

#endif /* __DIM_SUM_MEM_H */
