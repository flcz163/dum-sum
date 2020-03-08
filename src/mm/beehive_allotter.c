#include <dim-sum/beehive.h>
#include <dim-sum/boot_allotter.h>
#include <dim-sum/cache.h>
#include <dim-sum/cpumask.h>
#include <dim-sum/errno.h>
#include <dim-sum/init.h>
#include <dim-sum/magic.h>
#include <dim-sum/mem.h>
#include <dim-sum/mm.h>
#include <dim-sum/mm_types.h>
#include <dim-sum/mutex.h>
#include <dim-sum/numa.h>
#include <dim-sum/page_flags.h>
#include <dim-sum/rwsem.h>
#include <dim-sum/smp_lock.h>
#include <dim-sum/smp_bit_lock.h>
#include <dim-sum/smp.h>
#include <dim-sum/string.h>

#include <asm/barrier.h>

#include "internal.h"

#define DEFAULT_MAX_ORDER 2
#define DEFAULT_MIN_OBJECTS 4

/*
 * 判断是否可以合并的标志
 */
#define BEEHIVE_NEVER_MERGE (BEEHIVE_UNMERGEABLE | BEEHIVE_RED_ZONE | \
		BEEHIVE_POISON | BEEHIVE_STORE_USER)

#define BEEHIVE_MERGE_SAME (BEEHIVE_DEBUG_FREE | BEEHIVE_RECLAIM_ABLE | \
		BEEHIVE_CACHE_DMA)

/**
 * 保护全局分配器链表的锁
 */
struct mutex beehive_lock = MUTEX_INITIALIZER(beehive_lock);
/**
 * 所有分配器的链表头
 */
struct double_list all_beehives = LIST_HEAD_INITIALIZER(all_beehives);

#define MIN_PARTIAL 5

static int beehive_max_order = DEFAULT_MAX_ORDER;
static int beehive_min_objects = DEFAULT_MIN_OBJECTS;

/**
 * 初始化状态
 */
static enum {
	DOWN,
	/**
	 * 基本的数据结构就绪
	 * 但是还得等待伙伴系统就绪
	 */
	EARLY,
	/**
	 * 一切OK，可以调用本模块的API了
	 */
	UP,
} beehive_init_state = DOWN;

/**
 * 用于kmalloc分配的beehive对象
 */
static struct {
	int size;
	char *name;
	char *name_dma;
	struct beehive_allotter *beehive;
	struct beehive_allotter *beehive_dma;
} kmalloc_beehives[] = {
	{ .size = 32, .name = "kmalloc-32", .name_dma = "kmalloc-dma-32" },
	{ .size = 64, .name = "kmalloc-64", .name_dma = "kmalloc-dma-64" },
	{ .size = 96, .name = "kmalloc-96", .name_dma = "kmalloc-dma-96" },
	{ .size = 128, .name = "kmalloc-128", .name_dma = "kmalloc-dma-128" },
	{ .size = 192, .name = "kmalloc-192", .name_dma = "kmalloc-dma-192" },
	{ .size = 256, .name = "kmalloc-256", .name_dma = "kmalloc-dma-256" },
	{ .size = 512, .name = "kmalloc-512", .name_dma = "kmalloc-dma-512" },
	{ .size = 1024, .name = "kmalloc-1024", .name_dma = "kmalloc-dma-1024" },
	{ .size = 2048, .name = "kmalloc-2048", .name_dma = "kmalloc-dma-2048" },
};
/**
 * 注意与前面的数组保持一致
 */
#define MAX_KMALLOC_SIZE 2048

static inline void beehive_page_lock(struct page_frame *page)
{
	smp_bit_lock(PG_locked, &page->flags);
}

static inline void beehive_page_unlock(struct page_frame *page)
{
	smp_bit_unlock(PG_locked, &page->flags);
}

static inline int beehive_page_trylock(struct page_frame *page)
{
	return smp_bit_trylock(PG_locked, &page->flags);
}

static inline void
set_freepointer(struct beehive_allotter *beehive, void *object, void *fp)
{
	*(void **)(object + beehive->attrs.free_offset) = fp;
}

static inline void *
get_freepointer(struct beehive_allotter *beehive, void *object)
{
	return *(void **)(object + beehive->attrs.free_offset);
}

/**
 * 分配beehive页面
 */
static struct page_frame *
alloc_one_beehive(struct beehive_allotter *beehive, paf_t flags, int node_id)
{
	struct page_frame *page;
	struct beehive_node *node;
	void *start;
	void *curr;
	void *p;

	/**
	 * DMA缓存
	 */
	if (beehive->flags & BEEHIVE_CACHE_DMA)
		/**
		 * 从DMA伙伴系统中分配内存
		 */
		flags |= PAF_DMA;

	if (beehive->flags & BEEHIVE_RECLAIM_ABLE)
		flags |= __PAF_RECLAIMABLE;

	flags |= __PAF_BEEHIVE;

	/**
	 * 分配页面
	 */
	if (node_id == -1)
		page = alloc_page_frames(flags, beehive->attrs.order);
	else
		page = __alloc_page_frames(node_id, flags, beehive->attrs.order);

	if (!page)
		goto out;

	ASSERT(node_id < 0 || node_id_of_page(page) == node_id);
	node = beehive->nodes[node_id_of_page(page)];

	accurate_inc(&node->beehive_count);
	page->beehive = beehive;

	start = page_address(page);
	/**
	 * 毒化页面
	 */
	if (unlikely(beehive->flags & BEEHIVE_POISON))
		memset(start, MEMORY_SLICE_INUSE, PAGE_SIZE << beehive->attrs.order);

	/**
	 * 初始化页面中的所有对象指针
	 */
	curr = start;
	if (unlikely(beehive->attrs.ctor))
		beehive->attrs.ctor(beehive, start);
	for (p = start + beehive->attrs.size_swell;
	    p < start + beehive->attrs.obj_count * beehive->attrs.size_swell;
	    p += beehive->attrs.size_swell) {
		if (unlikely(beehive->attrs.ctor))
			beehive->attrs.ctor(beehive, p);
		set_freepointer(beehive, curr, p);
		curr = p;
	}
	set_freepointer(beehive, curr, NULL);

	page->freelist = start;
	page->inuse_count = 0;

out:
	return page;
}

/**
 * 将beehive页面放回伙伴系统
 */
static void discard_one_beehive(struct beehive_allotter *beehive, struct page_frame *page)
{
	struct beehive_node *node = beehive->nodes[node_id_of_page(page)];

	accurate_dec(&node->beehive_count);
	init_page_share_count(page);
	pgflag_clear_beehive(page);
	free_page_frames(page, beehive->attrs.order);
}

/**
 * 将beehive页面加入到半满链表的后面
 */
static void insert_to_partial_behind(struct beehive_node *node, struct page_frame *page)
{
	smp_lock(&node->list_lock);
	node->partial_count++;
	list_init(&page->beehive_list);
	list_insert_behind(&page->beehive_list, &node->partial_list);
	smp_unlock(&node->list_lock);
}

/**
 * 将beehive页面加入到半满链表的前面
 */
static void insert_to_partial_front(struct beehive_node *node, struct page_frame *page)
{
	smp_lock(&node->list_lock);
	node->partial_count++;
	list_init(&page->beehive_list);
	list_insert_front(&page->beehive_list, &node->partial_list);
	smp_unlock(&node->list_lock);
}

/**
 * 将beehive页面从半满链表中摘除
 */
static void delete_from_partial(struct beehive_allotter *beehive,
						struct page_frame *page)
{
	struct beehive_node *node = beehive->nodes[node_id_of_page(page)];

	smp_lock(&node->list_lock);
	list_del(&page->beehive_list);
	list_init(&page->beehive_list);
	node->partial_count--;
	smp_unlock(&node->list_lock);
}

/**
 * 获得半满的页并锁住
 */
static struct page_frame *
pick_and_lock_partial_page(struct beehive_allotter *beehive,
						paf_t flags, int node_id)
{
	struct page_frame *page;
	struct beehive_node *node;

	if (node_id < 0)
		node_id = numa_node_id();
	node = beehive->nodes[node_id];

	if (node->partial_count == 0)
		return NULL;

	smp_lock(&node->list_lock);

	list_for_each_entry(page, &node->partial_list, beehive_list) {
		/**
		 * 避免死锁，用trylock
		 */
		if (beehive_page_trylock(page)) {
			/**
			 * 从半满链表中摘除
			 */
			list_del(&page->beehive_list);
			node->partial_count--;
			pgflag_set_beehive_incache(page);

			smp_unlock(&node->list_lock);
			return page;
		}
	}

	smp_unlock(&node->list_lock);
	return NULL;
}

/**
 * 把beehive从CPU缓存中移除
 */
static void remove_cpu_cache(struct beehive_allotter *beehive,
				struct beehive_cpu_cache *cache)
{
	struct page_frame *page = cache->beehive_page;
	struct beehive_node *node = beehive->nodes[node_id_of_page(page)];

	/**
	 * 将内存对象从缓存链表中移到页面链表中
	 */
	while (unlikely(cache->freelist)) {
		void **object;

		/**
		 * 摘除缓存链表的头结点
		 */
		object = cache->freelist;
		cache->freelist = cache->freelist[cache->next_obj];

		/**
		 * 将头结点放到页面空闲链表中 
		 */
		object[cache->next_obj] = page->freelist;
		page->freelist = object;
		/**
		 * 减少页面中已经分配的对象计数
		 */
		page->inuse_count--;
	}
	/**
	 * 当前CPU缓存再无关联的页面对象
	 */
	cache->beehive_page = NULL;
	/**
	 * 清除标志，表示页面不在beehive CPU缓存中
	 */
	pgflag_clear_beehive_incache(page);

	if (page->inuse_count) {/* 有分配出去的对象 */
		if (page->freelist)/* 也有空闲对象 */
			/**
			 * 那就是半满beehive了，放到半满链表中
			 */
			insert_to_partial_front(node, page);
		/**
		 * 解除页面锁
		 */
		beehive_page_unlock(page);
	} else {/* 全空beehive */
		/**
		 * 节点的半满beehive池有点少，可以补充一点进去
		 */
		if (node->partial_count < MIN_PARTIAL) {
			/**
			 * 将beehive放到链表**后面**
			 * 好处是可以尽量保证页面完整，有利于回收
			 */
			insert_to_partial_behind(node, page);
			beehive_page_unlock(page);
		} else {
			/**
			 * 半满页面已经够了
			 * 解锁后释放给伙伴系统
			 */
			beehive_page_unlock(page);
			discard_one_beehive(beehive, page);
		}
	}
}

static void drain_cpu_cache(void *param)
{
	struct beehive_allotter *beehive = param;
	struct beehive_cpu_cache *cache = beehive->cpu_caches[smp_processor_id()];

	if (likely(cache && cache->beehive_page)) {
		beehive_page_lock(cache->beehive_page);
		remove_cpu_cache(beehive, cache);
	}
}

static void drain_cpu_caches(struct beehive_allotter *beehive)
{
	/**
	 * wait == 1
	 */
	smp_call_for_all(drain_cpu_cache, beehive, 1);
}

/**
 * 判断缓存页是否与所请求的节点编号匹配
 */
static inline int cache_match_node(struct beehive_cpu_cache *cache, int node_id)
{
	if ((node_id < 0) || cache->node == node_id)
		return 1;

	return 0;
}

static void init_cpu_cache(struct beehive_allotter *beehive,
			struct beehive_cpu_cache *cache)
{
	cache->beehive_page = NULL;
	cache->freelist = NULL;
	cache->node = 0;
	cache->next_obj = beehive->attrs.free_offset / sizeof(void *);
	cache->size_solid = beehive->attrs.size_solid;
}

/**
 * 分配Beehive对象的慢速流程
 */
static void *beehive_alloc_nocache(struct beehive_allotter *beehive,
		paf_t pafflags, int node, struct beehive_cpu_cache *cache)
{
	void **object;
	struct page_frame *page;

	/**
	 * 没有缓存的beehive页面
	 * 为它分配一个
	 */
	if (!cache->beehive_page)
		goto got_cache;

	/**
	 * 获取beehive页面的锁
	 */
	beehive_page_lock(cache->beehive_page);

	/**
	 * 本处理缓存的Beehive，其节点与所要求的节点不一致
	 * 换一个Beehive
	 */
	if (unlikely(!cache_match_node(cache, node))) {
		/**
		 * 取消当前活动Beehive，将其加入到NUMA节点的Partial链表中
		 */
		remove_cpu_cache(beehive, cache);
		goto got_cache;
	}

load_freelist:
	object = cache->beehive_page->freelist;
	/**
	 * 当前处理器的Beehive没有空闲对象了
	 * 换一个Beehive
	 */
	if (unlikely(!object)) {
		/**
		 * 取消当前活动Beehive，将其加入到NUMA节点的Partial链表中
		 */
		remove_cpu_cache(beehive, cache);
		goto got_cache;
	}

	/**
	 * 运行到这里，说明本地CPU有空闲对象
	 * 取第一个空闲对象，待返回
	 */
	object = cache->beehive_page->freelist;
	/**
	 * 取下一个空闲对象作为freelist队列头
	 */
	cache->freelist = object[cache->next_obj];
	/**
	 * 将页面中的对象全部移交给缓存管理
	 * 因此其占用对象为**全部**占用
	 */
	cache->beehive_page->inuse_count = beehive->attrs.obj_count;
	cache->beehive_page->freelist = NULL;
	cache->node = node_id_of_page(cache->beehive_page);

	beehive_page_unlock(cache->beehive_page);

	return object;

got_cache:
	/**
	 * 优先从特定NUMA节点中获得一个Partial Beehive
	 */
	page = pick_and_lock_partial_page(beehive, pafflags, node);
	if (page) {
		cache->beehive_page = page;
		goto load_freelist;
	}

	/**
	 * NUMA节点上没有可用Beehive了，必须分配一个新的
	 * 此时需要先打开中断，因为后续的分配可能会睡眠
	 */
	if (pafflags & __PAF_WAIT)
		enable_irq();

	/**
	 * 创建一个新的Beehive并将其初始化
	 */
	page = alloc_one_beehive(beehive, pafflags, node);

	/**
	 * 重新关上中断再试
	 */
	if (pafflags & __PAF_WAIT)
		disable_irq();

	if (page) {
		cache = beehive->cpu_caches[smp_processor_id()];
		if (cache->beehive_page) {
			beehive_page_lock(cache->beehive_page);
			remove_cpu_cache(beehive, cache);
		}
		beehive_page_lock(page);
		pgflag_set_beehive_incache(page);
		cache->beehive_page = page;

		goto load_freelist;
	}

	return NULL;
}

/**
 * 从一个beehive_cache中分配一个对象
 * 在创建beehive_cache时，会从root beehive_cache中分配对象
 */
void *beehive_alloc(struct beehive_allotter *beehive, paf_t pafflags)
{
	void **object;
	unsigned long flags;
	struct beehive_cpu_cache *cache;
	/**
	 * 默认不指定节点编号
	 */
	int node = -1;

	local_irq_save(flags);
	/**
	 * 获取本CPU的beehive_cpu_cache结构
	 */
	cache = beehive->cpu_caches[smp_processor_id()];
	/**
	 * 没有缓存的beehive页面
	 * 或者缓存页没有空间了
	 * 或者缓存的beehive页面与指定的NUMA节点不一致
	 */
	if (unlikely(!cache || !cache->freelist || !cache_match_node(cache, node)))
		/**
		 * 进入慢速分配流程
		 */
		object = beehive_alloc_nocache(beehive, pafflags, node, cache);
	else {
		/**
		 * 获得第一个空闲对象的指针
		 */
		object = cache->freelist;
		/**
		 * 更新空闲指针，使其指向下一个对象
		 */
		cache->freelist = object[cache->next_obj];
	}
	local_irq_restore(flags);

	if (unlikely((pafflags & __PAF_ZERO) && object))
		memset(object, 0, cache->size_solid);

	/**
	 * 返回对象地址
	 */
	return object;
}

/*
 * beehive慢速释放流程
 */
static void beehive_free_nocache(struct beehive_allotter *beehive,
		struct page_frame *page, void *addr, unsigned int next_obj)
{
	bool full;
	void **object = (void *)addr;

	beehive_page_lock(page);

	/**
	 * 该页是否是全满状态
	 */
	full = (page->freelist == NULL);
	/**
	 * 将对象插入到beehive的空闲链表中
	 */
	object[next_obj] = page->freelist;
	page->freelist = object;
	/* 递减对象计数 */
	page->inuse_count--;

	/**
	 * 处于每CPU缓存中的页面
	 */
	if (unlikely(pgflag_beehive_incache(page)))
		goto out_unlock;

	/**
	 * 半满变全空，将其归还给伙伴系统
	 */
	if (unlikely(!page->inuse_count)) {
		/**
		 * Beehive还在NODE节点的Partial链表中
		 * 将其移除
		 */
		if (!full)
			/*
			 * 从半满链表中移除
			 */
			delete_from_partial(beehive, page);

		beehive_page_unlock(page);

		/**
		 * 将页面归还给伙伴系统
		 */
		discard_one_beehive(beehive, page);
		return;
	}

	/**
	 * 全满变半满
	 */
	if (unlikely(full))
		/**
		 * 将其添加到半满队列 
		 * 注意，是放到后面，为什么?
		 */
		insert_to_partial_behind(beehive->nodes[node_id_of_page(page)], page);

out_unlock:
	beehive_page_unlock(page);
	return;
}

void beehive_free(struct beehive_allotter *beehive, void *addr)
{
	struct page_frame *page;
	void **object = (void *)addr;
	unsigned long flags;
	struct beehive_cpu_cache *cache;

	/**
	 * 找到beehive所在的页
	 */
	page = linear_virt_beehive(addr);
	BUG_ON(page->beehive != beehive);

	local_irq_save(flags);

	cache = beehive->cpu_caches[smp_processor_id()];
	/**
	 * 对象属于处理器当前活动BEEHIVE缓存页
	 */
	if (likely(page == cache->beehive_page)) {
		/**
		 * 将对象放回空闲队列
		 * 注意插入和删除的顺序，不然影响性能
		 */
		object[cache->next_obj] = cache->freelist;
		cache->freelist = object;
	} else
		/**
		 * 没办法，只能走慢速释放流程了。
		 */
		beehive_free_nocache(beehive, page, addr, cache->next_obj);

	local_irq_restore(flags);
}

/**
 * 查找一个满足要求的order值
 * 它能容纳指定数量的对象
 * 并且浪费的空间少于指定的比例
 */
static inline int
get_fit_order(int size, int min_objects, int max_order, int ratio)
{
	int order;
	int rem;

	for (order = 0; order <= max_order; order++) {
		unsigned long Beehive_size = PAGE_SIZE << order;

		if (Beehive_size < min_objects * size)
			continue;

		rem = Beehive_size % size;
		if (rem <= Beehive_size / ratio)
			return order;
	}

	return -1;
}

/**
 * 计算对象占用的页面数量
 * 并据此确定分配页面时的order值
 */
static inline int calculate_order(int size)
{
	int order;
	int min_objects;
	int ratio;

	/**
	 * 尽量使空间浪费少
	 */
	min_objects = beehive_min_objects;
	while (min_objects > 1) {
		/* size * 12.5% */
		ratio = 8;
		while (ratio >= 1) {
			order = get_fit_order(size, min_objects,
						beehive_max_order, ratio);
			if (order >= 0)
				return order;
			ratio /= 2;
		}
		min_objects /= 2;
	}

	/*
	 * 对象太大，选择一个大的页面块
	 */
	order = get_fit_order(size, 1, PG_AREA_MAX_ORDER, 1);
	if (order >= 0)
		return order;

	return -E2BIG;
}

/**
 * 计算对象对齐值
 */
static unsigned long calculate_alignment(unsigned long flags,
		unsigned long align, unsigned long size)
{
	/*
	 * BEEHIVE_HWCACHE_ALIGN只是建议对齐硬件缓存行
	 * 如果对齐太小，就减小对齐要求
	 */
	if (flags & BEEHIVE_HWCACHE_ALIGN) {
		unsigned long cache_line = cache_line_size();
		while (size <= cache_line / 2)
			cache_line /= 2;
		align = max(align, cache_line);
	}

	if (align < ARCH_Beehive_MINALIGN)
		align = ARCH_Beehive_MINALIGN;

	/**
	 * 空闲对象指针要求，强制的!
	 */
	return ALIGN(align, sizeof(void *));
}

/**
 * 确定beehive中数据的布局
 */
static int find_out_layout(struct beehive_allotter *beehive)
{
	unsigned long flags = beehive->flags;
	unsigned long size = beehive->attrs.size_solid;
	unsigned long align = beehive->attrs.align;

	/**
	 * 数据后面紧跟下一个对象的指针
	 * 因此需要指针对齐
	 */
	size = ALIGN(size, sizeof(void *));

	/**
	 * 红区占用一个long宽度
	 */
	if (flags & BEEHIVE_RED_ZONE)
		size += sizeof(unsigned long);

	if (((flags & BEEHIVE_POISON) ||beehive->attrs.ctor)) {
		/**
		 * 增加红区位置
		 */
		beehive->attrs.free_offset = size;
		size += sizeof(void *);
	}

	if (flags & BEEHIVE_RED_ZONE)
		size += sizeof(void *);

	align = calculate_alignment(flags, align, beehive->attrs.size_solid);
	size = ALIGN(size, align);
	beehive->attrs.size_swell = size;

	beehive->attrs.order = calculate_order(size);
	if (beehive->attrs.order < 0)
		return 0;

	/**
	 * 计算每个beehive中的对象数量
	 */
	beehive->attrs.obj_count = (PAGE_SIZE << beehive->attrs.order) / size;

	return !!beehive->attrs.obj_count;

}

static struct beehive_allotter *find_similar(size_t size,
		size_t align, unsigned long flags, const char *name,
		void (*ctor)(struct beehive_allotter *, void *))
{
	struct beehive_allotter *beehive;

	if (flags & BEEHIVE_NEVER_MERGE)
		return NULL;

	if (ctor)
		return NULL;

	size = ALIGN(size, sizeof(void *));
	align = calculate_alignment(flags, align, size);
	size = ALIGN(size, align);

	list_for_each_entry(beehive, &all_beehives, list) {
		if (beehive->flags & BEEHIVE_NEVER_MERGE)
			continue;

		if (beehive->attrs.ctor)
			return NULL;

		if (size > beehive->attrs.size_swell)
			continue;

		if ((flags & BEEHIVE_MERGE_SAME) 
			!= (beehive->flags & BEEHIVE_MERGE_SAME))
				continue;

		if ((beehive->attrs.size_swell & ~(align -1))
			!= beehive->attrs.size_swell)
			continue;

		if (beehive->attrs.size_swell - size >= sizeof(void *))
			continue;

		return beehive;
	}

	return NULL;
}

/**
 * 释放分配器的每CPU缓存结构
 */
static void free_cpu_caches(struct beehive_allotter *beehive)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct beehive_cpu_cache *cache = beehive->cpu_caches[cpu];

		if (cache) {
			beehive->cpu_caches[cpu] = NULL;
			/**
			 * 这里可以直接调用kfree释放
			 * 因为使用boot内存分配器的kmalloc分配器，永不会销毁
			 */
			kfree(cache);
		}
	}
}

/**
 * 释放分配器的NODE结构
 */
static void free_nodes(struct beehive_allotter *beehive)
{
	struct beehive_node *node;
	int i;

	for (i = 0; i < MAX_NUMNODES; i++) {
		node = beehive->nodes[i];
		if (node)
			kfree(node);
		beehive->nodes[i] = NULL;
	}
}

static struct beehive_cpu_cache *alloc_beehive_cpu_cache(struct beehive_allotter *beehive,
							int cpu, paf_t flags)
{
	struct beehive_cpu_cache *cache;

	if (beehive_init_state < UP)
		cache = alloc_boot_mem_permanent(sizeof(struct beehive_cpu_cache),
								cache_line_size());
	else
		cache = kmalloc(ALIGN(sizeof(struct beehive_cpu_cache),
				cache_line_size()), flags);

	if (!cache)
		return NULL;

	init_cpu_cache(beehive, cache);
	return cache;
}

static int alloc_beehive_cpu_caches(struct beehive_allotter *beehive, paf_t flags)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct beehive_cpu_cache *cache = beehive->cpu_caches[cpu];

		if (cache)
			continue;

		cache = alloc_beehive_cpu_cache(beehive, cpu, flags);
		if (!cache) {
			free_cpu_caches(beehive);
			return 0;
		}
		beehive->cpu_caches[cpu] = cache;
	}

	return 1;
}

static void init_beehive_node(struct beehive_node *node)
{
	node->partial_count = 0;
	accurate_set(&node->beehive_count, 0);
	smp_lock_init(&node->list_lock);
	list_init(&node->partial_list);
}

static int init_beehive_nodes(struct beehive_allotter *beehive, paf_t pafflags)
{
	int i;

	for (i = 0; i < MAX_NUMNODES; i++) {
		if (beehive_init_state <= EARLY)
			beehive->nodes[i] = alloc_boot_mem_permanent(
					sizeof(struct beehive_node), cache_line_size());
		else
			beehive->nodes[i] =
					kmalloc(sizeof(struct beehive_node), PAF_KERNEL);

		init_beehive_node(beehive->nodes[i]);
	}

	return 1;
}

static int init_beehive(struct beehive_allotter *beehive, paf_t pafflags,
		const char *name, size_t size,
		size_t align, unsigned long flags,
		void (*ctor)(struct beehive_allotter *, void *))
{
	memset(beehive, 0, sizeof(struct beehive_allotter));
	beehive->attrs.name = name;
	beehive->attrs.ctor = ctor;
	beehive->attrs.size_solid = size;
	beehive->attrs.align = align;
	beehive->flags = flags;
	list_init(&beehive->list);

	if (!find_out_layout(beehive))
		goto error;

	beehive->ref_count = 1;

	if (!init_beehive_nodes(beehive, pafflags & ~PAF_DMA))
		goto error;

	if (alloc_beehive_cpu_caches(beehive, pafflags & ~PAF_DMA))
		return 0;

	free_nodes(beehive);
error:
	if (flags & BEEHIVE_PANIC)
		panic("Cannot create beehive %s size=%lu realsize=%u "
			"order=%u offset=%u flags=%lx\n",
			beehive->attrs.name, (unsigned long)size,
			beehive->attrs.size_swell, beehive->attrs.order,
			beehive->attrs.free_offset, flags);

	return 1;
}

/**
 * 创建一个beehive分配器
 */
struct beehive_allotter *beehive_create(const char *name, size_t size,
		size_t align, unsigned long flags,
		void (*ctor)(struct beehive_allotter *, void *))
{
	struct beehive_allotter *beehive;

	mutex_lock(&beehive_lock);
	beehive = find_similar(size, align, flags, name, ctor);
	if (beehive) {
		int cpu;

		beehive->ref_count++;
		/**
		 * 修改对象实际大小
		 * 在清0时有用
		 */
		beehive->attrs.size_solid = max(beehive->attrs.size_solid, (int)size);
		for_each_possible_cpu(cpu)
			beehive->cpu_caches[cpu]->size_solid = beehive->attrs.size_solid;

		mutex_unlock(&beehive_lock);

		/**
		 * 太好了，可以借用其他beehive分配器:)
		 */
		return beehive;
	}

	beehive = kmalloc(sizeof(struct beehive_allotter), PAF_KERNEL);
	if (beehive) {
		if (init_beehive(beehive, PAF_KERNEL, name,
				size, align, flags, ctor) == 0) {
			list_insert_front(&beehive->list, &all_beehives);
			mutex_unlock(&beehive_lock);

			return beehive;
		}
		kfree(beehive);
	}
	mutex_unlock(&beehive_lock);

	if (flags & BEEHIVE_PANIC)
		panic("Cannot create beehive %s\n", name);
	else
		beehive = NULL;

	return beehive;
}

/*
 * 将所有半满链表中的页面释放回伙伴系统
 */
static int
free_partial_list(struct beehive_allotter *beehive, struct beehive_node *node)
{
	int inuse = 0;
	unsigned long flags;
	struct page_frame *page, *h;

	smp_lock_irqsave(&node->list_lock, flags);
	list_for_each_entry_safe(page, h, &node->partial_list, beehive_list) {
		if (!page->inuse_count) {
			list_del(&page->beehive_list);
			discard_one_beehive(beehive, page);
		} else
			inuse++;
	}
	smp_unlock_irqrestore(&node->list_lock, flags);

	return inuse;
}

/**
 * 释放分配器描述符
 */
static inline int free_beehive_desc(struct beehive_allotter *beehive)
{
	struct beehive_node *node;
	int i;

	drain_cpu_caches(beehive);
	free_cpu_caches(beehive);

	for (i = 0; i < MAX_NUMNODES; i++) {
		node = beehive->nodes[i];
		node->partial_count = free_partial_list(beehive, node);
		if (accurate_read(&node->beehive_count))
			return 1;
	}

	free_nodes(beehive);
	kfree(beehive);
	return 0;
}

/**
 * beehive分配器析构
 */
void beehive_destroy(struct beehive_allotter *beehive)
{
	mutex_lock(&beehive_lock);

	beehive->ref_count--;
	/**
	 * 引用计数为0了。可以安全释放描述符。
	 */
	if (!beehive->ref_count) {
		/**
		 * 从全局链表中摘除分配器描述符
		 */
		list_del(&beehive->list);
		mutex_unlock(&beehive_lock);

		free_beehive_desc(beehive);
	} else
		/**
		 * 还有人在持有此分配器，直接解锁退出
		 */
		mutex_unlock(&beehive_lock);
}

static struct beehive_allotter *find_beehive(size_t size, bool dma)
{
	int index;

	for (index = 0; index < ARRAY_SIZE(kmalloc_beehives); index++) {
		if (size > kmalloc_beehives[index].size)
			continue;

		return dma ? kmalloc_beehives[index].beehive_dma
				     : kmalloc_beehives[index].beehive;
	}
	
	return NULL;
}

void *kmalloc(size_t size, paf_t flags)
{
	struct beehive_allotter *beehive;

	/**
	 * 直接分配页面
	 */
	if (unlikely(size > MAX_KMALLOC_SIZE))
		return (void *)alloc_pages_memory(flags, get_order(size));

	/**
	 * 查找合适的beehive
	 */
	beehive = find_beehive(size, flags & PAF_DMA);

	if (unlikely(!beehive))
		return NULL;

	return beehive_alloc(beehive, flags);
}

void kfree(const void *addr)
{
	struct page_frame *page;

	if (unlikely(!addr))
		return;

	page = linear_virt_beehive(addr);
	/**
	 * 通过页面分配器分配的内存
	 */
	if (unlikely(!pgflag_beehive(page))) {
		/**
		 * 直接释放回伙伴系统
		 */
		loosen_page(page);
		return;
	}

	beehive_free(page->beehive, (void *)addr);
}

void *kmalloc_app(int size)
{
	return kmalloc(size, PAF_KERNEL);
}

void kfree_app(void *addr)
{
	kfree(addr);
}

static struct beehive_allotter *__init 
create_kmalloc_beehive(const char *name, size_t size, unsigned long flags)
{
	struct beehive_allotter *beehive;
	size_t align;
	int err;

	beehive = alloc_boot_mem_permanent(sizeof(struct beehive_allotter),
					cache_line_size());

	if (!beehive)
		panic("Out of memory when creating beehive %s\n", name);

	align = calculate_alignment(flags, ARCH_KMALLOC_MINALIGN, size);

	err = init_beehive(beehive, PAF_KERNEL, name, size, align, flags, NULL);
	if (err)
		panic("failure to init beehive %s\n", name);

	list_insert_front(&beehive->list, &all_beehives);

	return beehive;
}

static void __init init_kmalloc(unsigned long flags)
{
	int i;

	/**
	 * 为kmalloc创建默认的beehive对象
	 */
	for (i = 0; i < ARRAY_SIZE(kmalloc_beehives); i++) {
		kmalloc_beehives[i].beehive = create_kmalloc_beehive(
				kmalloc_beehives[i].name, kmalloc_beehives[i].size, flags);

		kmalloc_beehives[i].beehive_dma = create_kmalloc_beehive(
				kmalloc_beehives[i].name_dma, kmalloc_beehives[i].size,
				BEEHIVE_CACHE_DMA | flags);
	}
}

void __init init_beehive_early(void)
{
	beehive_init_state = EARLY;

	/**
	 * 初始化kmalloc需要的beehive对象
	 */
	init_kmalloc(0);
}

void __init init_beehive_allotter(void)
{
	beehive_init_state = UP;
}
