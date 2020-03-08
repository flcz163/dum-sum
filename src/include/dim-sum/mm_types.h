#ifndef _DIMSUM_MM_TYPES_H
#define _DIMSUM_MM_TYPES_H

#include <dim-sum/types.h>
#include <dim-sum/smp_lock.h>
#include <asm/page.h>
#include <asm/memory.h>

struct file_cache_space;

/**
 * 内存映射描述符
 */
struct memory_map_desc {
	/**
	 * 保护映射页表描述符的自旋锁
	 */
	struct smp_lock page_table_lock;
	/**
	 * 一级映射页表指针
	 */
	pt_l1_t *pt_l1;
	
	/**
	 * 体系结构相关的映射描述符
	 * 如ASID
	 */
	struct arch_memory_map map_context;
};

/*************************分割线************************/
struct page_frame {
	/**
	 * 一组标志，也对页框所在的管理区进行编号
	 * 在不支持NUMA的机器上，flags中字段中管理索引占两位，节点索引占一位。
	 * 在支持NUMA的32位机器上，flags中管理索引占用两位。节点数目占6位。
	 * 在支持NUMA的64位机器上，64位的flags字段中，管理区索引占用两位，节点数目占用10位。
	 * 最左边的位数，用于表示该页面所属的内存段编号
	 */
	unsigned long flags;
	
	/**
	 * 页框的引用计数。当小于0表示没有人使用。
	 * Page_count返回_count+1表示正在使用的人数。
	 */
	struct accurate_counter ref_count;		/* Usage count, see below. */
	union {
		/**
		 * 如果映射到用户态，则表示映射到用户进程地址空间的次数 
		 * 初始值为-1。因此实际含义为与当前进程共享此页的进程数量。
		 */
		struct accurate_counter share_count;	
		/**
		 * 对于beehive分配器来说，表示其中的**已经分配出去的** 对象数目
		 */
		unsigned int inuse_count;
	};
	union {
		/**
		 * 该页在某个页面缓存映射内的索引号(第xx页)
		 */
		pgoff_t index;
		/**
		 * beehive分配器的空闲对象指针
		 */
		void *freelist;
	};

	union {
		struct {
			/**
			 * 如果页是空闲的，则该字段由伙伴系统使用。
			 * 当用于伙伴系统时，如果该页是一个2^k的空闲页块的第一个页，那么它的值就是k.
			 * 这样，伙伴系统可以查找相邻的伙伴，以确定是否可以将空闲块合并成2^(k+1)大小的空闲块。
			 */
			unsigned int order;
			/**
			 * 可用于正在使用页的内核成分
			 * 如在缓冲页的情况下，它是一个缓冲器头指针。
			 */
			unsigned long private;				 
			/**
			 * 当页被插入页高速缓存时使用或者当页属于匿名页时使用）。
			 * 		如果mapping字段为空，则该页属于交换高速缓存。
			 *		如果mapping字段不为空，且最低位为1，表示该页为匿名页。同时该字段中存放的是指向anon_vma描述符的指针。
			 *		如果mapping字段不为空，且最低位为0，表示该页为映射页。同时该字段指向对应文件的address_space对象。
			 */
			struct file_cache_space*cache_space;
		};

		/**
		 * 对于beehive第一个页面来说
		 * 表示页面所在的内存分配器
		 */
		struct beehive_allotter *beehive;
		/**
		 * 对于beehive非第一个页面来说
		 * 指向其第一个页面
		 */
		struct page_frame *first_page;
	};

	union {
		struct double_list brick_list;
		struct double_list cache_list;
		/**
		 * 对于beehive分配器来说
		 * 通过此字段将其链接到节点的半满链表中
		 */
		struct double_list beehive_list;
		struct double_list pgcache_list;
		struct double_list lru;
	};
};


struct vm_area_struct {
	/* The first cache line has the info for VMA tree walking. */

	unsigned long vm_start;		/* Our start address within vm_mm. */
	unsigned long vm_end;		/* The first byte after our end address
					   within vm_mm. */
};

#endif /* _DIMSUM_MM_TYPES_H */
