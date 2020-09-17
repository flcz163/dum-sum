#include <dim-sum/beehive.h>
#include <dim-sum/boot_allotter.h>
#include <dim-sum/cache.h>
#include <dim-sum/delay.h>
#include <dim-sum/irq.h>
#include <dim-sum/highmem.h>
#include <dim-sum/init.h>
#include <dim-sum/mem.h>
#include <dim-sum/memory_regions.h>
#include <dim-sum/mm.h>
#include <dim-sum/numa.h>
#include <dim-sum/page_area.h>
#include <dim-sum/percpu.h>
#include <dim-sum/sched.h>
#include <dim-sum/smp_lock.h>
#include <dim-sum/stacktrace.h>

#include <asm-generic/current.h>
#include <asm/asm-offsets.h>

#include "internal.h"

/**
 * 每CPU上的页面统计计数
 */
static DEFINE_PER_CPU(struct page_statistics_cpu, statistics) = {0};

/**
 * 内存管理区名称，调试用
 */
static char *pg_area_names[PG_AREA_COUNT] = {
				"DMA",
				"KERNEL",
				"USER" };

/**
 * 内存区域的大小及空洞。
 * 以页为单位
 */
static unsigned long pg_area_size[PG_AREA_COUNT];
static unsigned long pg_area_hole[PG_AREA_COUNT];

/**
 * 全局唯一的NUMA节点。
 */
struct memory_node sole_memory_node;
/**
 * 所有NUMA节点的内存区
 * 参见NODE_PG_AREA宏
 */
struct page_area *page_area_nodes[1 << (PG_AREA_SHIFT + MEM_NODES_SHIFT)];

/**
 * 粗略的统计页面计数
 */
unsigned long __approximate_page_statistics(int offset)
{
	long ret = 0;
	int cpu;

	for_each_online_cpu(cpu) {
		unsigned long in;

		in = (unsigned long)&per_cpu_var(statistics, cpu) + offset;
		ret += *((unsigned long *)in);
	}

	/* !! */
	if (ret < 0)
		ret = 0;

	return ret;
}

/**
 * 更新当前CPU上的页面统计计数
 */
void __update_page_statistics(unsigned offset, int count)
{
	unsigned long flags;
	void* ptr;

	/**
	 * 中断里面也会分配内存，调用此函数
	 */
	local_irq_save(flags);
	ptr = &__get_cpu_var(statistics);
	*(long *)(ptr + offset) += count;
	local_irq_restore(flags);
}

/*
 * 检查页面是否在某个内存区中
 */
static int page_in_pgarea(struct page_area *pg_area, struct page_frame *page)
{
	unsigned long pg_num = number_of_page(page);

	if (pg_num >= (pg_area->attrs.pgnum_start + pg_area->attrs.pages_swell))
	{
		return 0;
	}

	if (pg_num < pg_area->attrs.pgnum_start)
	{
		return 0;
	}

	if (pg_area != page_to_pgarea(page))
	{
		return 0;
	}

	return 1;
}

static void encounter_confused_page(struct page_frame *page)
{
	printk(KERN_EMERG "Bad page state (in process '%s', page %p)\n",
		current->name, page);
	printk(KERN_EMERG "flags:0x%lx space:%p mapcount:%d count:%d\n",
		(unsigned long)page->flags,
		page->cache_space, page_mapcount(page), page_ref_count(page));
	printk(KERN_EMERG "Backtrace:\n");
	dump_stack();
	printk(KERN_EMERG "Trying to fix it up, but a reboot is needed\n");

	page->flags &= ~FREE_PAGE_FLAG;
	set_page_ref_count(page, 0);
	init_page_share_count(page);
	page->cache_space = NULL;
}

/**
 * 从大的块中，切分出一小块页面
 */
static inline void split_bricks(struct page_area *pg_area,
	struct page_frame *page, int low, int high, struct page_free_brick *bricks)
{
	unsigned long size = 1 << high;
	/**
	 * 首先在空闲块链表中删除第一个页框描述符。
	 */
	list_del(&page->brick_list);
	pgflag_clear_buddy(page);
	page->order = 0;
	bricks->brick_count--;

	while (high > low) {
		bricks--;
		high--;
		size >>= 1;
		ASSERT(page_in_pgarea(pg_area, &page[size]));
		list_insert_front(&page[size].brick_list, &bricks->free_list);
		bricks->brick_count++;
		page[size].order = high;
		pgflag_set_buddy(&page[size]);
	}
}

/**
 * 返回第一个被分配的页框的页描述符。如果内存管理区没有所请求大小的一组连续页框，则返回NULL。
 * 在指定的内存管理区中分配页框。它使用每CPU页框高速缓存来处理单一页框请求。
 * pg_area:内存管理区描述符的地址。
 * order：请求分配的内存大小的对数,0表示分配一个页框。
 * paf_flags:分配标志，如果paf_flags中的__paf_COLD标志被置位，那么页框应当从冷高速缓存中获取，否则应当从热高速缓存中获取（只对单一页框请求有意义。）
 */
static struct page_frame *
alloc_page_nocache(struct page_area *pg_area, int order, paf_t paf_flags)
{
	struct page_frame *page = NULL;
	struct page_free_brick * bricks;
	unsigned int current_order;
	unsigned long flags;

	/**
	 * 内存请求没有得到满足，或者是因为请求跨越了几个连续页框，或者是因为被选中的页框高速缓存为空。
	 * 调用__rmqueue函数(因为已经保护了，直接调用__rmqueue即可)从伙伴系统中分配所请求的页框。
	 */
	smp_lock_irqsave(&pg_area->lock, flags);

	/**
	 * 从所请求的order开始，扫描每个可用块链表进行循环搜索。
	 */
	for (current_order = order; current_order < PG_AREA_MAX_ORDER; ++current_order) {
		bricks = pg_area->buddies + current_order;
		/**
		 * 对应的空闲块链表为空，在更大的空闲块链表中进行循环搜索。
		 */
		if (list_is_empty(&bricks->free_list))
			continue;

		/**
		 * 运行到此，说明有合适的空闲块。
		 */
		page = list_container(bricks->free_list.next, struct page_frame, brick_list);
		/**
		 * 如果2^order空闲块链表中没有合适的空闲块，那么就是从更大的空闲链表中分配的。
		 * 将剩余的空闲块分散到合适的链表中去。
		 */
		split_bricks(pg_area, page, order, current_order, bricks);

		/**
		 * 减少空闲管理区的空闲页数量。
		 */
		pg_area->free_pages -= 1UL << order;

		break;
	}

	smp_unlock_irqrestore(&pg_area->lock, flags);

	return page;
}

/**
 * 判断内存区中的空闲页是否满足分配要求
 */
static int pages_enough(struct page_area *pg_area, 
		paf_t paf_mask, int order, unsigned long int try)
{
	long min = try ? pg_area->pages_min : pg_area->pages_low;
	long free_pages = pg_area->free_pages - (1 << order) + 1;
	int reserve_idx = 1;
	int i;

	if (try) {
		/**
		 * 如果paf_high标志被置位。则base除2。
		 * 注意这里不是：min /= 2;
		 * 一般来说，如果paf_mask的__paf_WAIT标志被置位，那么这个标志就会为1
		 * 换句话说，就是指从高端内存中分配。
		 */
		if (paf_mask & __PAF_EMERG)
			min -= min / 2;
		/**
		 * 如果作为参数传递的can_try_harder标志被置位，这个值再减少1/4
		 * can_try_harder=1一般是当：paf_mask中的__paf_WAIT标志被置位，或者当前进程是一个实时进程并且在进程上下文中已经完成了内存分配。
		 */
		if (paf_mask & PAF_NOWAIT)
			min -= min / 4;
	}

	if (paf_mask & PAF_DMA)
		reserve_idx = 0;
	else if (paf_mask & __PAF_USER)
		reserve_idx = 2;

	if (free_pages <= min + pg_area->pages_reserve[reserve_idx])
		return 0;

	/**
	 * 去掉小块，这些页面不满足order分配要求。
	 */
	for (i = 0; i < order; i++) {
		free_pages -= pg_area->buddies[i].brick_count << i;
		min >>= 1;
		if (free_pages <= min)
			return 0;
	}

	return 1;
}

static struct page_frame *
alloc_page_cache(struct page_area *pg_area, paf_t paf_flags)
{
	struct page_frame *page = NULL;
		struct per_cpu_pages *cache;
		unsigned long flags;
	int cache_type;

	if (paf_flags & __PAF_COLD)
		cache_type = PAGE_COLD_CACHE;
	else
		cache_type = PAGE_HOT_CACHE;

	/**
	 * 检查由__paf_COLD标志所标识的内存管理区本地CPU高速缓存是否需要被补充。
	 * 其count字段小于或者等于low
	 */
	cache = &pg_area->page_caches[get_cpu()].cpu_cache[cache_type];
	local_irq_save(flags);
	/**
	 * 当前缓存中的页框数低于low，需要从伙伴系统中补充页框。
	 * 调用rmqueue_bulk函数从伙伴系统中分配batch个单一页框
	 * rmqueue_bulk反复调用__rmqueue，直到缓存的页框达到low。
	 */
	if (cache->count <= cache->low) {
		int i;

		for (i = 0; i < cache->batch; ++i) {
			page = alloc_page_nocache(pg_area, 0, paf_flags);
			if (page == NULL)
				break;
			cache->count++;
			list_insert_behind(&page->cache_list, &cache->list);
		}
	}
	/**
	 * 如果count为正，函数从高速缓存链表中获得一个页框。
	 * count减1
	 */
	if (cache->count) {
		page = list_container(cache->list.next, struct page_frame, cache_list);
		list_del(&page->cache_list);
		cache->count--;
	}
	local_irq_restore(flags);
	/**
	 * 没有和get_cpu配对使用呢？
	 * 这就是内核，外层一定调用了get_cpu。这种代码看起来头疼。
	 */
	put_cpu();

	ASSERT(page_in_pgarea(pg_area, page));

	return page;
}

/**
 * 请求在NUMA节点中分配一组连续页框，它是管理区分配器的核心
 * paf_mask：在内存分配请求中指定的标志
 * order：   连续分配的页框数量的对数(实际分配的是2^order个连续的页框)
 */
struct page_frame *
__alloc_page_frames(int node_id, unsigned int paf_mask, unsigned int order)
{
	struct page_area_pool *pool = 
		MEMORY_NODE(node_id)->pools + (paf_mask & PAF_AREAMASK);
	const int wait = paf_mask & __PAF_WAIT;
	struct page_area **pg_areas, *area;
	struct task_desc *p = current;
	struct page_frame *page;

	ASSERT(order < PG_AREA_MAX_ORDER);
	ASSERT(boot_state >= KERN_MALLOC_READY);

	if (boot_state < KERN_MALLOC_READY)
		panic("failure to call __alloc_page_frames, " \
			"instead of alloc_boot_mem_permanent.\n");

	/**
	 * 备用内存区，以NULL结束
	 */
	pg_areas = pool->pg_areas;
	area = pg_areas[0];
	ASSERT(area != NULL);

try_again:
	if (order == 0) {
		area = pg_areas[0];
		while (area) {
			if (!pages_enough(area, paf_mask, 0, 0)) {
				area++;
				continue;
			}

			page = alloc_page_cache(area, paf_mask);
			if (page)
				goto got_pg;

			area++;
		}

		area = pg_areas[0];
		while (area) {
			if (!pages_enough(area, paf_mask, 0, 1)) {
				area++;
				continue;
			}

			page = alloc_page_cache(area, paf_mask);
			if (page)
				goto got_pg;

			area++;
		}
	}else {
		area = pg_areas[0];
	 	/**
	 	 * 扫描包含在后备缓冲池中的每一个内存区
		 */
		while (area) {
			/**
			 * zone_watermark_ok辅助函数接收几个参数，它们决定内存管理区中空闲页框个数的阀值min。
			 * 这是对内存管理区的第一次扫描，在第一次扫描中，阀值设置为z->pages_lo
			 */
			if (!pages_enough(area, paf_mask, order, 0)) {
				area++;
				continue;
			}

			page = alloc_page_nocache(area, order, paf_mask);
			if (page)
				goto got_pg;

			area++;
		}

		area = pg_areas[0];
		/**
		 * 第一次分配失败
		 * 看来得降低水线再试了。
		 * 执行对内存管理区的第二次扫描
		 */
		while (area) {
			if (!pages_enough(area, paf_mask, order, 1)) {
				area++;
				continue;
			}

			page = alloc_page_nocache(area, order, paf_mask);
			if (page)
				goto got_pg;

			area++;
		}
	}

	/**
	 * 如果产生内存分配的内核控制路径不是一个中断处理程序或者可延迟函数，
	 * 并且它试图回收页框（PF_MEMALLOC，TIF_MEMDIE标志被置位）,那么才对内存管理区进行第三次扫描。
	 */
	if (((p->flags & TASKFLAG_RECLAIM) || unlikely(test_process_flag(PROCFLAG_OOM_KILLED))) && !in_interrupt()) {
		area = pg_areas[0];
		while (area) {
			/**
			 * 本次扫描就不调用zone_watermark_ok，它忽略阀值，这样才能从预留的页中分配页。
			 * 允许这样做，因为是这个进程想要归还页框，那就暂借一点给它吧（呵呵，舍不得孩子套不到狼）。
			 */
			page = alloc_page_nocache(area, order, paf_mask);
			if (page)
				goto got_pg;

			area++;
		}

		/**
		 * 老天保佑，不要运行到这里来，实在是没有内存了。
		 * 不论是高端内存区还是普通内存区、还是DMA内存区，甚至这些管理区中保留的内存都没有了。
		 * 意味着我们的家底都完了。
		 */
		goto nopage;
	}

	/**
	 * 如果paf_mask的__paf_WAIT标志没有被置位，函数就返回NULL。
	 */
	if (!wait)
		goto nopage;

	/**
	 * 被动等待内存释放
	 * 以后这里会添加内存回收代码
	 */
	msleep(1);
	goto try_again;
	
	/**
	 * 既然不用重试，那就执行到nopage返回NULL了。
	 */
nopage:
	if (printk_ratelimit()) {
		printk(KERN_WARNING "%s: page allocation failure."
			" order:%d, mode:0x%x\n",
			p->name, order, paf_mask);
		dump_stack();
	}
	return NULL;

got_pg:
	/**
	 * 将第一个页清除一些标志，将private字段置0，并将页框引用计数器置1。
	 */
	if (page->cache_space || page_mapped_user(page) ||
	    (page->flags & ALLOC_PAGE_FLAG))
		encounter_confused_page(page);

	page->flags &= ~ALL_PAGE_FLAG;
	page->private = 0;
	set_page_ref_count(page, 1);

	/**
	 * 在beehive分配页面时，
	 * 分配了多页，并且需要组成复杂页
	 */
	if (paf_mask & __PAF_BEEHIVE) {
		int i;

		pgflag_set_beehive(page);
		pgflag_clear_additional(page);
		for (i = 1; i < (1 << order); i++) {
			struct page_frame *p = page + i;

			pgflag_set_beehive(p);
			pgflag_set_additional(p);
			p->first_page = page;
		}
	}

	/**
	 * 如果__paf_ZERO标志被置位，则将被分配的区域填充0。
	 */
	if (paf_mask & __PAF_ZERO) {
		int i;

		for(i = 0; i < (1 << order); i++)
			clear_highpage(page + i);
	}

	return page;
}

/**
 * 类似于alloc_pages，但是它返回第一个所分配页的线性地址。
 */
unsigned long alloc_pages_memory(unsigned int paf_mask, unsigned int order)
{
	struct page_frame * page;

	/**
	 * __paf_USER用于分配用户态内存
	 * 只函数不能传递该标志
	 */
	BUG_ON(paf_mask & __PAF_USER);

	page = alloc_page_frames(paf_mask, order);
	if (!page)
		return 0;

	return (unsigned long) page_address(page);
}

/**
 * 用于获取填满0的页框。
 */
fastcall unsigned long alloc_zeroed_page_memory(unsigned int paf_mask)
{
	return alloc_pages_memory(paf_mask | __PAF_ZERO, 0);
}

/**
 * 检查页面是否可以被释放
 * 标志、字段是否正常?
 */
static inline void free_pages_check(struct page_frame *page)
{
	if (page_mapped_user(page) ||page->cache_space != NULL ||
	    page_ref_count(page) != 0 || (page->flags & FREE_PAGE_FLAG))
		encounter_confused_page(page);
}

/**
 * 判断一个页是否可以成为伙伴
 */
static inline int page_is_buddy(struct page_frame *page, int order)
{
       if (pgflag_buddy(page) &&		/* 有buddy标志 */
           (page->order == order) &&		/* 与order匹配 */
           !pgflag_ghost(page) &&		/* 真实存在的页面 */
            page_ref_count(page) == 0)	/* 页面空闲 */
               return 1;

       return 0;
}

/**
 * 按照伙伴系统的策略释放页框。
 * page-被释放块中所包含的第一个页框描述符的地址。
 * pg_area-管理区描述符的地址。
 * order-块大小的对数。
 * base-纯粹由于效率的原因而引入。其实可以从其他三个参数计算得出。
 * 该函数假定调用者已经禁止本地中断并获得了自旋锁。
 */
static noinline void __free_pages_nocache (struct page_frame *page,
		struct page_area *pg_area, unsigned int order)
{
	/**
	 * page_idx包含块中第一个页框的下标。
	 * 这是相对于管理区中的第一个页框而言的。
	 */
	unsigned long page_idx;
	struct page_frame *coalesced;
	unsigned long pg_num;
	/**
	 * order_size用于增加管理区中空闲页框的计数器。
	 */
	int order_size = 1 << order;
	/**
	 * 最小的伙伴页描述符
	 */
	struct page_frame *min_buddy;
	unsigned long min_num;
	int i;

	for (i = 0 ; i < order_size; i++)
		free_pages_check(page + i);

	update_page_statistics(free, 1 << order);

	pg_num = number_of_page(page);
	min_num = pg_num & ~((1 << PG_AREA_MAX_ORDER) - 1);
	min_buddy = number_to_page(min_num);
	page_idx = page - min_buddy;

	/**
	 * 大小为2^k的块，它的线性地址都是2^k * 2 ^ 12的整数倍。
	 * 相应的，它在管理区的偏移应该是2^k倍。
	 */
	BUG_ON(page_idx & (order_size - 1));

	/**
	 * 最多循环10 - order次。每次都将一个块和它的伙伴进行合并。
	 * 每次从最小的块开始，向上合并。
	 */
	while (order < PG_AREA_MAX_ORDER-1) {
		struct page_free_brick *bricks;
		struct page_frame *buddy;
		int buddy_idx;

		/**
		 * 最小块的下标。它是要合并的块的伙伴。
		 * 注意异或操作的用法，是如何用来寻找伙伴的。
		 */
		buddy_idx = (page_idx ^ (1 << order));
		/**
		 * 通过伙伴的下标找到页描述符的地址。
		 */
		buddy = min_buddy + buddy_idx;
		if (!page_in_pgarea(pg_area, buddy))
			break;
		/**
		 * 判断伙伴块是否是大小为order的空闲页框的第一个页。
		 * 首先，伙伴的第一个页必须是空闲的(ref_count == -1)
		 * 同时，必须属于动态内存(PG_reserved被清0,PG_reserved为1表示留给内核或者没有使用)
		 * 最后，其private字段必须是order
		 */
		if (!page_is_buddy(buddy, order))
			break;
		/**
		 * 运行到这里，说明伙伴块可以与当前块合并。
		 */
		/* Move the buddy up one level. */
		/**
		 * 伙伴将被合并，将它从现有链表中取下。
		 */
		list_del(&buddy->brick_list);
		bricks = pg_area->buddies + order;
		bricks->brick_count--;
		pgflag_clear_buddy(page);
		page->order = 0;
		page_idx &= buddy_idx;
		/**
		 * 将合并了的块再与它的伙伴进行合并。
		 */
		order++;
	}

	/**
	 * 伙伴不能与当前块合并。
	 * 将块插入适当的链表，并以块大小的order更新第一个页框的private字段。
	 */
	coalesced = min_buddy + page_idx;
	coalesced->order = order;
	pgflag_set_buddy(coalesced);
	list_insert_front(&coalesced->brick_list, &pg_area->buddies[order].free_list);
	pg_area->buddies[order].brick_count++;
	/**
	 * 增加管理区的空闲页数
	 */
	pg_area->free_pages += order_size;
}

/**
 * 释放单个页框到每CPU页面缓存。
 * page-要释放的页面描述符地址。
 * cold-释放到热高速缓存还是冷高速缓存中
 */
static void fastcall free_page_to_cache(struct page_frame *page, int cache_idx)
{
	/**
	 * 获得page所在的内存区描述符。
	 */
	struct page_area *pg_area = page_to_pgarea(page);
	struct per_cpu_pages *cache;
	unsigned long flags;

	ASSERT(cache_idx < CPU_CACHE_COUNT);

	if (page_mapped_anon(page))
		page->cache_space = NULL;

	/* 缓存页面计数 */
	inc_page_statistics(cache);

	/**
	 * 冷高速缓存还是热高速缓存??
	 */
	cache = &pg_area->page_caches[get_cpu()].cpu_cache[cache_idx];
	local_irq_save(flags);
	/**
	 * 如果缓存的页框太多，就清除一些。
	 * 调用free_pages_nocache将这些页面释放给伙伴系统。
	 */
	if (cache->count >= cache->high) {
		struct page_frame *tmp;
		/**
		 * 实际归还给伙伴系统的页面
		 * 以及预期想要归还的页面
		 */
		int real_count = 0;
		int count = cache->batch;

		/**
		 * 获得内存区的锁
		 */
		smp_lock(&pg_area->lock);
		while (!list_is_empty(&cache->list) && count--) {
			tmp = list_container(cache->list.prev, struct page_frame, cache_list);
			list_del(&tmp->cache_list);
			__free_pages_nocache(tmp, pg_area, 0);
			real_count++;
		}
		smp_unlock(&pg_area->lock);

		/*
		 * 当然，需要更新一下count计数。
		 */
		cache->count -= real_count;
		/**
		 * 更新页面统计计数
		 */
		sub_page_statistics(cache, real_count);
	}
	/**
	 * 将释放的页框加到高速缓存链表上。
	 * 并增加缓存的count字段。
	 */
	list_insert_front(&page->cache_list, &cache->list);
	cache->count++;
	local_irq_restore(flags);
	put_cpu();
}

void fastcall free_cold_page_frame(struct page_frame *page)
{
	free_page_to_cache(page, PAGE_COLD_CACHE);
}

void fastcall free_hot_page_frame(struct page_frame *page)
{
	free_page_to_cache(page, PAGE_HOT_CACHE);
}

/**
 * 释放页框
 */
void free_page_frames(struct page_frame *page, unsigned int order)
{
	/**
	 * 满足如下条件再真正释放
	 * 1、确实在使用中的页面
	 * 2、引用计数递减为0
	 */
	if (!pgflag_ghost(page) && loosen_page_testzero(page)) {
		if (order == 0)
			/* 释放到CPU本地缓存页 */
			free_hot_page_frame(page);
		else {
			struct page_area *pg_area = page_to_pgarea(page);
			unsigned long flags;

			smp_lock_irqsave(&pg_area->lock, flags);
			__free_pages_nocache(page, pg_area, order);
			smp_unlock_irqrestore(&pg_area->lock, flags);
		}
	}
}

/**
 * 释放整页的内存
 */
void free_pages_memory(unsigned long addr, unsigned int order)
{
	if (addr != 0) {
		BUG_ON(!linear_virt_addr_is_valid((void *)addr));
		/**
		 * 二传手，直接释放页框就行了
		 */
		free_page_frames(linear_virt_to_page((void *)addr), order);
	}
}

/**
 * 计算所有区的大小及其空洞
 */
static void __init
calc_page_area_sizes(unsigned long pg_num_start, unsigned long pg_num_end)
{
	unsigned long dma_pgnum_limit = pg_num_start;
	unsigned long kernel_pgnum_limit;
	int i;

	/**
	 * 计算DMA区的大小
	 */
#ifdef CONFIG_PAGE_AREA_DMA
	dma_pgnum_limit = PAGE_NUM_ROUND_DOWN(phys_addr_dma_max);
	pg_area_size[PG_AREA_DMA] = dma_pgnum_limit - pg_num_start;
#endif

	/**
	 * 计算内核和用户区的大小
	 */
	kernel_pgnum_limit =
		min(PAGE_NUM_ROUND_DOWN(phys_addr_kernel_max), pg_num_end);
	pg_area_size[PG_AREA_KERNEL] = kernel_pgnum_limit - dma_pgnum_limit;
	pg_area_size[PG_AREA_USER] = pg_num_end - kernel_pgnum_limit;

	/**
	 * 计算各个内存区的空洞
	 * 先假设所有区域全部都是空洞
	 */
	memcpy(pg_area_hole, pg_area_size, sizeof(pg_area_hole));

	/**
	 * 遍历所有内存块
	 */
	for (i = 0; i < all_memory_regions.cnt; i++) {
		unsigned long start;
		unsigned long end;

		start = PAGE_NUM_ROUND_UP(all_memory_regions.regions[i].base);
		end = PAGE_NUM_ROUND_DOWN(all_memory_regions.regions[i].base
							+ all_memory_regions.regions[i].size);

		if (start >= pg_num_end) /* 这会发生吗 */
			continue;

#ifdef CONFIG_PAGE_AREA_DMA
		if (start < dma_pgnum_limit) {
			unsigned long dma_end = min(end, dma_pgnum_limit);
			/**
			 * 扣除DMA区内有效数据
			 */
			pg_area_hole[PG_AREA_DMA] -= dma_end - start;
		}
#endif

		/**
		 * 包含在KERNEL区中
		 */
		if (end > dma_pgnum_limit) {
			unsigned long kernel_end = min(end, kernel_pgnum_limit);
			unsigned long kernel_start = max(start, dma_pgnum_limit);
			pg_area_hole[PG_AREA_KERNEL] -= kernel_end - kernel_start;
		}

		/**
		 * 也包含在USER区中
		 */
		if (end > kernel_pgnum_limit) {
			unsigned long user_end = end;
			unsigned long user_start = max(start, kernel_pgnum_limit);
			pg_area_hole[PG_AREA_USER] -= user_end - user_start;
		}
	}
}

/**
 * 计算页面等待表的大小
 */
static inline unsigned long calc_wait_table_size(unsigned long pages)
{
	unsigned long size = 1;

	/* 256页面共享一个队列 */
	pages >>= 8;
	while (size < pages)
		size <<= 1;

	/*
	 * 如果页面数量少，那么等待队列较小，会引起过多的冲突
	 * 这里合理的限制最小值，减少冲突
	 * 避免引起线程唤醒风暴
	 */
	size = min(size, 4096UL);

	return max(size, 4UL);
}

static void __init setup_pages(unsigned long pgnum_start, unsigned long count,
						int node_id, unsigned long area_id)
{
	unsigned long end_pgnum = pgnum_start + count;
	unsigned long pgnum;

	for (pgnum = pgnum_start; pgnum < end_pgnum; pgnum++) {
		struct page_frame *page = number_to_page(pgnum);
		unsigned long flags;
		unsigned long sect_id;

		/**
		 * 修改页面标志
		 * 注意标志里面包含节点、内存段编号
		 */
		flags = NODE_PG_AREA(node_id, area_id) << NODE_PG_AREA_SHIFT;
		sect_id = page_num_to_section_nr(pgnum);
		flags |= (sect_id & MEM_SECTIONS_MASK) << MEM_SECTIONS_RSHIFT;
		page->flags = flags;
		set_page_ghost(page);

		set_page_ref_count(page, 0);
		init_page_share_count(page);
		list_init(&page->brick_list);
	}
}

static void __init init_one_node(struct memory_node *node)
{
	unsigned long page_ref_count = 0;
	unsigned long i, j;
	const unsigned long area_alignment = 1UL << PG_AREA_MAX_ORDER;
	int cpu, node_id = node->attrs.node_id;
	unsigned long pgnum_start = node->attrs.start_pgnum;
	unsigned long table_count, table_size;

	/**
	 * 计算NUMA节点的内存页面数量(含空洞)
	 */
	for (i = 0; i < PG_AREA_COUNT; i++)
		page_ref_count += pg_area_size[i];
	node->attrs.pages_swell = page_ref_count;

	/**
	 * 计算NUMA节点的内存页面数量(不含空洞)
	 */
	for (i = 0; i < PG_AREA_COUNT; i++)
		page_ref_count -= pg_area_hole[i];
	node->attrs.pages_solid = page_ref_count;
	printk(KERN_DEBUG "On node %d totalpages: %lu/%lu\n",
			node->attrs.node_id, node->attrs.pages_swell, node->attrs.pages_solid);

	/**
	 * 构建NUMA节点中每个内存区描述符
	 */
	for (i = 0; i < PG_AREA_COUNT; i++) {
		struct page_area *pg_area = node->pg_areas + i;
		unsigned long size_swell, size_solid;
		unsigned long batch;

		smp_lock_init(&pg_area->lock);
		pg_area->attrs.name = pg_area_names[i];
		pg_area->free_pages = 0;
		/**
		 * 建立内存区索引数组
		 */
		page_area_nodes[NODE_PG_AREA(node_id, i)] = pg_area;
		pg_area->mem_node = node;

		/**
		 * 计算每个管理区的页面数量
		 */
		size_solid = size_swell = pg_area_size[i];
		size_solid -= pg_area_hole[i];
		pg_area->attrs.pages_swell = size_swell;
		pg_area->attrs.pages_solid = size_solid;
		pg_area->attrs.pgnum_start = pgnum_start;
		/**
		 * 内存分配保留页面数量
		 * 从PG_AREA_USER开始分配，就会保留得更多
		 * 这里采用静态计算值，以降低水线计算占用的CPU
		 */
		pg_area->pages_reserve[PG_AREA_DMA] = 0;
		pg_area->pages_reserve[PG_AREA_KERNEL] = size_solid / 256;
		pg_area->pages_reserve[PG_AREA_USER] = size_solid / 32;

		/**
		 * 计算每个CPU上面缓存的页面数量
		 * 经验值，根据情况可调整
		 */
		batch = pg_area->attrs.pages_solid / 64;
		batch /= MAX_CPUS;
		if (batch > 32)
			batch = 32;
		if (batch < 1)
			batch = 1;

		/**
		 * 初始化每CPU页面缓存数据结构
		 */
		for (cpu = 0; cpu < MAX_CPUS; cpu++) {
			struct per_cpu_pages *cache;

			cache = &pg_area->page_caches[cpu].cpu_cache[PAGE_HOT_CACHE];	/* hot */
			cache->count = 0;
			cache->low = 2 * batch;
			cache->high = 6 * batch;
			cache->batch = 1 * batch;
			list_init(&cache->list);

			cache = &pg_area->page_caches[cpu].cpu_cache[PAGE_COLD_CACHE];	/* cold */
			cache->count = 0;
			cache->low = 0;
			cache->high = 2 * batch;
			cache->batch = 1 * batch;
			list_init(&cache->list);
		}
		printk(KERN_DEBUG "  %s pg_area: %lu pages, batch:%lu\n",
				pg_area_names[i], size_solid, batch);

		if (!size_swell) /* size_swell? */
			continue;

		/**
		 * 为页面等待队列分配内存
		 */
		table_count = calc_wait_table_size(size_swell);
		table_size = table_count * sizeof(struct wait_queue);
		pg_area->wait_table_bits =	ffz(~table_count);
		pg_area->wait_table = (struct wait_queue *)
			alloc_boot_mem_permanent(table_size, 0);
		/**
		 * 初始化页面等待队列
		 */
		for(j = 0; j < table_count; j++)
			init_waitqueue(pg_area->wait_table + j);

		if ((pgnum_start) & (area_alignment -1))
			panic(KERN_CRIT "BUG: wrong page area alignment, it will crash\n");

		/**
		 * 初始化内存区里面每一个页面
		 */
		setup_pages(pgnum_start, size_swell, node_id, i);

		/**
		 * 初始化空闲块链表
		 */
		for (j = 0; j < PG_AREA_MAX_ORDER; j++) {
			list_init(&pg_area->buddies[j].free_list);
			pg_area->buddies[j].brick_count = 0;
		}

		pgnum_start += size_swell;
	}
}

/**
 * 构建节点的后备内存区
 */
static void __init init_page_area_pool(struct memory_node *node)
{
	int i, j, k, local_node;
	unsigned int area_type;

	local_node = node->attrs.node_id;
	memset(node->pools, 0, sizeof(node->pools));

	for (i = 0; i < PAF_AREATYPES; i++) {
		struct page_area_pool *pool;
		int idx = 0;

		pool = node->pools + i;

		area_type = PG_AREA_KERNEL;
		if (i & __PAF_USER)
			area_type = PG_AREA_USER;
		if (i & PAF_DMA)
			area_type = PG_AREA_DMA;

		for (j = 0; j < MAX_NUMNODES; j++) {
			int __maybe_unused node_id = (local_node + j) % MAX_NUMNODES;

			for (k = 0; k <= area_type; k++) {
				struct page_area *pg_area;
				
				pg_area = MEMORY_NODE(node_id)->pg_areas + k;
				if (pg_area->attrs.pages_solid)
					pool->pg_areas[idx++] = pg_area;
			}
		}

		pool->pg_areas[idx] = NULL;
	}
}

void __init init_page_allotter(void)
{
	unsigned long pg_num_min, pg_num_max;
	unsigned int node_id;

	pg_num_min = PAGE_NUM_ROUND_UP(min_phys_addr());
	pg_num_max = PAGE_NUM_ROUND_DOWN(max_phys_addr());
	/**
	 * 计算所有区的大小及其空洞
	 */
	calc_page_area_sizes(pg_num_min, pg_num_max);

	/**
	 * 遍历所有NUMA节点
	 */
	for (node_id = 0; node_id < num_possible_nodes(); node_id++) {
		struct memory_node *node = MEMORY_NODE(node_id);

		/**
		 * 这里应该根据实际情况处理，暂且这样
		 */
		node->attrs.start_pgnum = pg_num_min;
		init_one_node(node);
	}

	/**
	 * 为每一个NUMA节点，构建其后备内存区
	 */
	for (node_id = 0; node_id < num_possible_nodes(); node_id++) {
		struct memory_node *node = MEMORY_NODE(node_id);

		init_page_area_pool(node);
	}
}
