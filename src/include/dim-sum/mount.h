#ifndef _DIM_SUM_MOUNT_H
#define _DIM_SUM_MOUNT_H

#include <dim-sum/object.h>

/**
 * 文件系统安装点
 */
struct mount_desc
{
	struct object object;
	/**
	 * mount标志
	 */
	int flags;
	/**
	 * 设备文件名。
	 */
	char *dev_name;
	/**
	 * 通过此字段将描述符添加到哈希表中
	 */
	struct hash_list_node hash_node;
	/**
	 * 指向父文件系统，这个文件系统安装在其上。
	 */
	struct mount_desc *parent;
	/**
	 * 安装点目录节点。
	 */
	struct filenode_cache *mountpoint;
	/**
	 * 指向这个文件系统根目录的dentry。
	 */
	struct filenode_cache *sticker;
	/**
	 * mount在该文件系统上面的子文件系统链表头
	 */
	struct double_list children;
	/**
	 * 通过此字段将其加入父文件系统的children链表中。
	 */
	struct double_list child;	/* and going through their mnt_child */
	/**
	 * 该文件系统的超级块对象。
	 */
	struct super_block *super_block;
};

static inline struct mount_desc *hold_mount(struct mount_desc *desc)
{
	if (desc)
		hold_object(&desc->object);

	return desc;
}

static inline void loosen_mount(struct mount_desc *desc)
{
	if (desc)
		loosen_object(&desc->object);
}

extern struct mount_desc *lookup_mount(struct mount_desc *, struct filenode_cache *);

/**
 * 保护已经安装文件系统的链表。
 */
extern struct smp_lock mount_lock;
/*
 * Block device ioctls
 */
#define BLKROSET   _IO(0x12, 93)	/* Set device read-only (0 = read-write).  */
#define BLKROGET   _IO(0x12, 94)	/* Get read-only status (0 = read_write).  */
#define BLKRRPART  _IO(0x12, 95)	/* Re-read partition table.  */
#define BLKGETSIZE _IO(0x12, 96)	/* Return device size.  */
#define BLKFLSBUF  _IO(0x12, 97)	/* Flush buffer cache.  */
#define BLKRASET   _IO(0x12, 98)	/* Set read ahead for block device.  */
#define BLKRAGET   _IO(0x12, 99)	/* Get current read ahead setting.  */

/*
 * Prototypes
 */
extern int mount(const char *, const char *,
		   const char *, unsigned long, const void *);
extern int umount(const char *);
extern int umount2(const char *, int);
extern int pivot_root(const char *, const char *);

/**
 * 在已经安装文件系统中禁止setuid和setgid标志。
 */
#define MNT_NOSUID	1
/**
 * 在已经安装文件系统中禁止访问设备文件
 */
#define MNT_NODEV	2
/**
 * 在已经安装文件系统中不允许程序执行。
 */
#define MNT_NOEXEC	4

extern struct mount_desc *do_internal_mount(const char *fstype, int flags,
				      const char *name, void *data);
long do_mount(char * dev_name, char * dir_name, char *type_page,
		  unsigned long flags, void *data_page);

#endif /* _DIM_SUM_MOUNT_H */
