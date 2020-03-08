#ifndef _DIM_SUM_NAMEI_H
#define _DIM_SUM_NAMEI_H

#include <dim-sum/fnode_cache.h>

struct mount_desc;

struct open_intent {
	int	flags;
	int	create_mode;
};

#define MAX_NESTED_LINKS	5
#define MAX_LINK_COUNT		64

enum {
	/**
	 * 如果最后一个文件是符号链接，则解释它。
	 */
	__FNODE_LOOKUP_READLINK,
	/**
	 * 路径中最后一个文件必须是目录。
	 */
	__FNODE_LOOKUP_DIRECTORY,
	/**
	 * 不解析最后一个文件
	 */
	__FNODE_LOOKUP_NOLAST,
};

#define FNODE_LOOKUP_READLINK		(1UL << __FNODE_LOOKUP_READLINK)
#define FNODE_LOOKUP_DIRECTORY	 (1UL << __FNODE_LOOKUP_DIRECTORY)
#define FNODE_LOOKUP_NOLAST		(1UL << __FNODE_LOOKUP_NOLAST)

/**
 * PATHTYPE_NOTHING:	还没有开始解析
 * PATHTYPE_ROOT:最后一个分量是"/"
 * PATHTYPE_NORMAL:	最后一个分量是普通文件名
 * PATHTYPE_DOT:	最后一个分量是"."
 * PATHTYPE_DOTDOT:	最后一个分量是".."
 */
enum {
	PATHTYPE_NOTHING,
	PATHTYPE_ROOT,
	PATHTYPE_NORMAL,
	PATHTYPE_DOT,
	PATHTYPE_DOTDOT,
};

/**
 * 路径查找的结果
 */
struct filenode_lookup_s {
	/**
	 * 查找标志。
	 */
	unsigned int	flags;
	/**
	 * 查找到的目录对象。
	 */
	struct filenode_cache	*filenode_cache;
	/**
	 * 已经安装的文件系统对象。
	 */
	struct mount_desc *mnt;
	/**
	 * 路径名最后一个分量的类型。如PATHTYPE_NORMAL
	 */
	int		path_type;
	/**
	 * 查找过程中用到的临时变量
	 */
	struct file_name cur;
	/**
	 * 路径名的最后一个分量。
	 * 当指定FNODE_LOOKUP_NOLAST时使用。
	 */
	struct file_name	last;
	/**
	 * 符号链接查找的嵌套深度。
	 */
	unsigned	nested_count;
	/**
	 * 嵌套关联路径名数组。
	 */
	char *symlink_names[MAX_NESTED_LINKS + 1];
};

extern void loosen_fnode_look(struct filenode_lookup_s *);

static inline char *
look_curr_symlink_name(struct filenode_lookup_s *look)
{
	return look->symlink_names[look->nested_count];
}

static inline void
look_save_symlink_name(struct filenode_lookup_s *look,
	char *path)
{
	look->symlink_names[look->nested_count] = path;
}

extern void init_file_node_early(void);

#endif /* _DIM_SUM_NAMEI_H */
