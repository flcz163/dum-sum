#ifndef __DIM_SUM_LEXT3_FS_H
#define __DIM_SUM_LEXT3_FS_H

#include <dim-sum/approximate_counter.h>
#include <dim-sum/fs.h>
#include <dim-sum/journal.h>
#include <dim-sum/rbtree.h>
#include <dim-sum/semaphore.h>
#include <dim-sum/smp_lock.h>
#include <dim-sum/types.h>

#define LEXT3_RESERVE_UID		0
#define LEXT3_RESERVE_GID		0

/**
 * 文件块大小
 */
#define LEXT3_MIN_BLOCK_SIZE		1024
#define LEXT3_MAX_BLOCK_SIZE		4096

/**
 * 默认加载选项
 */
#define LEXT3_DEFM_DEBUG		0x0001
#define LEXT3_DEFM_BSDGROUPS	0x0002
#define LEXT3_DEFM_XATTR_USER	0x0004
#define LEXT3_DEFM_ACL			0x0008
#define LEXT3_DEFM_UID16		0x0010
#define LEXT3_DEFM_JMODE		0x0060
#define LEXT3_DEFM_JMODE_DATA	0x0020
#define LEXT3_DEFM_JMODE_ORDERED	0x0040
#define LEXT3_DEFM_JMODE_WBACK	0x0060

/**
 * 文件节点标志
 */
/**
 * 安全删除。
 * 在删除文件时，将其数据块中写入随机数。
 */
#define LEXT3_SECRM_FL		0x00000001
/**
 * 在删除文件时，并不真的删除，而是放到临时位置。
 */
#define LEXT3_UNRM_FL		0x00000002
/**
 * 文件被压缩存储。
 */
#define LEXT3_COMPR_FL		0x00000004
/**
 * 同步更新磁盘。用于重要的数据文件。
 */
#define LEXT3_SYNC_FL		0x00000008
/**
 * 不能被移动位置。
 * 在反碎片整理时，也不能移动它的位置。
 * 主要用于启动文件。
 */
#define LEXT3_IMMUTABLE_FL		0x00000010
/**
 * 只能添加而不能修改现有文件的内容。
 */
#define LEXT3_APPEND_FL		0x00000020
/**
 * 即使link count == 0，也不真正删除文件。
 */
#define LEXT3_NODUMP_FL		0x00000040
#define LEXT3_NOATIME_FL		0x00000080
#define LEXT3_DIRTY_FL			0x00000100
/**
 * 存在压缩块，似乎未用。
 */
#define LEXT3_COMPRBLK_FL		0x00000200
#define LEXT3_NOCOMPR_FL		0x00000400
#define LEXT3_ECOMPR_FL		0x00000800
/**
 * 使用了目录索引。
 */
#define LEXT3_INDEX_FL			0x00001000
#define LEXT3_IMAGIC_FL			0x00002000
/**
 * 文件数据块应当被记录到日志
 */
#define LEXT3_JOURNAL_DATA_FL	0x00004000
#define LEXT3_NOTAIL_FL			0x00008000 
#define LEXT3_DIRSYNC_FL		0x00010000
/**
 * 所有目录都平均分布到块组中
 * 而不是与父目录放到同一个块组中
 */
#define LEXT3_TOPDIR_FL			0x00020000
#define LEXT3_RESERVED_FL		0x80000000

#define LEXT3_FL_USER_VISIBLE	0x0003DFFF
#define LEXT3_FL_USER_MODIFIABLE		0x000380FF

/**
 * 加载标志
 */
#define LEXT3_MOUNT_CHECK		0x00001
#define LEXT3_MOUNT_OLDALLOC	0x00002
#define LEXT3_MOUNT_GRPID		0x00004
#define LEXT3_MOUNT_DEBUG		0x00008
#define LEXT3_MOUNT_ERRORS_CONT		0x00010
#define LEXT3_MOUNT_ERRORS_RO			0x00020
#define LEXT3_MOUNT_ERRORS_PANIC		0x00040
#define LEXT3_MOUNT_MINIX_DF			0x00080
#define LEXT3_MOUNT_NOLOAD			0x00100
#define LEXT3_MOUNT_ABORT				0x00200
#define LEXT3_MOUNT_DATA_FLAGS		0x00C00
#define LEXT3_MOUNT_JOURNAL_DATA		0x00400
#define LEXT3_MOUNT_ORDERED_DATA		0x00800
#define LEXT3_MOUNT_WRITEBACK_DATA	0x00C00
#define LEXT3_MOUNT_UPDATE_JOURNAL	0x01000
#define LEXT3_MOUNT_NO_UID32			0x02000
#define LEXT3_MOUNT_XATTR_USER		0x04000
#define LEXT3_MOUNT_POSIX_ACL			0x08000
/**
 * 是否允许使用保留窗口
 */
#define LEXT3_MOUNT_RESERVATION		0x10000
#define LEXT3_MOUNT_BARRIER			0x20000

#define LEXT3_SUPER_MAGIC				0xEF53

#define LEXT3_DFL_MAX_MNT_COUNT		20
#define LEXT3_DFL_CHECKINTERVAL		0

/**
 * 保留的文件节点编号
 */
#define LEXT3_BAD_FILENO		 1
/**
 * 根节点编号
 */
#define LEXT3_ROOT_FILENO		 2
/**
 * bootloader节点编号
 */
#define LEXT3_BOOTLOADER_FILENO	5
#define LEXT3_UNDEL_DIR_FILENO	 	6
#define LEXT3_RESIZE_FILENO		7
/**
 * 日志文件节点编号
 */
#define LEXT3_JOURNAL_FILENO	 	8
/**
 * 第一个用户可用文件节点编号
 */
#define LEXT3_OLD_FIRST_FILENO	11

/**
 * 当出现错误时，忽略错误
 */
#define LEXT3_ERRORS_CONTINUE		1
/**
 * 当出现错误时，只能以只读方式运行
 */
#define LEXT3_ERRORS_RO			2
/**
 *  当出现错误时，系统停止
 */
#define LEXT3_ERRORS_PANIC		3
#define LEXT3_ERRORS_DEFAULT		LEXT3_ERRORS_CONTINUE

#define LEXT3_XATTR_MAGIC		0xEA020000

enum {
	/**
	 * 该文件节点有页面数据在日志中
	 */
	__LEXT3_FNODE_JDATA,
	/**
	 * 刚创建的文件节点对象
	 */
	__LEXT3_STATE_NEW,
	/**
	 * 扩展属性保存在文件节点元数据内
	 */
	__LEXT3_STATE_XATTR,
};

/**
 * 文件类型
 */
#define LEXT3_FT_UNKNOWN		0
#define LEXT3_FT_REG_FILE	1
#define LEXT3_FT_DIR		2
#define LEXT3_FT_CHRDEV		3
#define LEXT3_FT_BLKDEV		4
#define LEXT3_FT_FIFO		5
#define LEXT3_FT_SOCK		6
#define LEXT3_FT_SYMLINK		7
#define LLEXT3_FT_MAX		8

#define LEXT3_LINK_MAX		32000

/**
 * 文件节点状态
 */
#define LEXT3_FNODE_JDATA		(1UL << __LEXT3_FNODE_JDATA)
#define LEXT3_STATE_NEW		(1UL << __LEXT3_STATE_NEW)
#define LEXT3_STATE_XATTR		(1UL << __LEXT3_STATE_XATTR)

/**
 * LEXT3在磁盘上的超级块。
 */
struct lext3_superblock_phy {
	/**
	 * 文件节点的总数。
	 */
/*00*/	__le32	fnode_count;
	/**
	 * 以块为单位的文件系统的大小。
	 * 文件系统中的块数量
	 */
	__le32	block_count;
	/**
	 * 保留的块数。
	 */
	__le32	reserve_blocks;
	/**
	 * 空闲块计数器。
	 */
	__le32	free_blocks_count;
	/**
	 * 空闲文件结点计数器。
	 */
/*10*/	__le32	free_fnodes_count;
	/**
	 * 第一个使用的块号。
	 */
	__le32	first_data_block;
	/**
	 * 块的大小= 1024 * 2 << blksize_order_1024
	 */
	__le32	blksize_order_1024;
	/**
	 * 片的大小，未用
	 */
	__le32	frag_size_order_1024;
	/**
	 * 每组中的块数。
	 */
/*20*/	__le32	blocks_per_group;
	/**
	 * 每组中的片数。
	 */
	__le32	frags_per_group;
	/**
	 * 每组中的文件节点数量
	 */
	__le32	fnodes_per_group;
	/**
	 * 最后一次加载操作的时间。
	 */
	__le32	mount_time;	
	/**
	 * 最后一次写操作的时间。
	 */
/*30*/	__le32	modify_time;
	/**
	 * 加载操作计数器。
	 */
	__le16	mount_count;
	/**
	 * 最多允许加载的次数，超过此次数要求进行扫描
	 */
	__le16	max_mount_count;
	/**
	 * 魔术字。
	 */
	__le16	magic;
	/**
	 * 状态标志。
	 * 	0-已经安装或者没有正常卸载。
	 *	1-被正常卸载。
	 *	2-包含错误。
	 */
	__le16	state;
	/**
	 * 当检查到错误时的行为。
	 */
	__le16	errors;
	/**
	 * 次版本号。
	 */
	__le16	minor_rev_level;
	/**
	 * 最后一次检查的时间。
	 */
/*40*/	__le32	last_check;
	/**
	 * 两次次检查之间的时间间隔。
	 */
	__le32	check_interval;
	/**
	 * 创建文件系统的OS
	 */
	__le32	creator_os;
	/**
	 * 版本号。
	 */
	__le32	revision;
	/**
	 * 保留块的缺省UID。
	 */
/*50*/	__le16	def_reserve_uid;
	/**
	 * 保留块的缺省用户组ID。
	 */
	__le16	def_reserve_gid;
	/**
	 * 第一个非保留的文件结点号。
	 */
	__le32	first_fnode;
	/**
	 * 磁盘上文件结点结构的大小。
	 */
	__le16   fnode_size;
	/**
	 * 超级块的块组号。
	 */
	__le16	block_group_num;
	/**
	 * 具有兼容特点的功能选项。
	 */
	__le32	feature_compat;
	/**
	 * 具有非兼容特点的功能选项。
	 */
/*60*/	__le32	feature_incompat;
	/**
	 * 只读兼容特点的功能选项。
	 */
	__le32	feature_ro_compat;
	/**
	 * UUID
	 */
/*68*/	__u8	uuid[16];
	/**
	 * 卷名
	 */
/*78*/	char	volume_name[16];
	/**
	 * 最后一个安装点的路径名。
	 */
/*88*/	char	last_mounted[64];
	/**
	 * 用于压缩。
	 */
/*C8*/	__le32	algorithm_usage_bitmap;
	/**
	 * 预分配的块数。
	 */
	__u8	prealloc_blocks;
	/**
	 * 为目录预分配的块数
	 */
	__u8	prealloc_dir_blocks;
	/**
	 * 按字对齐，补空。
	 */
	__u16	reserved_gdt_blocks;
	/**
	 * 日志超级块的UUID
	 */
/*D0*/	__u8	journal_uuid[16];
	/**
	 * 日志文件节点编号
	 */
/*E0*/	__le32	journal_fnode_num;
	/**
	 * 日志设备文件节点编号
	 */
	__le32	journal_dev_num;
	/**
	 * 孤儿链表头。需要清除
	 */
	__le32	last_orphan;
	__le32	hash_seed[4];
	__u8	def_hash_version;
	__u8	reserved_char_pad;
	__u16	reserved_word_pad;
	__le32	default_mount_opts;
	__le32	first_meta_block_group;
	/**
	 * 用NULL填充1024字节。 
	 */
	__u32	reserved[190];
};

/**
 * 块组锁的数量
 */
#if MAX_CPUS >= 32
#define BLOCKGROUP_LOCK_COUNT	128
#elif MAX_CPUS >= 16
#define BLOCKGROUP_LOCK_COUNT	64
#elif MAX_CPUS >= 8
#define BLOCKGROUP_LOCK_COUNT	32
#elif MAX_CPUS >= 4
#define BLOCKGROUP_LOCK_COUNT	16
#elif MAX_CPUS >= 2
#define BLOCKGROUP_LOCK_COUNT	8
#else
#define BLOCKGROUP_LOCK_COUNT	4
#endif

/**
 * 块组锁，注意对齐
 */
struct blockgroup_lock {
	struct {
		struct smp_lock lock;
	} aligned_cacheline_in_smp locks[BLOCKGROUP_LOCK_COUNT];
};

/**
 * 内存中的LEXT3超级块
 */
struct lext3_superblock {
	/**
	 * 超级块在物理块设备中的位置，默认是1024
	 */
	unsigned long super_pos;
	/**
	 * 包含超级块的磁盘块缓冲区
	 */
	struct blkbuf_desc *blkbuf_super;
	/**
	 * 磁盘上的物理超级块缓存冲区内容。
	 */
	struct lext3_superblock_phy *phy_super;
	/**
	 * 文件系统逻辑块长度
	 */
	unsigned long block_size;
	/**
	 * 每个逻辑块里面的文件节点数量
	 */
	unsigned long fnodes_per_block;
	/**
	 * 每个块组里面有多少个块
	 */
	unsigned long blocks_per_group;
	/**
	 * 每个块组里面有多少个文件节点
	 */
	unsigned long fnodes_per_group;
	/**
	 * 每个块组里面有多少个块用于文件节点
	 */
	unsigned long fnode_blocks_per_group;
	/**
	 * 加载时的状态
	 * 	0-已经安装或者没有正常卸载。
	 *	1-被正常卸载。
	 *	2-包含错误
	 */
	unsigned short mount_state;
	/**
	 * 组描述符的块数量
	 */
	unsigned long blkcount_grpdesc;
	/**
	 * 可以放在一个块中的组描述符个数。
	 */
	unsigned long group_desc_per_block;
	/**
	 * 2 ^ group_desc_per_block_order = group_desc_per_block
	 */
	int group_desc_per_block_order;
	/**
	 * 块组数量
	 */
	unsigned long groups_count;
	/**
	 * 包含所有块组信息的块缓冲区数组
	 */
	struct blkbuf_desc **blkbuf_grpdesc;
	/**
	 * 加载选项
	 */
	unsigned long  mount_opt;
	uid_t reserve_uid;
	gid_t reserve_gid;
	/**
	 * 每个块里面可以包含多少个间接块指针，*order*
	 */
	int addr_per_block_order;
	/**
	 * 在磁盘中，一个文件节点的大小
	 */
	int fnode_size;
	/**
	 * 第一个文件节点编号
	 */
	int first_fnode;
	/**
	 * 保护节点版本号的锁
	 */
	struct smp_lock gen_lock;
	/**
	 * 节点版本号，用于判断目录里面文件是否有变化
	 */
	u32 generation;
	/**
	 * 可用数据块数量
	 */
	struct approximate_counter free_block_count;
	/**
	 * 可用文件节点数量
	 */
	struct approximate_counter free_fnode_count;
	/**
	 * 目录数量
	 */
	struct approximate_counter dir_count;
	/**
	 * 用于保护块组的锁(简单哈希了一下)
	 */
	struct blockgroup_lock group_block;

	/**
	 * 日志对象，目前只支持设备日志，不支持文件节点日志
	 */
	struct journal * journal;
	/**
	 * 日志所在的块设备
	 */
	struct block_device *blkdev_journal;
	/**
	 * 提交周期，定期向日志系统提交日志
	 * 默认是5秒
	 */
	unsigned long commit_interval;
	/**
	 * 孤儿链表头。
	 */
	struct double_list orphans;
};

/**
 * LEXT3文件系统的块组描述符
 * 内存、磁盘中的数据结构是一致的。
 */
struct lext3_group_desc
{
	/**
	 * 块位图的块号
	 */
	__le32	datablock_bitmap;
	/**
	 * 文件节点位图的块号
	 */
	__le32	fnode_bitmap;
	/**
	 * 第一个索引节点表块的块号。
	 */
	__le32	first_fnode_block;
	/**
	 * 组中空闲块的个数。
	 */
	__le16	free_block_count;
	/**
	 * 组中空闲文件节点的个数。
	 */
	__le16	free_fnode_count;
	/**
	 * 组中目录的个数
	 */
	__le16	dir_count;
	/**
	 * 用于对齐
	 */
	__u16	__pad;
	/**
	 * NULL，保留
	 */
	__le32	__reserved[3];
};

/**
 * 与间接块指针相关的常量
 */
/**
 * 在文件节点元数据区内的直接块数量
 */
#define	LEXT3_DIRECT_BLOCKS		12
/**
 * 第一级间接块的索引
 */
#define	LEXT3_INDIRECT1_INDEX			LEXT3_DIRECT_BLOCKS
/**
 * 第二级间接块的索引
 */
#define	LEXT3_INDIRECT2_INDEX			(LEXT3_INDIRECT1_INDEX + 1)
/**
 * 第三级间接块的索引
 */
#define	LEXT3_INDIRECT3_INDEX			(LEXT3_INDIRECT2_INDEX + 1)
/**
 * 数据块索引指针的个数。
 * 包含12个直接块，3个间接块指针
 */
#define	LEXT3_BLOCK_INDEX_COUNT		(LEXT3_INDIRECT3_INDEX + 1)

/**
 * 磁盘上存放的LEXT3文件节点元数据
 */
struct lext3_fnode_disk {
	/**
	 * 文件类型和访问权限。
	 *		0:未知文件
	 *		1:普通文件
	 *		2:目录
	 *		3:字符设备
	 *		4:块设备
	 *		5:命名管道
	 *		6:套接字
	 *		7:符号链接
	 */
	__le16	mode;
	/**
	 * 拥有者标识符。
	 */
	__le16	uid;
	/**
	 * 以字节为单位的文件长度。
	 */
	__le32	file_size;
	/**
	 * 最后一次访问文件的时间。
	 */
	__le32	access_time;
	/**
	 * 索引节点最后改变的时间。
	 */
	__le32	meta_modify_time;
	/**
	 * 文件内容最后修改的时间。
	 */
	__le32	data_modify_time;
	union {
		/**
		 * 删除时间。
		 */
		__le32	del_time;
		/**
		 * 对于孤儿节点来说，链接到下一个孤儿节点
		 */
		__le32	next_orphan;
	};
	/**
	 * 用户组标识符。
	 */
	__le16	gid;
	/**
	 * 硬链接计数。
	 */
	__le16	links_count;
	/**
	 * 文件的数据块数。以512B为单位
	 * 包含已经用数量，以及保留数量。
	 */
	__le32	block_count;
	/**
	 * 文件标志。
	 */
	__le32	flags;
	/**
	 * 特定的操作系统信息。
	 */
	union {
		struct {
			__u32  reserved;
		} linux_os;

		/**
		 * 仅仅HURD使用了此保留值。
		 */
		struct {
			__u32  translator;
		} hurd_os;

		struct {
			__u32  reserved;
		} masix_os;
	} os1;
	/**
	 * 指向数据块的指针。
	 * 前12个块是直接数据块。
	 * 第13块是一级间接块号。
	 * 14->二级间接块。
	 * 15->三级间接块。
	 * 对设备文件来说，指向其设备号
	 * 块设备、字符设备分别用0,1两项表示
	 */
	__le32	blocks[LEXT3_BLOCK_INDEX_COUNT];
	/**
	 * 文件版本，用于判断目录节点是否发生了变化
	 * 在目录seek时有用
	 */
	__le32	generation;
	/**
	 * 文件访问控制列表。
	 */
	__le32	file_acl;
	union {
		/**
		 * 目录访问控制列表。
		 */
		__le32	dir_acl;
		__le32	size_high;
	};
	/**
	 * 最后一个文件片的地址。未用
	 */
	__le32	frag_addr;
	/**
	 * 特定的操作系统信息。
	 */
	union {
		struct {
			__u8	frag_num;
			__u8	frag_size;
			__u16	__pad;
			__le16	uid_high;
			__le16	gid_high;
			__u32	reserved;
		} linux_os;
		struct {
			__u8	frag_num;
			__u8	frag_size;
			__u16	mode_high;
			__u16	uid_high;
			__u16	gid_high;
			__u32	author;
		} hurd_os;
		struct {
			__u8	frag_num;
			__u8	frag_size;
			__u16	__pad;
			__u32	reserved[2];
		} masix_os;
	} os2;				
	/**
	 * 与V1版本相比，文件节点额外占用的空间
	 */
	__le16	extra_fsize;
	__le16	__pad;
};

#define uid_low	uid
#define gid_low	gid
#define uid_high	os2.linux_os.uid_high
#define gid_high	os2.linux_os.gid_high

/**
 * LEXT3文件节点在内存中的映像
 */
struct lext3_file_node {
	/**
	 * 文件节点标志
	 */
	__u32	flags;
	/**
	 * 文件节点状态
	 */
	__u32	state;
	/**
	 * 通过此字段将文件节点加入到文件系统的孤儿链表。
	 */
	struct double_list orphan;
	/**
	 * 文件在磁盘上的大小，回写节点元数据后
	 * 才会与内存中的大小一致
	 */
	loff_t	filesize_disk;
	/**
	 * 针对不同文件类型，保存如下值
	 *	1、文件块编号及间接块编号
	 *	2、设备编号
	 *	3、快捷符号链接
	 */
	__le32	data_blocks[LEXT3_BLOCK_INDEX_COUNT];
	/**
	 * 文件访问控制列表。
	 */
	__u32	file_acl;
	/**
	 * 目录访问控制列表。
	 */
	__u32	dir_acl;
	union {
		__u32	del_time;
		__u32	next_orphan;
	};
	/**
	 * 包含文件节点的块组号
	 * 尽量将同一个文件的数据块放到文件节点相同的块中
	 */
	__u32	block_group_num;
	/**
	 * 上一次分配的逻辑文件块编号
	 */
	__u32	last_logic_block;
	/**
	 * 上一次分配的物理文件块编号
	 */
	__u32	last_phy_block;
 	/**
 	 * 在目录中查找文件用的临时变量
  	 */
	__u32	lookup_start;
	/**
	 * 传统的节点大小是128字节
	 * 但是文件系统也可以设置超过此值
	 * 本字段表示实际的磁盘节点超过128的字节数
	 */
	__u16 extra_fsize;
	/**
	 * 用于防止并发的执行文件截断操作
	 */
	struct semaphore truncate_sem;
	/**
	 * VFS层需要使用的文件节点对象
	 */
	struct file_node vfs_fnode;
};

/**
 * LEXT3文件名的最大长度。
 */
#define LEXT3_NAME_LEN 255

/**
 * LEXT3目录项结构，代表目录中一个文件。
 * 保存在目录节点的数据块中。
 * 是一个变长结构。但是为了效率的原因，它的长度是4的倍数。
 */
struct lext3_dir_item {
	/**
	 * 文件节点编号
	 */
	__le32	file_node_num;
	/**
	 * 目录项长度。也可以解释成一个指针。
	 * 为4的倍数
	 */
	__le16	rec_len;
	/**
	 * 文件名长度
	 */
	__u8	name_len;
	/**
	 * 文件类型。
	 */
	__u8	file_type;
	/**
	 * 文件名
	 */
	char	name[0];
};

/**
 * 文件节点所处的位置
 */
struct lext3_fnode_loc
{
	/**
	 * 所属块组
	 */
	unsigned long block_group;
	/**
	 * 设备块
	 */
	struct blkbuf_desc *blkbuf;
	/**
	 * 在设备块中的偏移
	 */
	unsigned long offset;
};

static inline struct lext3_superblock *
super_to_lext3(struct super_block *super)
{
	return super->fs_info;
}

static inline struct lext3_file_node *fnode_to_lext3(struct file_node *file_node)
{
	return container_of(file_node, struct lext3_file_node, vfs_fnode);
}

#define lext3_clear_opt(opt, mask)	opt &= ~mask
#define lext3_set_opt(opt, mask)		opt |= mask
#define lext3_test_opt(super, opt)		\
	(super_to_lext3(super)->mount_opt & opt)

#define LEXT3_HAS_COMPAT_FEATURE(super,mask)			\
	(super_to_lext3(super)->phy_super->feature_compat & cpu_to_le32(mask))
#define LEXT3_HAS_RO_COMPAT_FEATURE(super,mask)			\
	(super_to_lext3(super)->phy_super->feature_ro_compat & cpu_to_le32(mask))
#define LEXT3_HAS_INCOMPAT_FEATURE(super,mask)			\
	(super_to_lext3(super)->phy_super->feature_incompat & cpu_to_le32(mask))
#define LEXT3_SET_COMPAT_FEATURE(super,mask)			\
	super_to_lext3(super)->phy_super->feature_compat |= cpu_to_le32(mask)
#define LEXT3_SET_RO_COMPAT_FEATURE(super,mask)			\
	super_to_lext3(super)->phy_super->feature_ro_compat |= cpu_to_le32(mask)
#define LEXT3_SET_INCOMPAT_FEATURE(super,mask)			\
	super_to_lext3(super)->phy_super->feature_incompat |= cpu_to_le32(mask)
#define LEXT3_CLEAR_COMPAT_FEATURE(super,mask)			\
	super_to_lext3(super)->phy_super->feature_compat &= ~cpu_to_le32(mask)
#define LEXT3_CLEAR_RO_COMPAT_FEATURE(super,mask)			\
	super_to_lext3(super)->phy_super->feature_ro_compat &= ~cpu_to_le32(mask)
#define LEXT3_CLEAR_INCOMPAT_FEATURE(super,mask)			\
	super_to_lext3(super)->phy_super->feature_incompat &= ~cpu_to_le32(mask)

#define LEXT3_GOOD_OLD_REV	0
#define LEXT3_DYNAMIC_REV		1

#define LEXT3_CURRENT_REV		LEXT3_GOOD_OLD_REV
#define LEXT3_MAX_SUPP_REV		LEXT3_DYNAMIC_REV

/**
 * 版本兼容标志
 */
#define EXT2_FEATURE_COMPAT_EXT_ATTR		0x0008
#define LEXT3_FEATURE_COMPAT_DIR_PREALLOC	0x0001
#define LEXT3_FEATURE_COMPAT_IMAGIC_INODES	0x0002
#define LEXT3_FEATURE_COMPAT_HAS_JOURNAL	0x0004
#define LEXT3_FEATURE_COMPAT_EXT_ATTR		0x0008
#define LEXT3_FEATURE_COMPAT_RESIZE_INODE	0x0010
#define LEXT3_FEATURE_COMPAT_DIR_INDEX		0x0020
#define LEXT3_FEATURE_RO_COMPAT_SPARSE_SUPER	0x0001
#define LEXT3_FEATURE_RO_COMPAT_LARGE_FILE	0x0002
#define LEXT3_FEATURE_RO_COMPAT_BTREE_DIR	0x0004
#define LEXT3_FEATURE_INCOMPAT_COMPRESSION	0x0001
#define LEXT3_FEATURE_INCOMPAT_FILETYPE		0x0002
/**
 * 文件系统支持恢复操作
 * 并且上次系统关机时，没有卸载文件系统，需要恢复
 */
#define LEXT3_FEATURE_INCOMPAT_RECOVER		0x0004
#define LEXT3_FEATURE_INCOMPAT_JOURNAL_DEV	0x0008
#define LEXT3_FEATURE_INCOMPAT_META_BG		0x0010

#define LEXT3_FEATURE_COMPAT_SUPP	EXT2_FEATURE_COMPAT_EXT_ATTR
#define LEXT3_FEATURE_INCOMPAT_SUPP	\
	(LEXT3_FEATURE_INCOMPAT_FILETYPE | \
	LEXT3_FEATURE_INCOMPAT_RECOVER | \
	LEXT3_FEATURE_INCOMPAT_META_BG)
#define LEXT3_FEATURE_RO_COMPAT_SUPP		\
	(LEXT3_FEATURE_RO_COMPAT_SPARSE_SUPER | \
	LEXT3_FEATURE_RO_COMPAT_LARGE_FILE | \
	LEXT3_FEATURE_RO_COMPAT_BTREE_DIR)

#define LEXT3_OLD_FNODE_SIZE 128
#define LEXT3_OLD_FIRST_FILENO	11

#define LEXT3_ADDR_PER_BLOCK_BITS(super)	\
	(super_to_lext3(super)->addr_per_block_order)
#define LEXT3_FNODE_SIZE(super)		\
	(super_to_lext3(super)->fnode_size)
#define LEXT3_FIRST_FILENO(super)	\
	(super_to_lext3(super)->first_fnode)
#define LEXT3_BLOCK_SIZE(super)		\
	((super)->block_size)
#define LEXT3_ADDR_PER_BLOCK(super)	\
	(LEXT3_BLOCK_SIZE(super) / sizeof (__u32))
#define LEXT3_BLOCKS_PER_GROUP(super)	\
	(super_to_lext3(super)->blocks_per_group)
#define LEXT3_DESC_PER_BLOCK(super)	\
	(super_to_lext3(super)->group_desc_per_block)
#define LEXT3_FNODES_PER_GROUP(super)	\
	(super_to_lext3(super)->fnodes_per_group)
#define EXT3_DESC_PER_BLOCK_BITS(super)	\
	(super_to_lext3(super)->group_desc_per_block_order)
/**
 * 获得下一个孤儿文件节点
 */
#define LEXT3_NEXT_ORPHAN(fnode)	\
	fnode_to_lext3(fnode)->next_orphan

/**
 * 文件系统状态，正常卸载
 */
#define	LEXT3_VALID_FS			0x0001
/**
 * 文件系统状态，没有正常卸载
 */
#define	LEXT3_ERROR_FS		0x0002
/**
 * 在初始装载阶段，正在处理孤儿节点
 */
#define	LEXT3_ORPHAN_FS			0x0004

int lext3_loosen_blkdev(struct block_device *blkdev);
extern int lext3_group_has_super(struct super_block *super, int group);
extern unsigned long lext3_count_free_fnodes (struct super_block *);
extern unsigned long lext3_count_free_blocks (struct super_block *);
extern unsigned long lext3_count_dirs(struct super_block *);
extern struct lext3_group_desc *
lext3_get_group_desc(struct super_block *super,
	unsigned int block_group, struct blkbuf_desc ** blkbuf);
extern struct file_node *
lext3_load_orphan_fnode(struct super_block *, unsigned long);
extern void lext3_truncate (struct file_node *);
extern void lext3_discard_reservation(struct file_node *);
static inline struct journal *
lext3_get_fnode_journal(struct file_node *fnode)
{
	return super_to_lext3(fnode->super)->journal;
}

#define LEXT3_QUOTA_TRANS_BLOCKS 0
#define LEXT3_QUOTA_INIT_BLOCKS 0
#define LEXT3_SINGLEDATA_TRANS_BLOCKS	8U
#define LEXT3_XATTR_TRANS_BLOCKS		6U
#define LEXT3_DATA_TRANS_BLOCKS		\
		(LEXT3_SINGLEDATA_TRANS_BLOCKS + \
		LEXT3_XATTR_TRANS_BLOCKS - 2 + \
		2 * LEXT3_QUOTA_TRANS_BLOCKS)
#define LEXT3_DELETE_TRANS_BLOCKS	(2 * LEXT3_DATA_TRANS_BLOCKS + 64)
#define LEXT3_MAX_TRANS_DATA		64U
#define LEXT3_INDEX_EXTRA_TRANS_BLOCKS	8
#define EXT3_RESERVE_TRANS_BLOCKS	12U

static inline int lext3_journal_blocks_per_page(struct file_node *file_node)
{
	return journal_blocks_per_page(file_node);
}

void lext3_init_journal_params(struct super_block *super,
	struct journal *journal);
void lext3_clear_journal_err(struct super_block * super,
	struct lext3_superblock_phy * phy_super);
int lext3_create_journal(struct super_block *super,
	struct lext3_superblock_phy *phy_super, int journal_inum);
int lext3_load_journal(struct super_block * super,
	struct lext3_superblock_phy * phy_super);
struct journal_handle *
lext3_start_journal(struct file_node *file_node, int nblocks);
void lext3_journal_abort_handle(struct blkbuf_desc *blkbuf,
	struct journal_handle *handle, int err);

int lext3_journal_get_undo_access(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf, int *credits);

int lext3_journal_forget(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf);

int lext3_journal_revoke(struct journal_handle *handle,
	unsigned long blocknr, struct blkbuf_desc *blkbuf);

int lext3_journal_get_create_access(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf);

int lext3_journal_dirty_metadata(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf);

static inline int lext3_restart_journal(struct journal_handle *handle,
							int nblocks)
{
	return journal_restart(handle, nblocks);
}

int __lext3_journal_get_write_access(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf, int *credits);

static inline int
ext3_journal_get_write_access_credits(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf, int *credits)
{
	return __lext3_journal_get_write_access(handle, blkbuf, credits);
}

static inline int lext3_journal_get_write_access(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf)
{
	return __lext3_journal_get_write_access(handle, blkbuf, NULL);
}

extern int lext3_forget(struct journal_handle *, int,
			struct file_node *, struct blkbuf_desc *, int);

int  lext3_stop_journal(struct journal_handle *handle);

const char *
lext3_error_msg(struct super_block * super, int errno, char nbuf[16]);
void lext3_std_error (struct super_block * super, int errno);
int lext3_mark_fnode_dirty(struct journal_handle *handle,
	struct file_node *file_node);
extern int lext3_alloc_block (struct journal_handle *,
	struct file_node *, unsigned long, int *);
extern void
lext3_free_blocks (struct journal_handle *, struct file_node *,
	unsigned long, unsigned long);
void lext3_commit_super (struct super_block *super,
	struct lext3_superblock_phy * super_phy, int sync);

#define GROUP_LOCK_INDEX(block_group)	\
	(block_group) & (BLOCKGROUP_LOCK_COUNT-1)
#define lext3_block_group_lock(super, block_group) \
	(&(super)->group_block.locks[GROUP_LOCK_INDEX(block_group)].lock)

#define lext3_set_bit(nr,addr) \
	__test_and_set_bit((nr),(unsigned long*)addr)
#define lext3_set_bit_atomic(nr,addr) \
        atomic_test_and_set_bit((nr),(unsigned long*)addr)
#define lext3_clear_bit(nr, addr) \
	__test_and_clear_bit((nr),(unsigned long*)addr)
#define lext3_clear_bit_atomic(nr, addr) \
	        atomic_test_and_clear_bit((nr),(unsigned long*)addr)
#define lext3_test_bit(nr, addr) test_bit(nr, (unsigned long*)addr)
#define lext3_find_first_zero_bit(addr, size) \
	find_first_zero_bit((unsigned long*)addr, size)
#define lext3_find_next_zero_bit(addr, size, off) \
	find_next_zero_bit((unsigned long*)addr, size, off)

int lext3_fnode_prepare_write(struct journal_handle *handle,
	struct file_node *fnode, struct lext3_fnode_loc *fnode_loc);
int lext3_mark_fnode_loc_dirty(struct journal_handle *handle, 
     struct file_node *fnode, struct lext3_fnode_loc *fnode_loc);
static inline struct lext3_fnode_disk *
lext3_get_fnode_disk(struct lext3_fnode_loc *fnode_loc)
{
	return (struct lext3_fnode_disk *)
		(fnode_loc->blkbuf->block_data + fnode_loc->offset);
}

extern int lext3_add_orphan(struct journal_handle *, struct file_node *);
extern int lext3_orphan_del(struct journal_handle *, struct file_node *);
extern void lext3_read_fnode(struct file_node *);

/* dir.c */
extern struct file_ops lext3_dir_fileops;

/* file.c */
extern struct file_node_ops lext3_file_fnode_ops;
extern struct file_ops lext3_file_ops;

/* namei.c */
extern struct file_node_ops lext3_dir_fnode_ops;
extern struct file_node_ops lext3_special_fnode_ops;

/* symlink.c */
extern struct file_node_ops lext3_symlink_fnode_ops;
extern struct file_node_ops lext3_fast_symlink_fnode_ops;

/* fsync.c */
extern int lext3_sync_file (struct file *, struct filenode_cache *, int);

/* file_node.c */
extern int  lext3_setattr (struct filenode_cache *, struct fnode_attr *);
extern void lext3_init_cachespace(struct file_node *file_node);

#define lext3_permission NULL

extern int lext3_should_retry_alloc(struct super_block *super, int *retries);
extern void lext3_set_fnode_flags(struct file_node *);

/* super.c */
int lext3_commit_journal(struct super_block *super);

static inline int
ext3_acl_chmod(struct file_node *file_node)
{
	return 0;
}

static inline int
ext3_init_acl(struct journal_handle *handle, struct file_node *file_node, struct file_node *dir)
{
	return 0;
}

extern struct blkbuf_desc *
lext3_get_metablock (struct journal_handle *, struct file_node *, long, int, int *);
extern struct blkbuf_desc *
lext3_read_metablock (struct journal_handle *, struct file_node *, int, int, int *);

int lext3_ioctl (struct file_node * file_node, struct file * filp, unsigned int cmd,
		unsigned long arg);
extern struct file_node *
lext3_alloc_fnode (struct journal_handle *, struct file_node *, int);
extern int  lext3_write_fnode (struct file_node *, int);
extern void lext3_dirty_fnode(struct file_node *);
extern void lext3_delete_fnode (struct file_node *);
extern void lext3_free_fnode (struct journal_handle *, struct file_node *);
extern void lext3_update_revision(struct super_block *super);
extern int lext3_verify_dir_item(const char *, struct file_node *,
	struct lext3_dir_item *, struct blkbuf_desc *, unsigned long);
unsigned long lext3_blkcount_groupdesc(struct super_block *super, int group);

#define __printf_3_4 __attribute__((format (printf, 3, 4)))
#define __printf_2_3 __attribute__((format (printf, 2, 3)))
extern __printf_2_3 void lext3_enconter_error(struct super_block *, const char *, ...);
extern __printf_2_3 void lext3_abort_filesystem (struct super_block *, const char *, ...);
extern int fs_overflowuid;
extern int fs_overflowgid;
#define DEFAULT_FS_OVERFLOWUID	65534
#define DEFAULT_FS_OVERFLOWGID	65534
#define fs_high2lowuid(uid) ((uid) & ~0xFFFF ? (uid16_t)fs_overflowuid : (uid16_t)(uid))
#define fs_high2lowgid(gid) ((gid) & ~0xFFFF ? (gid16_t)fs_overflowgid : (gid16_t)(gid))
#define low_16_bits(x)	((x) & 0xFFFF)
#define high_16_bits(x)	(((x) & 0xFFFF0000) >> 16)

#endif /* __DIM_SUM_LEXT3_FS_H */
