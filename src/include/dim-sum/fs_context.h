#ifndef _DIM_SUM_FS_STRUCT_H
#define _DIM_SUM_FS_STRUCT_H

#include <dim-sum/smp_lock.h>
#include <dim-sum/smp_rwlock.h>
#include <dim-sum/smp_seq_lock.h>

struct filenode_cache;
struct mount_desc;

/**
 * 进程的fs字段指向的内容。
 */
struct task_fs_context {
	/**
	 * 共享fs结构的进程个数。
	 */
	struct accurate_counter count;
	/**
	 * 保护该结构的读写锁。
	 */
	struct smp_rwlock lock;
	/**
	 * 当打开文件设置文件权限时使用的位掩码。
	 */
	int umask;
	/**
	 * root_fnode_cache			根目录的目录项。
	 * curr_dir_fnode_cache			当前工作目录的目录项。
	 */
	struct filenode_cache *root_fnode_cache, *curr_dir_fnode_cache;
	/**
	 * root_mount			根目录所安装的文件系统对象。
	 * curr_dir_mount		当前工作目录所安装的文件系统对象。
	 */
	struct mount_desc * root_mount, *curr_dir_mount;
};

extern void set_fs_root(struct task_fs_context *, struct mount_desc *, struct filenode_cache *);
extern void set_fs_pwd(struct task_fs_context *, struct mount_desc *, struct filenode_cache *);

#endif	/* _DIM_SUM_FS_STRUCT_H */
