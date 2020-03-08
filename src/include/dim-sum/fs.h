#ifndef _DIM_SUM_FS_H
#define _DIM_SUM_FS_H

#include <dim-sum/cache.h>
#include <dim-sum/fnode_cache.h>
#include <dim-sum/semaphore.h>
#include <dim-sum/init.h>
#include <dim-sum/statfs.h>
#include <dim-sum/radix-tree.h>
#include <dim-sum/rwsem.h>
#include <dim-sum/stat.h>
#include <dim-sum/time.h>
#include <dim-sum/types.h>
#include <dim-sum/wait.h>

#include <uapi/dim-sum/fs.h>

#include <asm/fcntl.h>

struct file_node;
struct filenode_cache;
struct vm_area_struct;
struct mount_desc;
struct filenode_lookup_s;
struct async_io_desc;
struct poll_table_struct;
struct file_lock;
struct io_segment;
struct fnode_attr;
struct disk_device;
struct file;
struct page_frame;
struct blkbuf_desc;
struct task_file_handles;
struct beehive_allotter;

/**
 * 块设备的名称长度
 */
#define BDEVNAME_SIZE	32

#define VM_MAX_CACHE_HIT    	256	/* max pages in a row in cache before */

enum {
	/**
	 * 这种类型的文件系统必须位于物理磁盘上。
	 */
	__FS_REQUIRES_DEV,
};

#define FS_REQUIRES_DEV (1UL << __FS_REQUIRES_DEV)

#define RW_MASK		1
#define RWA_MASK	2
#define READ 0
#define WRITE 1
#define READA 2		/* read-ahead  - don't block if no resources */
#define SPECIAL 4	/* For non-blockdevice requests in request queue */
#define READ_SYNC	(READ | (1 << BIO_RW_SYNC))
#define WRITE_SYNC	(WRITE | (1 << BIO_RW_SYNC))
#define WRITE_BARRIER	((1 << BIO_RW) | (1 << BIO_RW_BARRIER))

/**
 * 与每个用户态读缓冲区相关的文件读操作的状态。
 */
typedef struct {
	/**
	 * 已经拷贝到用户态缓冲区的字节数
	 */
	size_t written;
	/**
	 * 待传送的字节数
	 */
	size_t remain_count;
	/**
	 * 在用户态缓冲区中的当前位置
	 */
	union {
		char __user * buf;
		void *data;
	} arg;
	/**
	 * 读操作的错误码。0表示无错误。
	 */
	int error;
} read_descriptor_t;
typedef int (*filldir_t)(void *, const char *, int, loff_t, fnode_num_t, unsigned);
typedef int (*read_actor_t)(read_descriptor_t *, struct page_frame *, unsigned long, unsigned long);

extern void __init init_vfs_early(void);
extern void __init init_vfs(void);
extern void __init mnt_init(void);
extern int __init init_rootfs(void);
extern void __init init_file_node(void);

struct super_block;
/**
 * 超级块操作方法
 */
struct super_block_ops {
	/**
	 * 为索引节点对象分配空间，包括具体文件系统的数据所需要的空间。
	 */
   	struct file_node *(*alloc)(struct super_block *sb);
	/**
	 * 释放索引节点对象。
	 */
	void (*free)(struct file_node *);
	/**
	 * 当最后一个用户释放索引节点时调用。通常调用generic_drop_inode。
	 */
	void (*release) (struct file_node *);
	/**
	 * 减少索引节点的引用计数值。
	 */
	void (*loosen) (struct file_node *);

	/**
	 * 用磁盘上的数据填充索引节点对象的字段。
	 * 索引节点对象的i_ino字段标识从磁盘上要索引节点。
	 */
	void (*read_fnode) (struct file_node *);

  	/**
  	 * 当索引节点标记为脏时调用。日志文件系统用来更新磁盘上的文件系统日志。
  	 */
   	void (*dirty_fnode) (struct file_node *);
	/**
	 * 更新索引节点对象的内容。flag参数表示IO操作是否应当同步。
	 */
	int (*write_fnode) (struct file_node *, int);

	/**
	 * 删除内存中的索引节点和磁盘上的文件数据和元数据。
	 */
	void (*delete_fnode) (struct file_node *);
	/**
	 * 由于文件系统被卸载而释放对超级块的引用。
	 */
	void (*loosen_super) (struct super_block *);
	/**
	 * 更新文件系统超级块。
	 */
	void (*write_super) (struct super_block *);
	/**
	 * 清除文件系统以更新磁盘上文件系统数据结构
	 */
	int (*sync_fs)(struct super_block *sb, int wait);
	/**
	 * 阻塞对文件系统的修改，并用指定对象的内容更新超级块。
	 * 当文件系统被冻结时调用。例如LVM会调用它。
	 */
	void (*lock_fs) (struct super_block *);
	/**
	 * 取消由write_super_lockfs阻塞。
	 */
	void (*unlock_fs) (struct super_block *);

	/**
	 * 返回文件系统的统计信息。
	 */
	int (*statfs) (struct super_block *, struct kstatfs *);

	/**
	 * 用新的选项重新安装文件系统。
	 */
	int (*remount) (struct super_block *, int *, char *);
	/**
	 * 撤销磁盘索引节点时调用。
	 */
	void (*clean_fnode) (struct file_node *);
	/**
	 * 开始卸载操作。只在NFS中使用。
	 */
	void (*umount_begin) (struct super_block *);

#if 0
	/**
	 * 显示特定文件系统的选项。
	 */
	int (*show_options)(struct seq_file *, struct mount_desc *);
#endif
	/**
	 * 读取限额设置。
	 */
	ssize_t (*quota_read)(struct super_block *, int, char *, size_t, loff_t);
	/**
	 * 修改限额配置。
	 */
	ssize_t (*quota_write)(struct super_block *, int, const char *, size_t, loff_t);
};

#define S_BIAS (1<<30)
#define	MAX_NON_LFS	((1UL<<31) - 1)
/**
 * 超级块对象
 */
struct super_block {
	/**
	 * 保护超级块使用的信号量
	 */
	struct semaphore	sem;
	/**
	 * 引用计数，存在性保证
	 */
	int			ref_count;
	/**
	 * 激活次数，决定是否需要卸载文件系统
	 */
	struct accurate_counter		active_count;
	/**
	 * mount/unmount时使用的信号量
	 */
	struct rw_semaphore	mount_sem;
	/**
	 * 设备标识符
	 */
	devno_t			dev_num;
	/**
	 * 文件的最大长度
	 */
	unsigned long long	max_file_size;
	/**
	 * 以字节为单位的块大小，512 * 2^n，并且不大于PAGE_SIZE
	 */
	unsigned long		block_size;
	/**
	 * 用order表示的块大小
	 */
	unsigned char		block_size_order;
	/**
	 * c/m/atime的时间戳粒度。也就是精确到多少秒
	 */
	u32		   gran_ns;
	/**
	 * 块设备原始的块大小
	 */
	unsigned long		block_size_device;
	/**
	 * 指向块设备驱动程序描述符的指针
	 */
	struct block_device	*blkdev;
	/**
	 * 脏标志
	 */
	unsigned char		dirty;
	/**
	 * 通过此字段将超级块链接到全局链表中
	 */
	struct double_list	list;
	/**
	 * 相同文件类型的超级块对象链表。
	 */
	struct double_list	cognate;
	/**
	 * 所有索引节点链表
	 */
	struct double_list	file_nodes;	/* all inodes */
	/**
	 * 脏索引节点链表
	 */
	struct double_list	dirty_nodes;
	/**
	 * 等待sync到磁盘的文件节点链表头
	 */
	struct double_list	sync_nodes;
	/**
	 * 文件系统类型。
	 */
	struct file_system_type	*fs_type;
	/**
	 * 超级块方法
	 */
	struct super_block_ops	*ops;
	/**
	 * 安装标志
	 */
	unsigned long		mount_flags;
	/**
	 * 文件系统的魔数
	 */
	unsigned long		magic;
	/**
	 * 文件系统根目录的文件缓存
	 */
	struct filenode_cache		*root_fnode_cache;
	/**
	 * 该文件系统中，所有文件对象链表头
	 */
	struct double_list	files;
	/**
	 * 包含超级块的块设备名称
	 */
	char blkdev_name[BDEVNAME_SIZE];

	/**
	 * 对超级块的索引节点进行同步的标志
	 */
	int			s_syncing;
	/**
	 * 对超级块的已安装文件系统进行同步的的标志
	 */
	int			need_sync_fs;

	/**
	 * 指向特定文件系统的超级块信息的指针。各文件系统自定义。
	 * 对ext2来说，是指向一个ext2_sb_info类型的结构。
	 */
	void 			*fs_info;	/* Filesystem private info */
};

/**
 * 对内核支持的每一种文件系统，存在一个这样的结构对其进行描述。
 */
struct file_system_type {
	/**
	 * 文件系统类型的名称 
	 */
	const char *name;
	/**
	 * 通过此字段链接到全局链表中
	 */
	struct double_list list;
	/**
	 * 此文件系统类型的属性 
	 */
	int flags;
	/**
	 * 函数指针，当安装此类型的文件系统时，就由VFS调用此例程从设备上将此文件系统的superblock读入内存中
	 */	
	struct super_block *(*load_filesystem) (struct file_system_type *, int,
				       const char *, void *);
	/**
	 * 删除超级块的方法。
	 */
	void (*unload_filesystem) (struct super_block *);
	/**
	 * 具有相同文件系统类型的超级块对象链表的头。
	 */
	struct double_list superblocks;
};

extern int invalidate_partition_sync(struct disk_device *, int);
extern int invalidate_fnode_device(struct block_device *, int);

extern int register_filesystem(struct file_system_type *);
extern int unregister_filesystem(struct file_system_type *);
struct file_system_type *lookup_file_system(const char *name);
void loosen_file_system(struct file_system_type *);
/*
 * Use sequence counter to get consistent file_size on 32-bit processors.
 */
#if BITS_PER_LONG==32 && defined(CONFIG_SMP)
#include <dim-sum/smp_seq_lock.h>
#define __NEED_I_SIZE_ORDERED
#define i_size_ordered_init(file_node) seqcount_init(&file_node->i_size_seqcount)
#else
#include <dim-sum/smp_seq_lock.h>
#define i_size_ordered_init(file_node) do { } while (0)
#endif

struct blkdev_infrast;
struct cache_space_ops;
struct writeback_control;

/*
 * NOTE:
 * read, write, poll, fsync, readv, writev, unlocked_ioctl and compat_ioctl
 * can be called without the big kernel lock held in all filesystems.
 */
/**
 * 文件操作选项
 */
struct file_ops {
	/**
	 * 方法llseek用来修改文件的当前读写位置。并将新位置作为返回值返回。
	 * 参数loff_t是一个长偏移量，即使在32位平台上也至少占用64位的数据宽度。
	 * 出错时返回一个负的返回值。如果这个函数指针是NULL，对seek的调用将会以某种不可预期的方式修改file结构中的位置计数。
	 */
	loff_t (*llseek) (struct file *, loff_t, int);
	/**
	 * 用来从设备中读取数据。该函数指针被赋为NULL时，将导致read系统调用出错并返回-EINVAL。函数返回非负值表示成功读取的字节数。
	 */
	ssize_t (*read) (struct file *, char __user *, size_t, loff_t *);
	/**
	 * 初始化一个异步的读取操作。即在函数返回之前可能不会完成的读取操作。如果该方法为NULL，所有的操作都通过read同步完成。
	 */
	ssize_t (*aio_read) (struct async_io_desc *, char __user *, size_t, loff_t);
	/**
	 * 向设备发送数据。如果没有这个函数，write系统调用会向程序返回一个-EINVAL。如果返回值非负，则表示成功写入的字节数。
	 */
	ssize_t (*write) (struct file *, const char __user *, size_t, loff_t *);
	/**
	 * 初始化设备上的异步写入操作。
	 */
	ssize_t (*aio_write) (struct async_io_desc *, const char __user *, size_t, loff_t);
	/**
	 * 对于设备文件来说，这个字段应该为NULL。它仅用于读取目录，只对文件系统有用。
	 * filldir_t用于提取目录项的各个字段。
	 */
	int (*readdir) (struct file *, void *, filldir_t);
	/**
	 * POLL方法是poll、epoll和select这三个系统调用的后端实现。这三个系统调用可用来查询某个或多个文件描述符上的读取或写入是否会被阻塞。
	 * poll方法应该返回一个位掩码，用来指出非阻塞的读取或写入是否可能。并且也会向内核提供将调用进程置于休眠状态直到IO变为可能时的信息。
	 * 如果驱动程序将POLL方法定义为NULL，则设备会被认为既可读也可写，并且不会阻塞。
	 */
	unsigned int (*poll) (struct file *, struct poll_table_struct *);
	/**
	 * 系统调用ioctl提供了一种执行设备特殊命令的方法(如格式化软盘的某个磁道，这既不是读也不是写操作)。
	 * 另外，内核还能识别一部分ioctl命令，而不必调用fops表中的ioctl。如果设备不提供ioctl入口点，则对于任何内核未预先定义的请求，ioctl系统调用将返回错误(-ENOTYY)
	 */
	int (*ioctl) (struct file_node *, struct file *, unsigned int, unsigned long);
	/**
	 * 与ioctl类似，但是不获取大内核锁。
	 */
	long (*unlocked_ioctl) (struct file *, unsigned int, unsigned long);
	/**
	 * 64位内核使用该方法实现32位系统调用。
	 */
	long (*compat_ioctl) (struct file *, unsigned int, unsigned long);
	/**
	 * 请求将设备内存映射到进程地址空间。如果设备没有实现这个方法，那么mmap系统调用将返回-ENODEV。
	 */
	int (*mmap) (struct file *, struct vm_area_struct *);
	/**
	 * 尽管这始终是对设备文件执行的第一个操作，然而却并不要求驱动程序一定要声明一个相应的方法。
	 * 如果这个入口为NULL，设备的打开操作永远成功，但系统不会通知驱动程序。
	 */
	int (*open) (struct file_node *, struct file *);
	/**
	 * 对flush操作的调用发生在进程关闭设备文件描述符副本的时候，它应该执行(并等待)设备上尚未完结的操作。
	 * 请不要将它同用户程序使用的fsync操作相混淆。目前，flush仅仅用于少数几个驱动程序。比如，SCSI磁带驱动程序用它来确保设备被关闭之前所有的数据都被写入磁带中。
	 * 如果flush被置为NULL，内核将简单地忽略用户应用程序的请求。
	 */
	int (*flush) (struct file *);
	/**
	 * 当file结构被释放时，将调用这个操作。与open相似，也可以将release设置为NULL。
	 */
	int (*release) (struct file_node *, struct file *);
	/**
	 * 该方法是fsync系统调用的后端实现
	 * 用户调用它来刷新待处理的数据。
	 * 如果驱动程序没有实现这一方法，fsync系统调用将返回-EINVAL。
	 */
	int (*fsync) (struct file *, struct filenode_cache *, int datasync);
	/**
	 * 这是fsync的异步版本。
	 */
	int (*aio_fsync) (struct async_io_desc *, int datasync);
	/**
	 * 这个操作用来通知设备其FASYNC标志发生了变化。异步通知是比较高级的话题，如果设备不支持异步通知，该字段可以是NULL。
	 */
	int (*fasync) (int, struct file *, int);
	/**
	 * LOCK方法用于实现文件锁定，锁定是常规文件不可缺少的特性。但是设备驱动程序几乎从来不会实现这个方法。
	 */
	int (*lock) (struct file *, int, struct file_lock *);
	/**
	 * 和writev用来实现分散、聚集型的读写操作。应用程序有时需要进行涉及多个内存区域的单次读或写操作。
	 * 利用这些系统调用可完成这类工作，而不必强加额外的数据拷贝操作。如果被设置为NULL，就会调用read和write方法(可能是多次)
	 */
	ssize_t (*readv) (struct file *, const struct io_segment *, unsigned long, loff_t *);
	ssize_t (*writev) (struct file *, const struct io_segment *, unsigned long, loff_t *);
	/**
	 * 这个方法实现sendfile系统调用的读取部分。sendfile系统调用以最小的复制操作将数据从一个文件描述符移动到另一个。
	 * 例如，WEB服务器可以利用这个方法将鞭个文件的内容发送到网络联接。设备驱动程序通常将sendfile设置为NULL。
	 */
	ssize_t (*sendfile) (struct file *, loff_t *, size_t, read_actor_t, void *);
	/**
	 * sendpage是sendfile系统调用的另一半。它由内核调用以将数据发送到对应的文件。每次一个数据页。
	 * 设备驱动程序通常也不需要实现sendfile。
	 */
	ssize_t (*sendpage) (struct file *, struct page_frame *, int, size_t, loff_t *, int);
	/**
	 * 在进程的地址空间中找到一个合适的位置，以便将底层设备中的内存段映射到该位置。
	 * 该任务通常由内存管理代码完成，但该方法的存在可允许驱动程序强制满足特定设备需要的任何对齐要求。大部分驱动程序可设置该方法为NULL。
	 */
	unsigned long (*get_unmapped_area)(struct file *, unsigned long, unsigned long, unsigned long, unsigned long);
	/**
	 * 该方法允许模块检查传递给fcntl调用的标志。当前只适用于NFS
	 */
	int (*check_flags)(int);
	/**
	 * 当应用程序使用fcntl来请求目录改变通知时，该方法将被调用。该方法仅对文件系统有用，驱动程序不必实现dir_notify。
	 * 当前适用于CIFS。
	 */
	int (*dir_notify)(struct file *filp, unsigned long arg);
	/**
	 * 用于定制flock系统调用的行为。当进程试图对文件加锁时，回调此函数。
	 */
	int (*flock) (struct file *, int, struct file_lock *);
};

/**
 * 索引节点操作
 */
struct file_node_ops {
	/**
	 * 向用户返回符号链接的内容
	 */
	int (*read_link) (struct filenode_cache *, char __user *, int);
	/**
	 * 将符号链接的内容读取到缓存中
	 */
	int (*follow_link) (struct filenode_cache *, struct filenode_lookup_s *);
	/**
	 * 在某一目录下，为与目录项对象相关的普通文件创建一个新的磁盘索引节点。
	 */
	int (*create) (struct file_node *,struct filenode_cache *,int, struct filenode_lookup_s *);
	/**
	 * 为包含在一个目录项对象中的文件名对应的索引节点查找目录?
	 */
	struct filenode_cache * (*lookup) (struct file_node *,struct filenode_cache *, struct filenode_lookup_s *);
	/**
	 * 创建硬连接。
	 */
	int (*link) (struct filenode_cache *,struct file_node *,struct filenode_cache *);
	/**
	 * 删除文件的硬连接。
	 */
	int (*unlink) (struct file_node *,struct filenode_cache *);
	/**
	 * 创建软链接。
	 */
	int (*symlink) (struct file_node *,struct filenode_cache *,const char *);
	/**
	 * 创建目录。
	 */
	int (*mkdir) (struct file_node *,struct filenode_cache *,int);
	/**
	 * 移除目录。
	 */
	int (*rmdir) (struct file_node *,struct filenode_cache *);
	/**
	 * 为特定设备文件创建一个索引节点。
	 */
	int (*mknod) (struct file_node *,struct filenode_cache *,int,devno_t);
	/**
	 * 重命名文件。
	 */
	int (*rename) (struct file_node *, struct filenode_cache *,
			struct file_node *, struct filenode_cache *);
	/**
	 * 释放follow_link分配的临时数据结构。
	 */
	void (*loosen_link) (struct filenode_cache *, struct filenode_lookup_s *);
	/**
	 * 根据i_size字段，修改索引节点相关的文件长度。
	 */
	void (*truncate) (struct file_node *);
	/**
	 * 检查文件权限。
	 */
	int (*permission) (struct file_node *, int, struct filenode_lookup_s *);
	/**
	 * 修改文件属性。
	 */
	int (*setattr) (struct filenode_cache *, struct fnode_attr *);
	/**
	 * 读取文件属性。
	 */
	int (*get_attribute) (struct mount_desc *mnt, struct filenode_cache *, struct file_attribute *);
	/**
	 * 设置扩展属性。这些属性放在索引节点外的磁盘块中。
	 */
	int (*setxattr) (struct filenode_cache *, const char *,const void *,size_t,int);
	/**
	 * 获取扩展属性。
	 */
	ssize_t (*getxattr) (struct filenode_cache *, const char *, void *, size_t);
	/**
	 * 获取扩展属性名称的整个链表。
	 */
	ssize_t (*listxattr) (struct filenode_cache *, char *, size_t);
	/**
	 * 删除索引节点的扩展属性。
	 */
	int (*removexattr) (struct filenode_cache *, const char *);
};

/**
 * 页高速缓存的核心数据结构
 * 在所有者的页和对这些页的操作之间建立起关联关系。
 */
struct file_cache_space {
	/**
	 * 分配内存的标志
	 */
	unsigned long alloc_flags;
	/**
	 * 如果存在，就指向拥有该对象的索引结点的指针。
	 */
	struct file_node		*fnode;		/* owner: file_node, block_device */
	/**
	 * 拥有者的页的基树
	 */
	struct radix_tree_root	page_tree;	/* radix tree of all pages */
	/**
	 * 保护基数的自旋锁
	 */
	struct smp_lock		tree_lock;	/* and spinlock protecting it */
	/**
	 * 地址空间中共享内存映射的个数。
	 */
	unsigned int		i_mmap_writable;/* count VM_SHARED mappings */
#if 0
	/**
	 * radix优先搜索树的根，用于映射页(如共享程序文件、共享C库)的反向映射。
	 */
	struct prio_tree_root	i_mmap;		/* tree of private and shared mappings */
#endif
	/**
	 * 地址空间中非线性内存区的链表
	 */
	struct double_list	i_mmap_nonlinear;/*list VM_NONLINEAR mappings */
	/**
	 * 保护radix优先搜索树的自旋锁。
	 */
	struct smp_lock		i_mmap_lock;	/* protect tree, count, list */
	/**
	 * 截断文件时使用的顺序计数器。
	 */
	unsigned int		truncate_count;	/* Cover race condition with truncate */
	/**
	 * 所有者的页总数。
	 */
	unsigned long		page_count;	/* number of total pages */
	/**
	 * 最后一次回写操作所作用的页的索引
	 */
	pgoff_t			writeback_index;/* writeback starts here */
	/**
	 * 对所有者页进行操作的方法
	 */
	struct cache_space_ops *ops;	/* methods */
	/**
	 * 错误位和内存分配器的标志
	 */
	unsigned long		flags;		/* error bits/paf mask */
	/**
	 * 指向拥有所有者数据的块设备的backing_dev_info指针
	 */
	struct blkdev_infrast *blkdev_infrast; /* device readahead, etc */
	/**
	 * 通常是管理private_list链表时使用的自旋锁
	 * 以下几个字段可以由各文件系统自行使用。
	 */
	struct smp_lock		block_lock;	/* for use by the file_cache_space */
} __attribute__((aligned(sizeof(long))));

struct file;
struct page_frame;
struct io_segment;
struct async_io_desc;

/**
 * 对页进行处理的各种方法
 */
struct cache_space_ops {
	/**
	 * 写操作(从页写到所有者的磁盘映象)
	 */
	int (*writepage)(struct page_frame *page, struct writeback_control *wbc);
	/**
	 * 读操作(从所有者的磁盘映象读到页)
	 */
	int (*readpage)(struct file *, struct page_frame *);
	/**
	 * 当有等待者在等待页面时，强制同步页面
	 * 以减少等待时间
	 */
	int (*sync_page)(struct page_frame *);

	/* Write back some dirty pages from this space. */
	/**
	 * 把指定数量的所有者脏页写回磁盘
	 */
	int (*writepages)(struct file_cache_space *, struct writeback_control *);

	/* Set a page dirty */
	/**
	 * 把所有者的页设置为脏页
	 */
	int (*set_page_dirty)(struct page_frame *page);

	/**
	 * 从磁盘中读所有者页的链表
	 */
	int (*readpages)(struct file *filp, struct file_cache_space *space,
			struct double_list *pages, unsigned nr_pages);

	/*
	 * ext3 requires that a successful prepare_write() call be followed
	 * by a commit_write() call - they must be balanced
	 */
	/**
	 * 为写操作做准备（由磁盘文件系统使用）
	 */
	int (*prepare_write)(struct file *, struct page_frame *, unsigned, unsigned);
	/**
	 * 完成写操作（由磁盘文件系统使用）
	 */
	int (*commit_write)(struct file *, struct page_frame *, unsigned, unsigned);
	/* Unfortunately this kludge is needed for FIBMAP. Don't use it */
	/**
	 * 从文件块索引中获取逻辑块号
	 */
	sector_t (*map_block)(struct file_cache_space *, sector_t);
	/**
	 * 使所有者的页无效（截断文件时用）
	 */
	int (*invalidatepage) (struct page_frame *, unsigned long);
	/**
	 * 由日志文件系统使用，以准备释放页
	 */
	int (*releasepage) (struct page_frame *, int);
	/**
	 * 所有者页的直接I/O传输(绕过页高速缓存)
	 */
	ssize_t (*direct_IO)(int, struct async_io_desc *, const struct io_segment *iov,
			loff_t offset, unsigned long nr_segs);
};

enum {
	/**
	 * 文件节点正在被释放。
	 */
	__FNODE_FREEING,
	/**
	 * 文件元数据脏
	 */
	__FNODE_DIRTY_SYNC,
	/**
	 * 文件节点中，与数据块相关的元数据(间接块)脏
	 */
	__FNODE_DIRTY_DATASYNC,
	/**
	 * 文件数据页面脏
	 */
	__FNODE_DIRTY_PAGES,
	/**
	 * 节点对象处理IO传送中。
	 */
	__FNODE_TRANSFERRING,
	/**
	 * 已经操作完对象，节点内容不再有意义
	 * 可以释放对节点的引用
	 */
	__FNODE_CLEAN,
	/**
	 * 索引节点对象已经分配，但是还没有从磁盘中读取数据。
	 */
	__FNODE_NEW,
};

#define FNODE_FREEING		(1UL << __FNODE_FREEING)
#define FNODE_DIRTY_SYNC	(1UL << __FNODE_DIRTY_SYNC)
#define FNODE_DIRTY_DATASYNC	(1UL << __FNODE_DIRTY_DATASYNC)
#define FNODE_DIRTY_PAGES		(1UL << __FNODE_DIRTY_PAGES)
#define FNODE_TRANSFERRING	(1UL << __FNODE_TRANSFERRING)
#define FNODE_CLEAN			(1UL << __FNODE_CLEAN)
#define FNODE_NEW			(1UL << __FNODE_NEW)
/**
 * 文件节点是否为脏。
 */
#define FNODE_DIRTY (FNODE_DIRTY_SYNC | FNODE_DIRTY_DATASYNC | FNODE_DIRTY_PAGES)

/**
 * 内核用该结构在内部表示一个文件。它与file不同。file表示的的文件描述符。
 * 对单个文件，可能会有许多个表示打开的文件描述符的filep结构，但是它们都指向单个inode结构。
 */
struct file_node {
	/**
	 * 保护文件节点的信号量。
	 */
	struct semaphore	sem;
	/**
	 * 文件节点编号。
	 */
	unsigned long		node_num;
	/**
	 * 状态标志
	 */
	unsigned long		state;
	/**
	 * 通过此字段将对象放入哈希表。
	 */
	struct hash_list_node	hash_node;
	/**
	 * 如果文件节点表示一个设备文件
	 * 此字段代码设备号
	 */
	devno_t			devno;
	/**
	 * 通过此字段将其链入到超级块的inode链表中。
	 */
	struct double_list	super_block;
	/**
	 * 通过此字段将其链入到字符设备或者块设备链表
	 */
	struct double_list	device;
	/**
	 * 索引节点的操作。
	 */
	struct file_node_ops	*node_ops;
	/**
	 * 缺省文件操作。
	 */
	struct file_ops	*file_ops;
	/**
	 * 指向页面缓存对象的指针。
	 */
	struct file_cache_space *cache_space;
	/**
	 * 通过此字段将队列链入不同状态的链表中。
	 */
	struct double_list	list;
	/**
	 * 文件的address_space对象。
	 */
	struct file_cache_space	cache_data;
	/**
	 * 如果文件节点是一个块设备，则此字段表示关联的设备描述符
	 * 由全局块设备锁保护
	 */
	struct block_device	*blkdev;
	/**
	 * 块大小的order数，如4096的块大小，其值为12
	 */
	unsigned int		block_size_order;

	/**
	 * 引用计数器。
	 */
	struct accurate_counter		ref_count;
	/**
	 * 文件类型与访问权限。
	 */
	umode_t			mode;
	/**
	 * 硬链接数目。
	 */
	unsigned int		link_count;
	/**
	 * 所有者ID
	 */
	uid_t			uid;
	/**
	 * 所有者组标识符。
	 */
	gid_t			gid;
	/**
	 * 文件的字节数。
	 */
	loff_t			file_size;
	/**
	 * 上次访问文件的时间。
	 */
	struct timespec		access_time;
	/**
	 * 上次修改文件的时间。
	 */
	struct timespec		data_modify_time;
	/**
	 * 上次修改索引节点的时间。
	 */
	struct timespec		meta_modify_time;
	/**
	 * 块的字节数。
	 * 太无耻了，这是欺骗stat系统调用的，实际没有用
	 */
	unsigned long		block_size;
	/**
	 * 版本号，每次修改目录节点后递增。
	 */
	unsigned long		version;
	/**
	 * 文件的块数量。以512字节为单位
	 */
	unsigned long		block_count;
	/**
	 * 文件最后一个块的字节数。
	 */
	unsigned short          i_bytes;
	/**
	 * 非0表示文件是一个套接字。
	 */
	unsigned char		i_sock;
	/**
	 * 保护索引节点某些字段的自旋锁。
	 */
	struct smp_lock		i_lock;	/* block_count, i_bytes, maybe file_size */

	/**
	 * inode所在的超级块。
	 */
	struct super_block	*super;
	/**
	 * 文件锁链表,通过此字段将文件上的所有锁链接成一个单链表。
	 */
	struct file_lock	*i_flock;
	/**
	 * 如果内核是一个管道则非0
	 */
	struct pipe_inode_info	*i_pipe;

	/**
	 * 表示字符设备的内部数据结构。当inode指向一个字符设备文件时，该字段包含了指向struct cdev结构的指针。
	 */
	struct char_device		*chrdev;
	/**
	 * 次设备号索引。
	 */
	int			minor;

	/**
	 * 索引节点版本号。由某些文件系统使用。
	 */
	__u32			generation;
	/**
	 * 索引节点弄脏的时间，以jiffies为单位。
	 */
	unsigned long		dirtied_jiffies;

	/**
	 * 文件系统的安装标志。
	 */
	unsigned int		flags;

	/**
	 * 用于写进程的引用计数。
	 */
	struct accurate_counter		write_users;
	/**
	 * 索引节点安全结构。
	 */
	void			*i_security;
	/**
	 * 文件系统私有数据指针。
	 */
	void		*private;
#ifdef __NEED_I_SIZE_ORDERED
	/**
	 * SMP系统为i_size字段获取一致性时使用的顺序计数器。
	 */
	seqcount_t		i_size_seqcount;
#endif
};

/* Page cache limit. The filesystems should put that into their max_file_size 
   limits, otherwise bad things can happen in VM. */ 
#if BITS_PER_LONG==32
#define MAX_LFS_FILESIZE	(((u64)PAGE_CACHE_SIZE << (BITS_PER_LONG-1))-1) 
#elif BITS_PER_LONG==64
#define MAX_LFS_FILESIZE 	0x7fffffffffffffffUL
#endif

struct filenode_stat {
	int nr_inodes;
	int nr_unused;
	int dummy[5];
};
extern struct filenode_stat fnode_stat;

extern int simple_statfs(struct super_block *, struct kstatfs *);
extern void generic_delete_fnode(struct file_node *file_node);
struct super_block *load_ram_filesystem(struct file_system_type *fs_type,
	int flags, void *data,
	int (*fill_super)(struct super_block *, void *, int));

extern struct file_node *fnode_alloc(struct super_block *);
extern int simple_readpage(struct file *file, struct page_frame *page);
extern int simple_prepare_write(struct file *file, struct page_frame *page,
			unsigned offset, unsigned to);
extern int simple_commit_write(struct file *file, struct page_frame *page,
				unsigned offset, unsigned to);

static inline void fnode_set_size(struct file_node *inode, loff_t i_size)
{
#if BITS_PER_LONG==32 && defined(CONFIG_SMP)
	write_seqcount_begin(&inode->i_size_seqcount);
	inode->i_size = i_size;
	write_seqcount_end(&inode->i_size_seqcount);
#elif BITS_PER_LONG==32 && defined(CONFIG_PREEMPT)
	preempt_disable();
	inode->i_size = i_size;
	preempt_enable();
#else
	inode->file_size = i_size;
#endif
}

static inline loff_t fnode_size(struct file_node *file_node)
{
#if BITS_PER_LONG==32 && defined(CONFIG_SMP)
	loff_t file_size;
	unsigned int seq;

	do {
		seq = read_seqcount_begin(&file_node->i_size_seqcount);
		file_size = file_node->file_size;
	} while (read_seqcount_retry(&file_node->i_size_seqcount, seq));
	return file_size;
#elif BITS_PER_LONG==32 && defined(CONFIG_PREEMPT)
	loff_t file_size;

	preempt_disable();
	file_size = file_node->file_size;
	preempt_enable();
	return file_size;
#else
	return file_node->file_size;
#endif
}

static inline void fnode_update_file_size(struct file_node *file_node, loff_t file_size)
{
#if BITS_PER_LONG==32 && defined(CONFIG_SMP)
	write_seqcount_begin(&file_node->i_size_seqcount);
	file_node->file_size = file_size;
	write_seqcount_end(&file_node->i_size_seqcount);
#elif BITS_PER_LONG==32 && defined(CONFIG_PREEMPT)
	preempt_disable();
	file_node->file_size = file_size;
	preempt_enable();
#else
	file_node->file_size = file_size;
#endif
}

#ifdef __KERNEL__

/**
 * 属性标志。用于notify_change，表示哪些属性发生了变化
 */
#define ATTR_MODE	1
#define ATTR_UID	2
#define ATTR_GID	4
#define ATTR_SIZE	8
#define ATTR_ATIME	16
#define ATTR_MTIME	32
#define ATTR_CTIME	64
#define ATTR_ATIME_SET	128
#define ATTR_MTIME_SET	256


/*
 * This is the Inode Attributes structure, used for notify_change().  It
 * uses the above definitions as flags, to know which values have changed.
 * Also, in this manner, a Filesystem can look at only the values it cares
 * about.  Basically, these are the attributes that the VFS layer can
 * request to change from the FS layer.
 *
 * Derek Atkins <warlord@MIT.EDU> 94-10-20
 */
/**
 * NFS客户端修改服务器端文件属性时用到的结构。
 */
struct fnode_attr {
	unsigned int	valid;
	/**
	 * 文件保护位 
	 */
	umode_t		ia_mode;
	/** 
	 * 文件拥有者的id 
	 */
	uid_t		uid;
	/** 
	 * 文件所属的组 
	 */
	gid_t		gid;
	/**
	 * 文件大小 
	 */
	loff_t		size;
	/**
	 * 文件的最后访问时间 
	 */
	struct timespec	ia_atime;
	/**
	 * 文件的最后修改时间 
	 */
	struct timespec	ia_mtime;
	struct timespec	ia_ctime;
	unsigned int	ia_attr_flags;
};

/*
 * Track a single file's readahead state
 */
/**
 * 预读算法使用的主要数据结构.每个文件对象在它的f_ra字段中存放该描述符。
 */
struct file_ra_state {
	/**
	 * 当前窗内第一页的索引。
	 */
	unsigned long start;		/* Current window */
	/**
	 * 当前窗内的页数。禁止预读时为－1，0表示当前窗为空
	 */
	unsigned long size;
	/**
	 * 控制预读的标志。
	 */
	unsigned long flags;		/* ra flags RA_FLAG_xxx*/
	/**
	 * 连续高速缓存命中数(进程请求的页同时又在页高速缓存内)
	 */
	unsigned long cache_hit;	/* cache hit count*/
	/**
	 * 进程请求的最后一页的索引。
	 */
	unsigned long prev_page;	/* Cache last read() position */
	/**
	 * 预读窗内的第一页的索引
	 */
	unsigned long ahead_start;	/* Ahead window */
	/**
	 * 预读窗的页数(0表示预读窗口空)
	 */
	unsigned long ahead_size;
	/**
	 * 预读窗口的最大页数(0表示预读窗永久禁止)
	 * 该字段的初始值(缺省值)存放在该文件所在块设备的backing_dev_info描述符中
	 * 应用可以通过调用posix_fadvise系统调用修改一个打开文件的ra_pages字段。
	 */
	unsigned long max_ra_pages;		/* Maximum readahead window */
	/**
	 * 预读命中计数器(用于内存映射文件)
	 */
	unsigned long mmap_hit;		/* Cache hit stat for mmap accesses */
	/**
	 * 预读失败计数器(用于内存映射文件)
	 */
	unsigned long mmap_miss;	/* Cache miss stat for mmap accesses */
};
void
file_ra_state_init(struct file_ra_state *ra, struct file_cache_space *space);

/**
 * 代表一个打开的文件。由内核在open时创建。当文件的所有实例都被关闭后，才释放该结构。
 */
struct file {
	/**
	 * 通过此字段，将文件链接到超级块的文件链表中
	 */
	struct double_list	super;
	/**
	 * 文件对应的目录项结构。除了用filp->fnode_cache->d_inode的方式来访问索引节点结构之外，设备驱动程序的开发者们一般无需关心dentry结构。
	 */
	struct filenode_cache		*fnode_cache;
	/**
	 * 含有该文件的已经安装的文件系统。
	 */
	struct mount_desc         *mount;
	/**
	 * 与文件相关的操作。内核在执行open操作时，对这个指针赋值，以后需要处理这些操作时就读取这个指针。
	 * 不能为了方便而保存起来。也就是说，可以在任何需要的时候修改文件的关联操作。即"方法重载"。
	 */
	struct file_ops	*file_ops;
	/**
	 * 文件对象的引用计数。
	 * 指引用文件对象的进程数。内核也可能增加此计数。
	 */
	struct accurate_counter		f_count;
	/**
	 * 文件标志。如O_RONLY、O_NONBLOCK和O_SYNC。为了检查用户请求是否非阻塞式的操作，驱动程序需要检查O_NONBLOCK标志，其他标志较少用到。
	 * 检查读写权限应该查看f_mode而不是f_flags。
	 */
	unsigned int 		flags;
	/** 
	 * 文件模式。FMODE_READ和FMODE_WRITE分别表示读写权限。
	 */
	mode_t			f_mode;
	/**
	 * 当前的读写位置。它是一个64位数。如果驱动程序需要知道文件中的当前位置，可以读取这个值但是不要去修改它。
	 * read/write会使用它们接收到的最后那个指针参数来更新这一位置。
	 */
	loff_t			pos;
#if 0
	/**
	 * 通过信号进行邋IO进程通知的数据。
	 */
	struct fown_struct	f_owner;
#endif
	/**
	 * 用户的UID和GID.
	 */
	unsigned int		f_uid, f_gid;
	/**
	 * 文件的预读状态。
	 */
	struct file_ra_state	readahead;

	/**
	 * 一次操作能读写的最大字节数。当前设置为2^31-1
	 */
	size_t			max_bytes;
	/**
	 * 版本号，每次使用后递增。
	 */
	unsigned long		version;
	/**
	 * 文件对象的安全结构指针。
	 */
	void			*f_security;

	/* needed for tty driver, and maybe others */
	/**
	 * open系统调用在调用驱动程序的open方法前将这个指针置为NULL。驱动程序可以将这个字段用于任何目的或者忽略这个字段。
	 * 驱动程序可以用这个字段指向已分配的数据，但是一定要在内核销毁file结构前在release方法中释放内存。
	 * 它是跨系统调用时保存状态的非常有用的资源。
	 */
	void			*private_data;

#ifdef CONFIG_EPOLL
	/* Used by fs/eventpoll.c to link all the hooks to this file */
	/**
	 * 文件的事件轮询等待者链表头。
	 */
	struct double_list	f_ep_links;
	/**
	 * 保护f_ep_links的自旋锁。
	 */
	struct smp_lock		f_ep_lock;
#endif /* #ifdef CONFIG_EPOLL */
	/**
	 * 指向文件地址空间的对象。
	 */
	struct file_cache_space	*cache_space;
};

extern void release_file(struct file *file);

//无效的文件描述符
#define INVALIDATE_FILE ((struct file *)1)
//未使用的文件描述符
#define UNUSED_FILE (NULL)

struct file_lock {
	struct file_lock *fl_next;	/* singly linked list for this file_node  */
	struct file_lock *fl_nextlink;	/* doubly linked list of all locks */
	struct file_lock *fl_prevlink;	/* used to simplify lock removal */
	struct task_desc *fl_owner;
	struct wait_queue *fl_wait;
	char fl_type;
	char fl_whence;
	off_t fl_start;
	off_t fl_end;
};

/**
 * 异步等待文件描述符
 */
struct fasync_struct {
	int    magic;
	struct fasync_struct	*fa_next; /* singly linked list */
	struct file 		*fa_file;
};

#define FASYNC_MAGIC 0x4601

//#include <fs/msdos_fs_sb.h>
//#include <fs/ext2_fs_sb.h>

extern void kill_fasync(struct fasync_struct *fa, int sig);

int register_chrdev(unsigned int major, const char * name, struct file_ops *fops);
	int unregister_chrdev(unsigned int major, const char * name);

extern int blkdev_open(struct file_node * file_node, struct file * filp);
extern struct file_ops def_blk_fops;
extern struct file_node_ops blkdev_inode_operations;

extern int register_chrdev(unsigned int, const char *, struct file_ops *);
extern int unregister_chrdev(unsigned int major, const char * name);
extern int chrdev_open(struct file_node * file_node, struct file * filp);
extern struct file_ops def_chrdev_fops;
extern struct file_node_ops chrdev_inode_operations;

static inline void init_fifo(struct file_node * file_node)
{
}

extern struct file_system_type *lookup_file_system(const char *name);

extern int fs_may_mount(devno_t dev);
extern int fs_may_umount(devno_t dev, struct file_node * mount_file_systems);
int file_readonly_all(struct super_block *sb);

extern struct blkbuf_desc ** buffer_pages;
extern int nr_buffers;
extern int buffermem;
extern int nr_buffer_heads;

#define BUF_CLEAN 0
#define BUF_UNSHARED 1 /* Buffers that were shared but are not any more */
#define BUF_LOCKED 2   /* Buffers scheduled for write */
#define BUF_LOCKED1 3  /* Supers, inodes */
#define BUF_DIRTY 4    /* Dirty buffers, not yet scheduled for write */
#define BUF_SHARED 5   /* Buffers shared */
#define NR_LIST 6

extern int check_disk_change(devno_t dev);
extern int invalidate_fnode_filesystem(struct super_block *);
extern void invalidate_buffers(devno_t dev);
extern int floppy_is_wp(int minor);
extern void sync_file_nodes(int wait);
extern void sync_filesystems(int wait);
extern void sync_dev(devno_t dev);
extern int fsync_dev(devno_t dev);
extern int notify_change(struct file_node *, struct fnode_attr *);
extern int namei(const char * pathname, struct file_node ** res_inode);
extern int lnamei(const char * pathname, struct file_node ** res_inode);
extern int get_write_access(struct file_node * file_node);
static inline void put_write_access(struct file_node * file_node)
{
	accurate_dec(&file_node->write_users);
}

extern int open_fnode_cache(const char *, int, int, struct filenode_lookup_s *);
extern int do_mknod(const char * filename, int mode, devno_t dev);
extern void loosen_file_node(struct file_node * file_node);
extern void __hold_file_node(struct file_node * file_node);
extern struct file_node * get_empty_inode(void);

extern void fnode_clean(struct file_node *);
extern struct file_node * get_pipe_inode(void);
extern struct file * file_alloc(void);
extern struct blkbuf_desc * get_hash_table(devno_t dev, int block, int size);
extern struct blkbuf_desc * getblk(devno_t dev, int block, int size);
extern void submit_block_requests(int rw, int nr, struct blkbuf_desc * bh[]);
extern void ll_rw_page(int rw, int dev, int nr, char * buffer);
extern void ll_rw_swap_file(int rw, int dev, unsigned int *b, int nb, char *buffer);
extern int is_read_only(int dev);
extern int set_blocksize(struct block_device *, int);
extern struct blkbuf_desc * bread(devno_t dev, int block, int size);
extern unsigned long bread_page(unsigned long addr,devno_t dev,int b[],int size,int no_share);
extern struct blkbuf_desc * breada(devno_t dev,int block, int size, 
				   unsigned int pos, unsigned int filesize);
unsigned long generate_cluster(devno_t dev, int b[], int size);

extern void show_buffers(void);
extern void mount_file_systems(void);

extern int char_read(struct file_node *, struct file *, char *, int);
extern int block_read(struct file_node *, struct file *, char *, int);
extern int read_ahead[];

extern int char_write(struct file_node *, struct file *, char *, int);
extern int block_write(struct file_node *, struct file *,const char *, int);

extern int generic_mmap(struct file_node *, struct file *, struct vm_area_struct *);

extern int file_fsync(struct file_node *, struct file *);

extern void dcache_add(struct file_node *, const char *, int, unsigned long);
extern int dcache_lookup(struct file_node *, const char *, int, unsigned long *);

extern int fnode_check_attr(struct file_node *, struct fnode_attr *);
extern int fnode_setattr(struct file_node *, struct fnode_attr *);

extern struct file_node * fnode_find_alloc(struct super_block *, unsigned long);
void wake_up_filenode(struct file_node *fnode);
struct file_node *fnode_read(struct super_block *sb, unsigned long ino);

#define memcpy_fromfs(to, from, n) memcpy((to),(from),(n))

#define memcpy_tofs(to, from, n) memcpy((to),(from),(n))

#define put_fs_byte(x,addr) (*(char *)(addr) = (x))
#define put_fs_long(x,addr) (*(long *)(addr) = (x))
#define put_fs_word(x,addr) (*(short *)(addr) = (x))

#define get_fs_byte(addr) get_user_byte((char *)(addr))
static inline unsigned char get_user_byte(const char *addr)
{
	return *addr;
}
#define get_fs_long(addr) get_user_long((long *)(addr))
static inline unsigned long get_user_long(const long *addr)
{
	return *addr;
}
static inline unsigned short get_user_word(const short *addr)
{
	return *addr;
}
extern void kill_fasync(struct fasync_struct *fa, int sig);

#define get_fs_word(addr) get_user_word((short *)(addr))

//xby_debug
#define paf_BUFFER 0
#define paf_NOBUFFER 0
#define suser() (1)
#define fsuser() (1)


#endif /* __KERNEL__ */

extern void __mark_filenode_dirty(struct file_node *, int);
static inline void mark_fnode_dirty(struct file_node *file_node)
{
	__mark_filenode_dirty(file_node, FNODE_DIRTY);
}

extern int writeback_fnode_sumbit_wait(struct file_node *, int);

extern void init_special_filenode(struct file_node *, umode_t, devno_t);

extern int simple_getattr(struct mount_desc *, struct filenode_cache *, struct file_attribute *);
extern void generic_get_file_attribute(struct file_node *, struct file_attribute *);
extern ssize_t generic_file_read(struct file *, char __user *, size_t, loff_t *);
extern ssize_t generic_file_write(struct file *, const char __user *, size_t, loff_t *);
extern int generic_file_mmap(struct file *, struct vm_area_struct *);
extern int simple_sync_file(struct file *, struct filenode_cache *, int);

extern ssize_t generic_file_sendfile(struct file *, loff_t *, size_t, read_actor_t, void *);
extern loff_t generic_file_llseek(struct file *file, loff_t offset, int origin);
extern struct filenode_cache *simple_lookup(struct file_node *, struct filenode_cache *, struct filenode_lookup_s *);
extern int simple_link(struct filenode_cache *, struct file_node *, struct filenode_cache *);
extern int simple_unlink(struct file_node *, struct filenode_cache *);
extern int simple_rmdir(struct file_node *, struct filenode_cache *);
extern int simple_rename(struct file_node *old_dir, struct filenode_cache *old_dentry,
		struct file_node *new_dir, struct filenode_cache *new_dentry);


extern struct file_ops simple_dir_ops;

static inline fnode_num_t parent_fnode_num(struct filenode_cache *fnode_cache)
{
	fnode_num_t res;

	smp_lock(&fnode_cache->lock);
	res = fnode_cache->parent->file_node->node_num;
	smp_unlock(&fnode_cache->lock);
	return res;
}

/**
 * 保护超级块对象链表的自旋锁。
 */
extern struct smp_lock super_block_lock;


/* And dynamically-tunable limits and defaults: */
struct files_stat_struct {
	int file_refcount;		/* read only */
	int nr_free_files;	/* read only */
	int max_files;		/* tunable */
};
extern struct files_stat_struct files_stat;


#define FMODE_READ 1
#define FMODE_WRITE 2

/* Internal kernel extensions */
#define FMODE_LSEEK	4
#define FMODE_PREAD	8
#define FMODE_PWRITE	FMODE_PREAD	/* These go hand in hand */
extern void file_move_to_list(struct file *f, struct double_list *list);

struct char_device;
extern void loosen_chrdev(struct char_device *p);

#define MAY_EXEC 1
#define MAY_WRITE 2
#define MAY_READ 4
#define MAY_APPEND 8

#define __IS_FLG(file_node,flg) ((file_node)->super->mount_flags & (flg))
#define IS_POSIXACL(file_node)	__IS_FLG(file_node, MS_POSIXACL)

#define hold_file(x)	accurate_inc(&(x)->f_count)
#define file_refcount(x)	accurate_read(&(x)->f_count)
void fastcall loosen_file(struct file *file);
struct file fastcall *file_find(unsigned int fd);

static inline struct file_node *file_fnode(const struct file *f)
{
	return f->fnode_cache->file_node;
}

static inline int security_file_permission (struct file *file, int mask)
{
	return 0;
}

extern struct file_node * fnode_find_alloc_special(struct super_block *, unsigned long, int (*test)(struct file_node *, void *), int (*set)(struct file_node *, void *), void *);

/*
 * Might pages of this file have been modified in userspace?
 * Note that i_mmap_writable counts all VM_SHARED vmas: do_mmap_pgoff
 * marks vma as VM_SHARED if it is shared, and the file was opened for
 * writing i.e. vma may be mprotected writable even if now readonly.
 */
static inline int fnode_mapped_writeble(struct file_cache_space *space)
{
	return space->i_mmap_writable != 0;
}

struct disk_device;
extern int revalidate_disk(struct disk_device *);

struct super_block *load_common_filesystem(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data,
	int (*fill_super)(struct super_block *, void *, int));
void unload_common_filesystem(struct super_block *sb);

extern struct block_device *blkdev_load_desc(const char *);
extern struct block_device *blkdev_open_exclude(const char *, int, void *);
extern void blkdev_close_exclude(struct block_device *);
extern int superblock_set_blocksize(struct super_block *, int);

extern struct cache_space_ops def_blk_cachespace_ops;

typedef int map_block_f(struct file_node *file_node, sector_t iblock,
			struct blkbuf_desc *bh_result, int create);
typedef int get_blocks_f(struct file_node *file_node, sector_t iblock,
			unsigned long max_blocks,
			struct blkbuf_desc *bh_result, int create);
typedef void dio_iodone_t(struct file_node *file_node, loff_t offset,
			ssize_t bytes, void *private);

#define PAGECACHE_TAG_DIRTY	0
#define PAGECACHE_TAG_WRITEBACK	1

struct writeback_control;

struct block_io_desc;
extern void blk_submit_request(bool, struct block_io_desc *);


enum {
	DIO_LOCKING = 1, /* need locking between buffered and direct access */
	DIO_NO_LOCKING,  /* bdev; no locking at all between buffered/direct */
	DIO_OWN_LOCKING, /* filesystem locks buffered and direct internally */
};

int __init init_devfs(void);
extern void mount_devfs_fs(void);
struct super_block *load_single_filesystem(struct file_system_type *fs_type,
	int flags, void *data,
	int (*fill_super)(struct super_block *, void *, int));

extern int blkdev_read_only(struct block_device *);
void unload_isolate_filesystem(struct super_block *sb);

extern struct file_ops bad_sock_fops;
extern struct file_ops def_fifo_fops;

ssize_t __generic_file_write(struct file *file, const struct io_segment *iov,
				unsigned long nr_segs, loff_t *ppos);

#define OSYNC_METADATA	(1<<0)
#define OSYNC_DATA	(1<<1)
#define OSYNC_INODE	(1<<2)

extern int generic_read_link(struct filenode_cache *, char __user *, int);
extern ssize_t generic_read_dir(struct file *, char __user *, size_t, loff_t *);

extern int blkdev_ioctl(struct file_node *, struct file *, unsigned, unsigned long);
extern ssize_t generic_file_readv(struct file *filp, const struct io_segment *iov, 
	unsigned long nr_segs, loff_t *ppos);

extern void loosen_block_device(struct block_device *);
struct super_block *load_isolate_filesystem(struct file_system_type *, char *,
			struct super_block_ops *ops, unsigned long);
extern struct file_node * hold_file_node(struct file_node *);
extern void blkdev_set_size(struct block_device *, loff_t size);
extern int open_block_device(struct block_device *, mode_t, unsigned);

extern int sb_min_blocksize(struct super_block *, int);
extern void blkdev_exclude_invalidate(struct block_device *);
extern int close_block_device(struct block_device *);
extern int blkdev_exclude(struct block_device *, void *);

#define S_IFMT  00170000
#define S_IFSOCK 0140000
#define S_IFLNK	 0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
/**
 * 设置用户和组的执行时ID
 */
#define S_ISUID  0004000
#define S_ISGID  0002000
#define S_ISVTX  0001000

#define S_ISLNK(m)	(((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m)	(((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)	(((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)	(((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)	(((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m)	(((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m)	(((m) & S_IFMT) == S_IFSOCK)

#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100

#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010

#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001

#ifdef __KERNEL__
#define S_IRWXUGO	(S_IRWXU|S_IRWXG|S_IRWXO)
#define S_IALLUGO	(S_ISUID|S_ISGID|S_ISVTX|S_IRWXUGO)
#define S_IRUGO		(S_IRUSR|S_IRGRP|S_IROTH)
#define S_IWUGO		(S_IWUSR|S_IWGRP|S_IWOTH)
#define S_IXUGO		(S_IXUSR|S_IXGRP|S_IXOTH)
#endif


/* Inode flags - they have nothing to super_block flags now */

#define S_SYNC		1	/* Writes are synced at once */
#define S_NOATIME	2	/* Do not update access times */
#define S_APPEND	4	/* Append-only file */
#define S_IMMUTABLE	8	/* Immutable file */
#define S_DEAD		16	/* removed, but still open directory */
#define S_NOQUOTA	32	/* Inode is not counted to quota */
#define S_DIRSYNC	64	/* Directory modifications are synchronous */
#define S_NOCMTIME	128	/* Do not update file c/mtime */
#define S_SWAPFILE	256	/* Do not truncate: swapon got its bmaps */

/*
 * Superblock flags that can be altered by MS_REMOUNT
 */
#define MS_RMT_MASK     (MFLAG_RDONLY|MS_SYNCHRONOUS|MS_MANDLOCK|MS_NOATIME|MS_NODIRATIME)

#define IS_RDONLY(file_node) (((file_node)->super) && ((file_node)->super->mount_flags & MFLAG_RDONLY))
#define IS_NOSUID(file_node) ((file_node)->flags & MS_NOSUID)
#define IS_NODEV(file_node) ((file_node)->flags & MS_NODEV)
#define IS_NOEXEC(file_node) ((file_node)->flags & MS_NOEXEC)
#define IS_SYNC(file_node) ((file_node)->flags & MS_SYNCHRONOUS)

static inline bool file_is_sync(struct file *file)
{
	struct file_cache_space *space = file->cache_space;
	struct file_node *file_node = space->fnode;

	return (file->flags & O_SYNC) || IS_SYNC(file_node);
}

#define IS_APPEND(file_node) ((file_node)->flags & S_APPEND)
#define IS_IMMUTABLE(file_node) ((file_node)->flags & S_IMMUTABLE)

/*
 * umount2() flags
 */
#define MNT_FORCE	1	/* Forcibly unmount */
#define MNT_DETACH	2	/* Detach from tree only */
#define MNT_EXPIRE	4	/* Mark for expiry */

#define BLOCK_SIZE 1024
#define BLOCK_SIZE_BITS 10

#define NR_FILE  8192	/* this can well be larger on a larger system */

#define DT_UNKNOWN	0
#define DT_FIFO		1
#define DT_CHR		2
#define DT_DIR		4
#define DT_BLK		6
#define DT_REG		8
#define DT_LNK		10
#define DT_SOCK		12
#define DT_WHT		14

extern void fnode_init(struct file_node *);
/* Invalid file_node operations hold_percpu_var fs/bad_inode.c */
extern void build_bad_file_node(struct file_node *);
extern int is_bad_file_node(struct file_node *);
extern void takeout_file_node(struct file_node *);

extern int generic_file_open(struct file_node * file_node, struct file * filp);

ssize_t __blockdev_direct_IO(int rw, struct async_io_desc *aio, struct file_node *file_node,
	struct block_device *bdev, const struct io_segment *iov, loff_t offset,
	unsigned long nr_segs, get_blocks_f get_blocks, dio_iodone_t end_io,
	int lock_type);
static inline ssize_t blockdev_direct_IO(int rw, struct async_io_desc *aio,
	struct file_node *file_node, struct block_device *bdev, const struct io_segment *iov,
	loff_t offset, unsigned long nr_segs, get_blocks_f get_blocks,
	dio_iodone_t end_io)
{
	return __blockdev_direct_IO(rw, aio, file_node, bdev, iov, offset,
				nr_segs, get_blocks, end_io, DIO_LOCKING);
}

extern ssize_t generic_file_aio_write(struct async_io_desc *, const char __user *, size_t, loff_t);
int sync_fnode(struct file_node *file_node, struct writeback_control *wbc);

/*
 * Attribute flags.  These should be or-ed together to figure out what
 * has been changed!
 */
#define ATTR_MODE	1
#define ATTR_UID	2
#define ATTR_GID	4
#define ATTR_SIZE	8
#define ATTR_ATIME	16
#define ATTR_MTIME	32
#define ATTR_CTIME	64
#define ATTR_ATIME_SET	128
#define ATTR_MTIME_SET	256
#define ATTR_FORCE	512	/* Not a change, but a change it */
#define ATTR_ATTR_FLAG	1024
#define ATTR_KILL_SUID	2048
#define ATTR_KILL_SGID	4096

#define __IS_FLG(file_node,flg) ((file_node)->super->mount_flags & (flg))

#define IS_DIRSYNC(file_node)	(__IS_FLG(file_node, MS_SYNCHRONOUS|MS_DIRSYNC) || \
					((file_node)->flags & (S_SYNC|S_DIRSYNC)))
#define IS_MANDLOCK(file_node)	__IS_FLG(file_node, MS_MANDLOCK)

#define IS_NOQUOTA(file_node)	((file_node)->flags & S_NOQUOTA)
#define IS_APPEND(file_node)	((file_node)->flags & S_APPEND)
#define IS_IMMUTABLE(file_node)	((file_node)->flags & S_IMMUTABLE)
#define IS_NOATIME(file_node)	(__IS_FLG(file_node, MS_NOATIME) || ((file_node)->flags & S_NOATIME))
#define IS_NODIRATIME(file_node)	__IS_FLG(file_node, MS_NODIRATIME)
#define IS_POSIXACL(file_node)	__IS_FLG(file_node, MS_POSIXACL)

#define IS_DEADDIR(file_node)	((file_node)->flags & S_DEAD)
#define IS_NOCMTIME(file_node)	((file_node)->flags & S_NOCMTIME)
#define IS_SWAPFILE(file_node)	((file_node)->flags & S_SWAPFILE)

extern ssize_t generic_file_aio_read(struct async_io_desc *, char __user *, size_t, loff_t);

//extern int vfs_unlink(struct file_node *, struct filenode_cache *);
ssize_t generic_file_writev(struct file *filp, const struct io_segment *iov, 
			unsigned long nr_segs, loff_t *ppos);

extern loff_t no_llseek(struct file *file, loff_t offset, int origin);
extern int register_chrdev_region(devno_t, unsigned, const char *);

extern int nonseekable_open(struct file_node * file_node, struct file * filp);

extern int alloc_chrdev_region(devno_t *, unsigned, unsigned, const char *);
extern void unregister_chrdev_region(devno_t, unsigned);
extern int writeback_submit_data(struct file_cache_space *space,
	loff_t start, loff_t end, int sync_mode);
int writeback_wait_data(struct file_cache_space *space);
unsigned long invalidate_mapping_pages(struct file_cache_space *space,
					pgoff_t start, pgoff_t end);
unsigned long invalidate_page_cache(struct file_cache_space *space);
void deactivate_super(struct super_block *sb);
int init_isolate_superblock(struct super_block *s, void *data);
extern int remount_filesystem(struct super_block *sb, int flags,
			 void *data, int force);
extern void blkdev_detach(struct file_node *file_node);
extern void file_detach_superblock(struct file *f);
extern int vfs_create(struct file_node *, struct filenode_cache *, int, struct filenode_lookup_s *);
extern int do_truncate(struct filenode_cache *, loff_t start);
extern int vfs_stat(char __user *, struct file_attribute *);
extern int vfs_lstat(char __user *, struct file_attribute *);
extern loff_t default_llseek(struct file *file, loff_t offset, int origin);
extern int rw_verify_area(int, struct file *, loff_t *, size_t);
extern ssize_t vfs_read(struct file *, char __user *, size_t, loff_t *);
extern ssize_t vfs_write(struct file *, const char __user *, size_t, loff_t *);
extern ssize_t __generic_file_aio_write(struct async_io_desc *aio,
	const struct io_segment *io_seg, unsigned long seg_count, loff_t *ppos);

#define fops_get(fops) \
       (fops)
#define fops_put(fops) \
       do { } while(0)

void __init init_file_systems(void);

/* node.c */
extern int generic_symlink(struct file_node *, const char *, int);
extern struct file_node_ops generic_symlink_fnode_ops;
extern int generic_follow_link(struct filenode_cache *, struct filenode_lookup_s *);
extern void generic_loosen_link(struct filenode_cache *, struct filenode_lookup_s *);

/* node_bond.c */
extern void fnode_update_time(struct file_node *file_node, int ctime);
void fnode_update_atime(struct file_node *fnode);
extern void __insert_inode_hash(struct file_node *, unsigned long hashval);
extern void takeout_file_node(struct file_node *);
void putin_file_node(struct file_node *file_node);
/**
 * 将文件节点的逻辑块映射为块设备块编号
 */
static inline int fnode_map_block(struct file_node *fnode, int block)
{
	sector_t ret = 0;

	if (fnode->cache_space->ops->map_block)
		ret = fnode->cache_space->ops->map_block(fnode->cache_space, block);

	return ret;
}

/* super.c */
/**
 * 所有的超级块对象链表。
 */
extern struct double_list all_super_blocks;
extern int anon_blkdev_major;
extern void sync_superblocks(void);
extern struct super_block *get_associated_superblock(struct block_device *);
extern void unlock_loosen_super_block(struct super_block *sb);
int __loosen_super(struct super_block *super);
void loosen_super(struct super_block *super);

/* cache_space.c */
int generic_readpages(struct file_cache_space *space, struct double_list *pages,
				unsigned nr_pages, map_block_f map_block);
int generic_readpage(struct page_frame *page, map_block_f map_block);

typedef int (*writepage_journal_f)(struct file_cache_space *,
	struct page_frame *, struct writeback_control *);
int writepage_journal(struct file_cache_space *space,
	struct page_frame *page, struct writeback_control *wbc);
int __generic_writepages(struct file_cache_space *space,
		struct writeback_control *wbc, map_block_f map_block,
		writepage_journal_f writepage_journal);
/**
 * 日志型文件系统回写页面的方法
 */
static inline int writepages_journal(struct file_cache_space *space, struct writeback_control *wbc)
{
	return __generic_writepages(space, wbc, NULL, writepage_journal);
}

/* file.c */
extern struct smp_lock files_lock;
extern ssize_t file_sync_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos);
extern ssize_t file_sync_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos);

/* open.c */
extern struct file *
real_open_file(struct filenode_lookup_s *, int);
extern struct file *file_open(const char *, int, int);
extern int file_close(struct file *filp);

/* readdir.c */

extern loff_t vfs_llseek(struct file *file, loff_t offset, int origin);

extern int vfs_readdir(struct file *, filldir_t, void *);

static inline void file_accessed(struct file *file)
{
	if (!(file->flags & O_NOATIME))
		fnode_update_atime(file->fnode_cache->file_node);
}

/* simple.c */
extern int simple_empty(struct filenode_cache *);
extern int simple_dir_open(struct file_node *, struct file *);
extern int simple_dir_close(struct file_node *, struct file *);

extern loff_t simple_dir_lseek(struct file *, loff_t, int);
extern int simple_dir_readdir(struct file *, void *, filldir_t);

/* pagemap.h */
void __remove_from_page_cache(struct page_frame *page);
void remove_from_page_cache(struct page_frame *page);

/* devfs */
#undef __printf_1_2
#undef __printf_3_4
#define __printf_1_2 __attribute__((format (printf, 1, 2)))
#define __printf_3_4 __attribute__((format (printf, 3, 4)))
extern __printf_1_2 int devfs_mk_dir(const char *fmt, ...);
extern __printf_3_4 int devfs_mk_blkdev(devno_t dev, umode_t mode, const char *fmt, ...);
extern __printf_3_4 int devfs_mk_chrdev(devno_t dev, umode_t mode, const char *fmt, ...);
extern int devfs_mk_symlink(const char *name, const char *link);
extern __printf_1_2 int devfs_remove(const char *fmt, ...);
#endif /* _DIM_SUM_FS_H */
