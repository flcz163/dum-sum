#ifndef __DIM_SUM_PAGE_FLAGS_H
#define __DIM_SUM_PAGE_FLAGS_H

/**
 * 页面描述符的标志位
 */
enum {
	/**
	 * 目前未被内核管理并使用的页
	 * 	1、页面是空洞
	 * 	2、或者被boot占用
	 */
	PG_GHOST,
	/**
	 * 页面属于BEEHIVE内存分配器
	 */
	PG_BEEHIVE,
	/**
	 * 页面锁定标志
	 * 对文件页缓存来说，表示是否有人在锁定该缓存页
	 * 对BEEHIVE来说，表示是否正在操作该页中的beehive
	 */
	PG_locked,
	PG_private,
	/**
	 * 是否属于伙伴系统管理
	 */
	PG_buddy = PG_private,
	PG_ADDITIONAL = PG_private,
	PG_HASBUFFER = PG_private,
	/**
	 * 是否为活动的缓存页
	 */
	PG_active,
	/**
	 * beehive分配器中的每CPU缓存页面
	 */
	PG_BEEHIVE_INCACHE = PG_active,
	/**
	 * 页面是否最新，即从磁盘读入成功
	 */
	PG_uptodate,
	/**
	 * 磁盘页面读写过程中出现了错误
	 */
	PG_error,
	/**
	 * 在文件系统之外，将页面置为脏，待文件系统处理
	 */
	PG_pending_dirty,
	/**
	 * 已经向块设备提交了回写请求
	 */
	PG_writeback,
	/**
	 * 缓存页全部与磁盘块映射，无空洞
	 */
	PG_mappedtodisk,
	PG_dirty,
	PG_compound,
	PG_reclaim,
	PG_arch_1,
	PGFLAG_COUNT
};

#define FREE_PAGE_FLAG ((1UL << PG_private) | (1UL << PG_locked) \
			| (1UL << PG_active) | (1UL << PG_reclaim) | (1UL << PG_BEEHIVE) \
			| (1UL << PG_writeback) || (1UL << PG_dirty))

#define ALLOC_PAGE_FLAG ((1UL << PG_private) | (1UL << PG_locked) \
			| (1UL << PG_dirty) | (1UL << PG_reclaim) | (1UL << PG_writeback))

#define ALL_PAGE_FLAG GENMASK(PGFLAG_COUNT - 1, 0)

/**
 * NUMA节点编号、内存区编号在flags中的偏移量
 * 最左边的位用于保存这些编号
 */
#define NODE_PG_AREA_SHIFT (sizeof(unsigned long) * 8 - MEM_NODES_SHIFT - PG_AREA_SHIFT)

#define pgflag_map_to_disk(page)	test_bit(PG_mappedtodisk, &(page)->flags)
#define set_page_map_to_disk(page) atomic_set_bit(PG_mappedtodisk, &(page)->flags)
#define clear_page_map_to_disk(page) atomic_clear_bit(PG_mappedtodisk, &(page)->flags)

#define pgflag_pending_dirty(page)	test_bit(PG_pending_dirty, &(page)->flags)
#define set_page_pending_dirty(page)	atomic_set_bit(PG_pending_dirty, &(page)->flags)
#define clear_page_pending_dirty(page)	atomic_clear_bit(PG_pending_dirty, &(page)->flags)

#define clear_page_ghost(page)	__clear_bit(PG_GHOST, &(page)->flags)
#define pgflag_ghost(page)	test_bit(PG_GHOST, &(page)->flags)
#define set_page_ghost(page)	atomic_set_bit(PG_GHOST, &(page)->flags)

#define pgflag_error(page)		test_bit(PG_error, &(page)->flags)
#define set_page_error(page)	atomic_set_bit(PG_error, &(page)->flags)
#define ClearPageError(page)	atomic_clear_bit(PG_error, &(page)->flags)


#define pgflag_uptodate(page)	test_bit(PG_uptodate, &(page)->flags)
#define set_page_uptodate(page)	atomic_set_bit(PG_uptodate, &(page)->flags)
#define clear_page_uptodate(page)	atomic_clear_bit(PG_uptodate, &(page)->flags)

#define pgflag_locked(page)		\
		test_bit(PG_locked, &(page)->flags)
#define pgflag_set_locked(page)		\
		atomic_set_bit(PG_locked, &(page)->flags)
#define pgflag_test_and_set_locked(page)		\
		atomic_test_and_set_bit(PG_locked, &(page)->flags)
#define ClearPageLocked(page)		\
		atomic_clear_bit(PG_locked, &(page)->flags)
#define TestClearPageLocked(page)	\
		atomic_test_and_clear_bit(PG_locked, &(page)->flags)
#define SetPagePrivate(page)	atomic_set_bit(PG_private, &(page)->flags)
#define ClearPagePrivate(page)	atomic_clear_bit(PG_private, &(page)->flags)
#define pgflag_private(page)	test_bit(PG_private, &(page)->flags)
#define pgflag_buddy(page)	test_bit(PG_buddy, &(page)->flags)
#define pgflag_set_buddy(page)	atomic_set_bit(PG_buddy, &(page)->flags)
#define pgflag_clear_buddy(page)	atomic_clear_bit(PG_buddy, &(page)->flags)
#define __SetPagePrivate(page)  __set_bit(PG_private, &(page)->flags)
#define __ClearPagePrivate(page) __clear_bit(PG_private, &(page)->flags)
#define pgflag_beehive(page)		test_bit(PG_BEEHIVE, &(page)->flags)
#define pgflag_set_beehive(page)	__set_bit(PG_BEEHIVE, &(page)->flags)
#define pgflag_clear_beehive(page)	__clear_bit(PG_BEEHIVE, &(page)->flags)
#define pgflag_additional(page)		test_bit(PG_ADDITIONAL, &(page)->flags)
#define pgflag_set_additional(page)	__set_bit(PG_ADDITIONAL, &(page)->flags)
#define pgflag_clear_additional(page)	__clear_bit(PG_ADDITIONAL, &(page)->flags)

#define pgflag_beehive_incache(page) \
	test_bit(PG_BEEHIVE_INCACHE, &(page)->flags)
#define pgflag_set_beehive_incache(page) \
	__set_bit(PG_BEEHIVE_INCACHE, &(page)->flags)
#define pgflag_clear_beehive_incache(page) \
	__clear_bit(PG_BEEHIVE_INCACHE, &(page)->flags)



int test_clear_page_writeback(struct page_frame *page);
int test_set_page_writeback(struct page_frame *page);
#define pgflag_writeback(page)	test_bit(PG_writeback, &(page)->flags)
#define SetPageWriteback(page)						\
	do {								\
		if (!atomic_test_and_set_bit(PG_writeback,			\
				&(page)->flags))			\
			inc_page_statistics(fs_wb);			\
	} while (0)
#define pgflag_test_set_writeback(page)					\
	({								\
		int ret;						\
		ret = atomic_test_and_set_bit(PG_writeback,			\
					&(page)->flags);		\
		if (!ret)						\
			inc_page_statistics(fs_wb);			\
		ret;							\
	})
#define ClearPageWriteback(page)					\
	do {								\
		if (atomic_test_and_clear_bit(PG_writeback,			\
				&(page)->flags))			\
			dec_page_statistics(fs_wb);			\
	} while (0)
#define pgflag_test_clear_writeback(page)					\
	({								\
		int ret;						\
		ret = atomic_test_and_clear_bit(PG_writeback,			\
				&(page)->flags);			\
		if (ret)						\
			dec_page_statistics(fs_wb);			\
		ret;							\
	})

static inline void set_page_writeback(struct page_frame *page)
{
	test_set_page_writeback(page);
}

#define pgflag_dirty(page)		test_bit(PG_dirty, &(page)->flags)
#define set_pageflag_dirty(page)	atomic_set_bit(PG_dirty, &(page)->flags)
#define clear_pageflag_dirty(page)	atomic_clear_bit(PG_dirty, &(page)->flags)
#define test_set_pageflag_dirty(page)	atomic_test_and_set_bit(PG_dirty, &(page)->flags)
#define pgflag_test_clear_dirty(page) atomic_test_and_clear_bit(PG_dirty, &(page)->flags)
int test_clear_page_dirty(struct page_frame *page);
static inline void clear_page_dirty(struct page_frame *page)
{
	test_clear_page_dirty(page);
}

#endif /* __DIM_SUM_PAGE_FLAGS_H */
