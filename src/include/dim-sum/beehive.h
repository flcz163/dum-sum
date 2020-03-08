#ifndef __DIM_SUM_BEEHIVE_H
#define	__DIM_SUM_BEEHIVE_H

#include <dim-sum/accurate_counter.h>
#include <dim-sum/cpu.h>
#include <dim-sum/mem.h>
#include <dim-sum/numa.h>
#include <dim-sum/string.h>

#include <asm/page.h>

#ifndef ARCH_Beehive_MINALIGN
#define ARCH_Beehive_MINALIGN __alignof__(unsigned long long)
#endif

#define ARCH_KMALLOC_MINALIGN __alignof__(unsigned long long)

enum beehive_flag {
	/**
	 * DMA缓存
	 */
	__BEEHIVE_CACHE_DMA,
	/**
	 * 用于缓存对象，在内存紧张时可以回收
	 */
	__BEEHIVE_RECLAIM_ABLE,
	/**
	 * 强制指定，不允许与其他BEEHIVE合并
	 * 用于BOOT BEEHIVE
	 */
	__BEEHIVE_UNMERGEABLE,
	/**
	 * 如果失败就panic
	 * 创建关键对象时指定
	 */
	__BEEHIVE_PANIC,
	/**
	 * 分配对象要求缓存行对齐
	 * 建议而不是强制的
	 */
	__BEEHIVE_HWCACHE_ALIGN,
	/**
	 * 将页面内容进行毒化
	 */
	__BEEHIVE_POISON,
	__BEEHIVE_DEBUG_FREE,
	__BEEHIVE_RED_ZONE,
	__BEEHIVE_STORE_USER,
	BEEHIVE_FLAG_COUNT
};

#define BEEHIVE_CACHE_DMA		(1UL << __BEEHIVE_CACHE_DMA)
#define BEEHIVE_UNMERGEABLE	(1UL << __BEEHIVE_UNMERGEABLE)
#define BEEHIVE_RECLAIM_ABLE	(1UL << __BEEHIVE_RECLAIM_ABLE)
#define BEEHIVE_PANIC			(1UL << __BEEHIVE_PANIC)
#define BEEHIVE_HWCACHE_ALIGN	(1UL << __BEEHIVE_HWCACHE_ALIGN)
#define BEEHIVE_POISON			(1UL << __BEEHIVE_POISON)
#define BEEHIVE_DEBUG_FREE	(1UL << __BEEHIVE_DEBUG_FREE)
#define BEEHIVE_RED_ZONE		(1UL << __BEEHIVE_RED_ZONE)
#define BEEHIVE_STORE_USER	(1UL << __BEEHIVE_STORE_USER)

/**
 * 每个beehive在NUMA内存节点中的信息
 */
struct beehive_node {
	/**
	 * 保护本描述符的锁
	 */
	struct smp_lock list_lock;
	/**
	 * 半满链表中，有多少个对象
	 */
	unsigned long partial_count;
	/**
	 * 半满链表头
	 */
	struct double_list partial_list;
	/**
	 * beehive数量，包含全满及半满的beehive
	 */
	struct accurate_counter beehive_count;
};

/**
 * 每个beehive的CPU缓存信息
 * 用于快速分配和释放
 */
struct beehive_cpu_cache {
	/**
	 * 对象实际大小
	 */
	unsigned int size_solid;
	/**
	 * beehive所在的页面
	 */
	struct page_frame *beehive_page;
	/**
	 * beehive页面中，空闲对象首指针
	 * 如果没有选中缓存的beehive页面，则为NULL
	 */
	void **freelist;
	/**
	 * 在每个空闲对象中，指向下一个对象的指针在对象中的偏移
	 */
	unsigned int next_obj;
	/**
	 * 缓存页所在的节点
	 */
	int node;
};

struct beehive_allotter {
	/**
	 * 引用计数，当多个类似分配器共用本描述符时，增加该计数
	 * 由于全局锁beehive_lock的原因，这里可以不用struct ref_count
	 * 可以减少内存消耗
	 */
	int ref_count;
	/**
	 * 分配器标志，如BEEHIVE_XX
	 */
	unsigned long flags;
	struct {
		/**
		 * 对象实际的大小
		 */
		int size_solid;
		/**
		 * 对象占用的空间大小
		 * 含元数据、调试数据
		 */
		int size_swell;
		const char		*name;
		int order;
		/**
		 * 每个beehive中的内存对象数量
		 */
		int obj_count;
		int free_offset;
		unsigned int		align;
		/* 半满链表中，数量不能低于这个数 */
		unsigned long min_partial;
		void (*ctor)(struct beehive_allotter *, void *);
	} attrs;

	/**
	 * 链接入全局分配器链表
	 */
	struct double_list list;
	/**
	 * 每CPU缓存
	 */
	struct beehive_cpu_cache *cpu_caches[MAX_CPUS];
	/**
	 * NUMA节点缓存
	 */
	struct beehive_node *nodes[MAX_NUMNODES];
};

struct beehive_allotter *beehive_create(const char *name, size_t size,
		size_t align, unsigned long flags,
		void (*ctor)(struct beehive_allotter *, void *));
extern void beehive_destroy(struct beehive_allotter *);
extern void *beehive_alloc(struct beehive_allotter *, paf_t);
static inline void *beehive_zalloc(struct beehive_allotter *k, paf_t flags)
{
	return beehive_alloc(k, flags | __PAF_ZERO);
}
extern void beehive_free(struct beehive_allotter *, void *);

void *kmalloc(size_t size, paf_t flags);
static inline void *kzalloc(size_t size, paf_t flags)
{
	return kmalloc(size, flags | __PAF_ZERO);
}
void kfree(const void *);

#endif /* __DIM_SUM_BEEHIVE_H */
