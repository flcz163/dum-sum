#ifndef __DIM_SUM_RADIX_TREE_H
#define __DIM_SUM_RADIX_TREE_H

#include <dim-sum/preempt.h>
#include <dim-sum/types.h>

/**
 * 页高速缓存基树的根
 */
struct radix_tree_root {
	/**
	 * 树的当前深度。不包含叶子节点。
	 */
	unsigned int		height;
	/**
	 * 为新节点请求内存时所用的标志。
	 */
	int			paf_mask;
	/**
	 * 指向与树中第一层节点相应的数据结构。
	 */
	struct radix_tree_node	*rnode;
};

#define RADIX_TREE_INIT(mask)	{					\
	.height = 0,							\
	.paf_mask = (mask),						\
	.rnode = NULL,							\
}

#define RADIX_TREE(name, mask) \
	struct radix_tree_root name = RADIX_TREE_INIT(mask)

#define INIT_RADIX_TREE(root, mask)					\
do {									\
	(root)->height = 0;						\
	(root)->paf_mask = (mask);					\
	(root)->rnode = NULL;						\
} while (0)

int radix_tree_insert(struct radix_tree_root *, unsigned long, void *);
void *radix_tree_lookup(struct radix_tree_root *, unsigned long);
void *radix_tree_delete(struct radix_tree_root *, unsigned long);
unsigned int
radix_tree_gang_lookup(struct radix_tree_root *root, void **results,
			unsigned long first_index, unsigned int max_items);
int radix_tree_preload(int paf_mask);
void init_radix_tree(void);
void *radix_tree_tag_set(struct radix_tree_root *root,
			unsigned long index, int tag);
void *radix_tree_tag_clear(struct radix_tree_root *root,
			unsigned long index, int tag);
int radix_tree_tag_get(struct radix_tree_root *root,
			unsigned long index, int tag);
unsigned int
radix_tree_gang_lookup_tag(struct radix_tree_root *root, void **results,
		unsigned long first_index, unsigned int max_items, int tag);
int radix_tree_tagged(struct radix_tree_root *root, int tag);

static inline void radix_tree_preload_end(void)
{
	preempt_enable();
}

#endif /* __DIM_SUM_RADIX_TREE_H */
