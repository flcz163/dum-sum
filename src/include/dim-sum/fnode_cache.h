#ifndef __DIM_SUM_DCACHE_H
#define __DIM_SUM_DCACHE_H

#include <dim-sum/accurate_counter.h>
#include <dim-sum/hash_list.h>
#include <dim-sum/smp_lock.h>

struct filenode_lookup_s;
struct super_block;

/**
 * 文件名，用于支持大字符集
 */
struct file_name {
	/**
	 * 文件名及其长度
	 */
	unsigned char *name;
	unsigned int len;
	/**
	 * 哈希值，用于确定将文件放入哪个哈希桶
	 * 以加快搜索过程
	 */
	unsigned int hash;
};

enum {
	/**
	 * 缓存 节点在哈希表中
	 */
	__FNODECACHE_INHASH,
	/**
	 * 动态分配的文件名
	 */
	__FNODECACHE_DYNAME,
};
/**
 * 缓存节点标志
 */
#define FNODECACHE_INHASH (1UL << __FNODECACHE_INHASH)
#define FNODECACHE_DYNAME	(1UL << __FNODECACHE_DYNAME)

#define FNAME_INCACHE_LEN 35
/**
 * 目录节点缓存对象描述符。
 */
struct filenode_cache {
	/**
	 * 保护该结构的自旋锁
	 */
	struct smp_lock lock;
	/**
	 * 目录项对象引用计数器
	 */
	struct accurate_counter ref_count;
	/**
	 * 标志，如FNODECACHE_INHASH
	 */
	unsigned int flags;
	/**
	 * 文件系统操作缓存节点的方法
	 */
	struct fnode_cache_ops *ops;
	/**
	 * 通过此节点，将描述符链接到哈希表中
	 */
	struct hash_list_node hash_node;
	/**
	 * 与文件名关联的索引结点
	 */
	struct file_node *file_node;
	/**
	 * 文件名
	 */
	struct file_name file_name;
	/**
	 * 父目录的目录项对象
	 */
	struct filenode_cache *parent;
	/**
	 * 子文件节点链表头
	 */
	struct double_list children;
	/**
	 * 通过此字段，将节点链接到父目录的children链表中
	 */
	struct double_list child;
	/**
	 * 对目录而言，用于记录mount在该目录中的文件系统数量。
	 */
	int mount_count;
	/**
	 * 存放短文件名
	 */
	unsigned char embed_name[FNAME_INCACHE_LEN + 1];	/* small names */
};

/**
 * 目录项对象的操作方法
 */
struct fnode_cache_ops {
	/**
	 * 为缓存的文件节点生成文件名哈希值
	 * 覆盖内核本身的哈希生成方法
	 */
	int (*d_hash) (struct filenode_cache *, struct file_name *);
	/**
	 * 比较两个文件名是否相同
	 * 缺省的是大小写敏感的字符串比较
	 */
	int (*d_compare) (struct filenode_cache *, struct file_name *, struct file_name *);
	/**
	 * 当需要将对象从缓存中摘除时调用此方法
	 * 默认什么都不做
	 */
	int (*may_delete)(struct filenode_cache *);
	/**
	 * 当要释放一个目录项对象时（放入Beehive分配器），调用该方法，缺省vfs什么都不做
	 */
	void (*d_release)(struct filenode_cache *);
	/**
	 * 当目录项变成负状态时，调用本方法。缺省vfs调用iput释放索引结点对象
	 */
	void (*loosen_file_node)(struct filenode_cache *, struct file_node *);
};

extern struct smp_lock filenode_cache_lock;
extern void fnode_cache_stick(struct filenode_cache *, struct file_node *);
static inline struct filenode_cache *hold_dirnode_cache(struct filenode_cache *filenode_cache)
{
	if (filenode_cache) {
		BUG_ON(!accurate_read(&filenode_cache->ref_count));
		accurate_inc(&filenode_cache->ref_count);
	}
	return filenode_cache;
}

extern void putin_fnode_cache(struct filenode_cache *);
void takeout_fnode_cache(struct filenode_cache *filenode_cache);

extern struct filenode_cache * hold_filenode_cache(struct filenode_cache *);
extern struct filenode_cache * fnode_cache_alloc(struct filenode_cache *, const struct file_name *);
extern void loosen_filenode_cache(struct filenode_cache *);

/* only used at mount-time */
extern struct filenode_cache * fnode_cache_alloc_root(struct file_node *);

static inline void hash_append(unsigned int ch, unsigned int *hash)
{
	*hash = (*hash + (ch << 4) + (ch >> 4)) * 11;
}

/*
 * Finally: cut down the number of bits to a int value (and try to avoid
 * losing bits)
 */
static inline unsigned long end_name_hash(unsigned long hash)
{
	return (unsigned int) hash;
}

extern struct mount_desc *lookup_mount(struct mount_desc *, struct filenode_cache *);
#define IS_ROOT(x) ((x) == (x)->parent)

extern void shrink_fnode_cache(struct super_block *);
extern void shrink_dcache_parent(struct filenode_cache *);
extern void rename_fnode_cache(struct filenode_cache *, struct filenode_cache *);
void remove_fnode_cache(struct filenode_cache * filenode_cache);
extern void genocide_fnode_cache(struct filenode_cache *);

void init_filenode_cache_early(void);
void init_filenode_cache(void);
#endif /* __DIM_SUM_DCACHE_H */
