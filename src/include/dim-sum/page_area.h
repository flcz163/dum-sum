#ifndef _DIM_SUM_PAGE_AREA_H
#define _DIM_SUM_PAGE_AREA_H

#include <dim-sum/board_config.h>
#include <dim-sum/cache.h>
#include <dim-sum/numa.h>
#include <dim-sum/cpu.h>
#include <dim-sum/wait.h>

#include <asm/page.h>

#define PG_AREA_MAX_ORDER 11

/**
 * 内存区类型
 */
enum page_area_type {
	/* 可用于DMA */
	PG_AREA_DMA,
	/* 可在内核中直接访问 */
	PG_AREA_KERNEL,
	/* 可分配给用户，内核需要映射后再访问 */
	PG_AREA_USER,
	/* 类型数量 */
	PG_AREA_COUNT
};
/**
 * 2 << PG_AREA_SHIFT >= PG_AREA_COUNT
 */
#define PG_AREA_SHIFT		2
#define PG_AREA_MASK		((1UL << PG_AREA_SHIFT) -1)

/**
 * 内存管理区页框高速缓存描述符
 */
struct per_cpu_pages {
	/**
	 * 高速缓存中的页框个数
	 */
	int count;
	/**
	 * 下界，低于此就需要补充高速缓存。
	 */
	int low;		/* low watermark, refill needed */
	/**
	 * 上界，高于此则向伙伴系统释放页框。
	 */
	int high;		/* high watermark, emptying needed */
	/**
	 * 当需要增加或者减少高速缓存页框时，操作的页框个数。
	 */
	int batch;		/* chunk size for buddy add/remove */
	/**
	 * 高速缓存中包含的页框描述符链表。
	 */
	struct double_list list;
};

/**
 * 每CPU页面缓存类型
 */
enum {
	PAGE_COLD_CACHE,
	PAGE_HOT_CACHE,
	CPU_CACHE_COUNT,
};

/**
 * 内存管理区描述符
 */
struct page_area {
	/**
	 * 保护该描述符的自旋锁
	 */
	struct smp_lock		lock;
	/**
	 * 管理区中空闲页的数目
	 */
	unsigned long		free_pages;
	/**
	 * Pages_min-管理区中保留页的数目
	 * Page_low-回收页框使用的下界。同时也被管理区分配器为作为阈值使用。
	 * pages_high-回收页框使用的上界，同时也被管理区分配器作为阈值使用。
	 */
	unsigned long		pages_min, pages_low, pages_high;
	/**
	 * 为内存不足保留的页框
	 */
	unsigned long		pages_reserve[PG_AREA_COUNT];
	
	/**
	 * 用于实现单一页框的特殊高速缓存。
	 * 每CPU、每内存管理区都有一个。包含热高速缓存和冷高速缓存。
	 */
	struct per_cpu_page_cache {
		/**
		 * 热高速缓存和冷高速缓存。
		 */
		struct per_cpu_pages cpu_cache[CPU_CACHE_COUNT];	/* 0: hot.  1: cold */
	} aligned_cacheline_in_smp page_caches[MAX_CPUS];

	/**
	 * 标识出管理区中的空闲页框块。
	 * 包含11个元素，被伙伴系统使用。分别对应大小的1,2,4,8,16,32,128,256,512,1024连续空闲块的链表。
	 * 第k个元素标识所有大小为2^k的空闲块。free_list字段指向双向循环链表的头。
	 */
	struct page_free_brick {
		/**
		 * 在内存区域中的空闲页面块链表
		 */
		struct double_list	free_list;
		/**
		 * 页面块数量
		 */
		unsigned long		brick_count;
	} buddies[PG_AREA_MAX_ORDER];

	struct {
		char unused[0];
	} aligned_cacheline_in_smp _pad1;

	/**
	 * 进程等待队列的散列表。这些进程正在等待管理区中的某页。
	 */
	struct wait_queue	* wait_table;

	/**
	 * 等待队列散列表数组的大小。值为2^order
	 */
	unsigned long		wait_table_bits;

	/**
	 * 所属NUMA内存节点。
	 */
	struct memory_node	*mem_node;

	/**
	 * 内存区基本属性
	 */
	struct {
		/**
		 * 指针指向管理区的传统名称：DMA、NORMAL、HighMem
		 */
		char			*name;
		/**
		 * 管理区的第一个页框的下标。
		 */
		unsigned long		pgnum_start;
		/**
		 * 以页为单位的管理区的总大小，包含空洞。
		 */
		unsigned long		pages_swell;
		/**
		 * 以页为单位的管理区的总大小，不包含空洞。
		 */
		unsigned long		pages_solid;
	} attrs; 
};

/**
 * 内存区域，在非连续内存模型中，代表一块内存bank
 * 在NUMA系统中，一般表示一个NUMA节点。
 */
struct memory_node {
	/**
	 * 节点管理区描述符数组
	 */
	struct page_area pg_areas[PG_AREA_COUNT];
	/**
	 * 用于页面分配的内存区域
	 * 按此顺序依次在内存区域中查找合适的页面
	 */
	struct page_area_pool {
		/* +1，表示结束 */
		struct page_area *pg_areas[MAX_NUMNODES * PG_AREA_COUNT + 1];
	} pools[PG_AREA_COUNT];

	struct {
		/**
		 * 节点标识符
		 */
		int node_id;
		/**
		 * 节点的大小，包括空洞
		 */
		unsigned long pages_swell;
		/**
		 * 内存结点的大小，不包含空洞（以页为单位）
		 */
		unsigned long pages_solid; /* total number of physical pages */
		/**
		 * 节点中第一个页框的下标。
		 */
		unsigned long start_pgnum;
	} attrs;
};

/**
 * 内存区的类型
 * 为了节约一个字段，没有在page_area中定义一个type字段
 * 直接计算出来
 */
#define PG_AREA_TYPE(pg_area)		((pg_area) - (pg_area)->mem_node->pg_areas)

/**
 * 获得内存区在page_area_nodes中的索引号
 */
#define NODE_PG_AREA(node, pg_area)	((node << PG_AREA_SHIFT) | pg_area)

#endif /* _DIM_SUM_PAGE_AREA_H */
