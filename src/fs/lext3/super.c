#include <dim-sum/beehive.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/block_buf.h>
#include <dim-sum/err.h>
#include <dim-sum/fs.h>
#include <dim-sum/journal.h>
#include <dim-sum/lext3_fs.h>
#include <dim-sum/stacktrace.h>
#include <dim-sum/timer.h>

/**
 * 默认的超级块块号
 */
#define LEXT3_SUPERBLOCK_POS 1024

/**
 * 超级块为VFS层分配文件节点的分配器
 */
static struct beehive_allotter *node_allotter;

static void mark_recovery_complete(struct super_block * super,
					struct lext3_superblock_phy * phy_super);
static int setup_super(struct super_block *super, struct lext3_superblock_phy *phy_super,
			    int read_only);

#define log2(n) ffz(~(n))

/**
 * 解析用户装载文件系统的选项
 * 目前省略，直接赋予默认值
 */
static int parse_mount_opts(char *options, struct super_block *super,
	unsigned long *journal_fnode_num, unsigned long *blocks_count,
	int is_remount)
{
	if (journal_fnode_num)
		*journal_fnode_num = 0;

	if (blocks_count)
		*blocks_count = 0;
	
	return 1;
}

/**
 * 计算最大的文件长度
 */
static loff_t calc_max_filesize(int order)
{
	loff_t res = LEXT3_DIRECT_BLOCKS;

	res += 1LL << (order-2);
	res += 1LL << (2*(order-2));
	res += 1LL << (3*(order-2));
	res <<= order;
	/**
	 * 硬件扇区号限制
	 */
	if (res > (512LL << 32) - (1 << order))
		res = (512LL << 32) - (1 << order);

	return res;
}

static inline int calc_group_block_count(struct super_block *super)
{
	struct lext3_superblock *lext3_super = super_to_lext3(super);
	int desc_per_block = LEXT3_DESC_PER_BLOCK(super);

	return (lext3_super->groups_count + desc_per_block - 1) / desc_per_block;
}

static inline int feature_has_meta_bg(struct super_block *super)
{
	return LEXT3_HAS_INCOMPAT_FEATURE(super, LEXT3_FEATURE_INCOMPAT_META_BG);
}

static inline char *get_journal_type(struct super_block *super)
{
	unsigned long opt = lext3_test_opt(super, LEXT3_MOUNT_DATA_FLAGS);

	if (opt == LEXT3_MOUNT_JOURNAL_DATA)
		return "journal";

	if (opt == LEXT3_MOUNT_ORDERED_DATA)
		return "ordered";

	return "writeback";
}

static inline int check_mount_count(struct super_block *super)
{
	struct lext3_superblock *lext3_super = super_to_lext3(super);
	struct lext3_superblock_phy *phy_super = lext3_super->phy_super;
	__s16 max_mount_count;
	__s16 mount_count;

	max_mount_count = (__s16) le16_to_cpu(phy_super->max_mount_count);
	mount_count = le16_to_cpu(phy_super->mount_count);

	if (max_mount_count >= 0 && mount_count >=max_mount_count)
		return 1;

	return 0;
}

static inline int check_mount_interval(struct super_block *super)
{
	struct lext3_superblock *lext3_super = super_to_lext3(super);
	struct lext3_superblock_phy *phy_super = lext3_super->phy_super;
	__u32 interval;
	__u32 last;

	interval = le32_to_cpu(phy_super->check_interval);
	last = le32_to_cpu(phy_super->last_check);

	if (interval && (last + interval <= get_seconds()))
		return 1;

	return 0;
}

static struct file_node *lext3_fnode_alloc(struct super_block *super)
{
	struct lext3_file_node *lext3_fnode;

	/**
	 * PAF_NOFS!
	 */
	lext3_fnode = beehive_alloc(node_allotter, PAF_NOFS | __PAF_ZERO);
	if (!lext3_fnode)
		return NULL;

	list_init(&lext3_fnode->orphan);
	sema_init(&lext3_fnode->truncate_sem, 1);
	lext3_fnode->vfs_fnode.version = 1;
	fnode_init(&lext3_fnode->vfs_fnode);

	return &lext3_fnode->vfs_fnode;
}

static void lext3_fnode_free(struct file_node *fnode)
{
	beehive_free(node_allotter, fnode_to_lext3(fnode));
}

/**
 * 释放对块设备的引用
 */
int lext3_loosen_blkdev(struct block_device *blkdev)
{
	/**
	 * 取消对块设备的排它设置
	 */
	blkdev_exclude_invalidate(blkdev);
	/**
	 * 关闭块设备
	 */
	return close_block_device(blkdev);
}

static int lext3_remove_journal_blkdev(struct lext3_superblock *lext3_super)
{
	struct block_device *blkdev;
	int ret = -ENODEV;

	blkdev = lext3_super->blkdev_journal;
	if (blkdev) {
		ret = lext3_loosen_blkdev(blkdev);
		lext3_super->blkdev_journal = NULL;
	}

	return ret;
}

static void lext3_loosen_super (struct super_block *super)
{
	struct lext3_superblock_phy *phy_super;
	struct lext3_superblock *lext3_super;
	int i;

	lext3_super = super_to_lext3(super);
	phy_super = lext3_super->phy_super;

	journal_destroy(lext3_super->journal);
	if (!(super->mount_flags & MFLAG_RDONLY)) {
		LEXT3_CLEAR_INCOMPAT_FEATURE(super, LEXT3_FEATURE_INCOMPAT_RECOVER);
		phy_super->state = cpu_to_le16(lext3_super->mount_state);
		/**
		 * 标记超级块为脏并提交到磁盘
		 */
		blkbuf_mark_dirty(lext3_super->blkbuf_super);
		lext3_commit_super(super, phy_super, 1);
	}

	/**
	 * 释放块组描述符的块
	 */
	for (i = 0; i < lext3_super->blkcount_grpdesc; i++)
		loosen_blkbuf(lext3_super->blkbuf_grpdesc[i]);
	kfree(lext3_super->blkbuf_grpdesc);

	approximate_counter_destroy(&lext3_super->free_block_count);
	approximate_counter_destroy(&lext3_super->free_fnode_count);
	approximate_counter_destroy(&lext3_super->dir_count);

	loosen_blkbuf(lext3_super->blkbuf_super);

	/**
	 * 内存中的孤儿节点必须为空，否则说明有BUG
	 */
	ASSERT(list_is_empty(&lext3_super->orphans));

	blkdev_invalidate_pgcache(super->blkdev);
	/**
	 * 日志块设备是单独的设备
	 * 刷新日志缓存并解除对日志设备的引用
	 */
	if (lext3_super->blkdev_journal &&
	    lext3_super->blkdev_journal != super->blkdev) {
		blkdev_sync(lext3_super->blkdev_journal);
		blkdev_invalidate_pgcache(lext3_super->blkdev_journal);
		lext3_remove_journal_blkdev(lext3_super);
	}

	super->fs_info = NULL;
	kfree(lext3_super);

	return;
}

/**
 * 超级块总是在日志中更新，不需要特别的处理
 */
static void lext3_write_super (struct super_block * super)
{
	if (down_trylock(&super->sem) == 0)
		BUG();

	super->dirty = 0;
}

static int lext3_sync_fs(struct super_block *super, int wait)
{
	trans_id_t target;

	super->dirty = 0;
	/**
	 * 提交并等待日志完成
	 */
	if (journal_start_commit(super_to_lext3(super)->journal, &target)) {
		if (wait)
			log_wait_commit(super_to_lext3(super)->journal, target);
	}

	return 0;
}

/**
 * 同步文件系统，并且暂停接受读写请求
 * 在块设备做快照时调用
 */
static void lext3_lock_fs(struct super_block *super)
{
	super->dirty = 0;

	if (!(super->mount_flags & MFLAG_RDONLY)) {
		struct journal *journal = super_to_lext3(super)->journal;

		/**
		 * 日志屏障，不再接受更新
		 * 并且刷新日志
		 */
		journal_lock_updates(journal);
		journal_flush(journal);

		/**
		 * 标记文件系统完整，不需要恢复
		 * 然后回写超级块
		 */
		LEXT3_CLEAR_INCOMPAT_FEATURE(super, LEXT3_FEATURE_INCOMPAT_RECOVER);
		lext3_commit_super(super, super_to_lext3(super)->phy_super, 1);
	}
}

/**
 * 快照完成后调用
 */
static void lext3_unlock_fs(struct super_block *super)
{
	if (!(super->mount_flags & MFLAG_RDONLY)) {
		down(&super->sem);
		LEXT3_SET_INCOMPAT_FEATURE(super, LEXT3_FEATURE_INCOMPAT_RECOVER);
		lext3_commit_super(super, super_to_lext3(super)->phy_super, 1);
		up(&super->sem);
		journal_unlock_updates(super_to_lext3(super)->journal);
	}
}

static int lext3_remount (struct super_block *super, int *flags, char *data)
{
	struct lext3_superblock_phy *phy_super;
	struct lext3_superblock *lext3_super;
	unsigned long tmp;

	lext3_super = super_to_lext3(super);
	if (!parse_mount_opts(data, super, &tmp, NULL, 1))
		return -EINVAL;

	if (lext3_super->mount_opt & LEXT3_MOUNT_ABORT)
		lext3_abort_filesystem(super, "Abort forced by user");

	super->mount_flags = super->mount_flags & ~MS_POSIXACL;
	if (lext3_super->mount_opt & LEXT3_MOUNT_POSIX_ACL)
		super->mount_flags |= MS_POSIXACL;

	phy_super = lext3_super->phy_super;

	lext3_init_journal_params(super, lext3_super->journal);

	/**
	 * 只读属性发生了变化
	 */
	if ((*flags & MFLAG_RDONLY) != (super->mount_flags & MFLAG_RDONLY)) {
		if (lext3_super->mount_opt & LEXT3_MOUNT_ABORT)
			return -EROFS;

		if (*flags & MFLAG_RDONLY) {
			super->mount_flags |= MFLAG_RDONLY;
			mark_recovery_complete(super, phy_super);
		} else {
			__le32 ret;

			/**
			 * 从只读变非只读，但是有些选项只能在只读模式下支持
			 */
			ret = LEXT3_HAS_RO_COMPAT_FEATURE(super,
					~LEXT3_FEATURE_RO_COMPAT_SUPP);
			if (ret) {
				printk(KERN_WARNING "LEXT3: %s: couldn't "
				       "remount RDWR because of unsupported "
				       "optional features (%x).\n",
				       super->blkdev_name, le32_to_cpu(ret));
				return -EROFS;
			}

			lext3_clear_journal_err(super, phy_super);
			lext3_super->mount_state = le16_to_cpu(phy_super->state);
			if (!setup_super (super, phy_super, 0))
				super->mount_flags &= ~MFLAG_RDONLY;
		}
	}

	return 0;
}

static int lext3_statfs (struct super_block *super, struct kstatfs *buf)
{
	struct lext3_superblock_phy *phy_super;
	unsigned long overhead;
	int i;

	phy_super = super_to_lext3(super)->phy_super;

	if (lext3_test_opt (super, LEXT3_MOUNT_MINIX_DF))
		overhead = 0;
	else {
		unsigned long groups_count;

		groups_count = super_to_lext3(super)->groups_count;
		overhead = le32_to_cpu(phy_super->first_data_block);

		for (i = 0; i < groups_count; i++)
			overhead += lext3_group_has_super(super, i) +
				lext3_blkcount_groupdesc(super, i);

		overhead += (2 + super_to_lext3(super)->fnode_blocks_per_group)
						* groups_count;
	}

	buf->f_type = LEXT3_SUPER_MAGIC;
	buf->f_bsize = super->block_size;
	buf->f_blocks = le32_to_cpu(phy_super->block_count) - overhead;
	buf->f_bfree = lext3_count_free_blocks (super);
	buf->f_bavail = buf->f_bfree - le32_to_cpu(phy_super->reserve_blocks);
	if (buf->f_bavail < 0)
		buf->f_bavail = 0;
	buf->f_files = le32_to_cpu(phy_super->fnode_count);
	buf->f_ffree = lext3_count_free_fnodes (super);
	buf->f_namelen = LEXT3_NAME_LEN;

	return 0;
}

static void lext3_clean_fnode(struct file_node *file_node)
{
	lext3_discard_reservation(file_node);
}

static struct super_block_ops lext3_superblock_ops = {
	.alloc	= lext3_fnode_alloc,
	.free	= lext3_fnode_free,
	.read_fnode	= lext3_read_fnode,
	.write_fnode	= lext3_write_fnode,
	.dirty_fnode	= lext3_dirty_fnode,
	.delete_fnode	= lext3_delete_fnode,
	.clean_fnode	= lext3_clean_fnode,
	.loosen_super	= lext3_loosen_super,
	.write_super	= lext3_write_super,
	.sync_fs	= lext3_sync_fs,
	.lock_fs = lext3_lock_fs,
	.unlock_fs	= lext3_unlock_fs,
	.statfs		= lext3_statfs,
	.remount	= lext3_remount,
};

static int load_super_block(struct super_block *super,
	struct lext3_superblock *lext3_super, int silent)
{
	struct lext3_superblock_phy *phy_super;
	unsigned long block_offset = 0;
	struct blkbuf_desc *blk_desc;
	unsigned long block_num;
	int hard_blocksize;
	int blocksize;

	/**
	 * 根据块设备的物理块大小，以及LEXT3文件系统默认值
	 * 来设置文件系统块大小
	 */
	blocksize = sb_min_blocksize(super, LEXT3_MIN_BLOCK_SIZE);
	if (!blocksize) {
		printk(KERN_ERR "LEXT3: unable to set blocksize\n");
		return -ENODEV;
	}
	block_num = lext3_super->super_pos / blocksize;
	block_offset = lext3_super->super_pos % blocksize;

	/**
	 * 从块设备读取超级块的内容
	 */
	blk_desc = blkbuf_read_block(super, block_num);
	if (!blk_desc) {
		printk (KERN_ERR "LEXT3: unable to read super_block\n");
		return -ENODEV;
  	} else {
		char *blk_data = (char *)blk_desc->block_data;

		phy_super = (struct lext3_superblock_phy *)(blk_data + block_offset);
		lext3_super->blkbuf_super = blk_desc;
  	}

	super->magic = le16_to_cpu(phy_super->magic);
	if (super->magic != LEXT3_SUPER_MAGIC)
		goto fail_recognize;

	blocksize = BLOCK_SIZE << le32_to_cpu(phy_super->blksize_order_1024);
	if (blocksize < LEXT3_MIN_BLOCK_SIZE ||
	    blocksize > LEXT3_MAX_BLOCK_SIZE) {
		printk(KERN_ERR 
		       "LEXT3: Unsupported filesystem blocksize %d on %s.\n",
		       blocksize, super->blkdev_name);
		return -ENODEV;
	}

	hard_blocksize = blkdev_hardsect_size(super->blkdev);
	if (blocksize < hard_blocksize) {
		printk(KERN_ERR "LEXT3: blocksize %d too small for "
		       "device blocksize %d.\n", blocksize, hard_blocksize);
		return -ENODEV;
	}

	if (super->block_size != blocksize) {
		loosen_blkbuf (blk_desc);
		lext3_super->blkbuf_super = NULL;
		superblock_set_blocksize(super, blocksize);
		block_num = lext3_super->super_pos / blocksize;
		block_offset = lext3_super->super_pos % blocksize;
		blk_desc = blkbuf_read_block(super, block_num);
		if (!blk_desc) {
			printk(KERN_ERR 
			       "LEXT3: Can't read super_block on 2nd try.\n");
			return -ENODEV;
		} else {
			char *blk_data = (char *)blk_desc->block_data;

			blk_data += block_offset;
			phy_super = (struct lext3_superblock_phy *)blk_data;
			lext3_super->blkbuf_super = blk_desc;
		}

		if (phy_super->magic != cpu_to_le16(LEXT3_SUPER_MAGIC)) {
			printk (KERN_ERR 
				"LEXT3: Magic mismatch, very weird !\n");
			return -ENODEV;
		}
	}

	lext3_super->phy_super = phy_super;
	return 0;

fail_recognize:
	if (!silent)
		printk(KERN_ERR "VFS: Can't find lext3 filesystem on dev %s.\n",
		       super->blkdev_name);
	return -ENODEV;	
}

static int recognize_super(struct super_block *super,
	struct lext3_superblock *lext3_super, unsigned long *journal_fnode_num,
	void *data, int silent)
{
	struct lext3_superblock_phy *phy_super;
	unsigned long def_mount_opts;
	unsigned long mount_opts = 0;
	__le32 features;
	int blocksize;

	phy_super = lext3_super->phy_super;
	/**
	 * 处理默认的装载标志
	 */
	def_mount_opts = le32_to_cpu(phy_super->default_mount_opts);
	if (def_mount_opts & LEXT3_DEFM_DEBUG)
		mount_opts |=LEXT3_MOUNT_DEBUG;
	if (def_mount_opts & LEXT3_DEFM_BSDGROUPS)
		mount_opts |= LEXT3_MOUNT_GRPID;
	if (def_mount_opts & LEXT3_DEFM_UID16)
		mount_opts |= LEXT3_MOUNT_NO_UID32;
	if (def_mount_opts & LEXT3_DEFM_XATTR_USER)
		mount_opts |= LEXT3_MOUNT_XATTR_USER;
	if (def_mount_opts & LEXT3_DEFM_ACL)
		mount_opts |= LEXT3_MOUNT_POSIX_ACL;

	switch (def_mount_opts & LEXT3_DEFM_JMODE) {
	case LEXT3_DEFM_JMODE_DATA:
		mount_opts |= LEXT3_MOUNT_JOURNAL_DATA;
		break;
	case LEXT3_DEFM_JMODE_ORDERED:
		mount_opts |= LEXT3_MOUNT_ORDERED_DATA;
		break;
	case LEXT3_DEFM_JMODE_WBACK:
		mount_opts |= LEXT3_MOUNT_WRITEBACK_DATA;
		break;
	}

	switch (le16_to_cpu(lext3_super->phy_super->errors)) {
	case LEXT3_ERRORS_PANIC:
		mount_opts |= LEXT3_MOUNT_ERRORS_PANIC;
		break;
	case LEXT3_ERRORS_RO:
		mount_opts |= LEXT3_MOUNT_ERRORS_RO;
		break;
	}

	/**
	 * 根据物理块设备中的装载标志
	 * 确定最终的装载标志
	 */
	mount_opts |= LEXT3_MOUNT_RESERVATION;
	lext3_super->mount_opt |= mount_opts;
	super->mount_flags &= ~MS_POSIXACL;
	if (lext3_super->mount_opt & LEXT3_MOUNT_POSIX_ACL)
		super->mount_flags |= MS_POSIXACL;

	lext3_super->reserve_uid = le16_to_cpu(phy_super->def_reserve_uid);
	lext3_super->reserve_gid = le16_to_cpu(phy_super->def_reserve_gid);

	/**
	 * 解析用户指定的装载选项
	 */
	if (!parse_mount_opts ((char *)data, super, journal_fnode_num, NULL, 0))
		return -ENODEV;

	/**
	 * 最老的文件版本，不应当有兼容标志
	 * 文件系统可能存在问题
	 */
	if (le32_to_cpu(phy_super->revision) == LEXT3_GOOD_OLD_REV &&
	    (phy_super->feature_compat ||
	     phy_super->feature_ro_compat ||
	     phy_super->feature_incompat))
		printk(KERN_WARNING "LEXT3: feature flags set on rev 0 fs, "
		       "running e2fsck is recommended\n");

	/**
	 * 检查兼容标志
	 */
	features = LEXT3_HAS_INCOMPAT_FEATURE(super,
				~LEXT3_FEATURE_INCOMPAT_SUPP);
	if (features) {
		printk(KERN_ERR "LEXT3: %s: couldn't mount because of "
		       "unsupported optional features (%x).\n",
		       super->blkdev_name, le32_to_cpu(features));
		return -ENODEV;
	}

	features = LEXT3_HAS_RO_COMPAT_FEATURE(super,
				~LEXT3_FEATURE_RO_COMPAT_SUPP);
	if (!(super->mount_flags & MFLAG_RDONLY) && features) {
		printk(KERN_ERR "EXT3: %s: couldn't mount RDWR because of "
		       "unsupported optional features (%x).\n",
		       super->blkdev_name, le32_to_cpu(features));
		return -ENODEV;
	}

	super->max_file_size = calc_max_filesize(super->block_size_order);

	blocksize = BLOCK_SIZE << le32_to_cpu(phy_super->blksize_order_1024);
	/**
	 * 检查文件节点长度是否正确
	 */
	if (le32_to_cpu(phy_super->revision) == LEXT3_GOOD_OLD_REV) {
		lext3_super->fnode_size = LEXT3_OLD_FNODE_SIZE;
		lext3_super->first_fnode = LEXT3_OLD_FIRST_FILENO;
	} else {
		lext3_super->fnode_size = le16_to_cpu(phy_super->fnode_size);
		lext3_super->first_fnode = le32_to_cpu(phy_super->first_fnode);
		/**
		 * 1、文件节点长度不能小于128
		 * 2、文件节点长度必须是2的幂
		 * 3、文件节点长度不能大于一个逻辑块长度
		 */
		if ((lext3_super->fnode_size < LEXT3_OLD_FNODE_SIZE) ||
		    (lext3_super->fnode_size & (lext3_super->fnode_size - 1)) ||
		    (lext3_super->fnode_size > blocksize)) {
			printk (KERN_ERR
				"LEXT3: unsupported file node size: %d\n",
				lext3_super->fnode_size);
			return -ENODEV;
		}
	}
	ASSERT(lext3_super->fnode_size > 0);

	lext3_super->block_size = LEXT3_MIN_BLOCK_SIZE <<
				   le32_to_cpu(phy_super->frag_size_order_1024);
	if (blocksize != lext3_super->block_size) {
		printk(KERN_ERR
		       "LEXT3: fragsize %lu != blocksize %u (unsupported)\n",
		       lext3_super->block_size, blocksize);
		return -ENODEV;
	}

	lext3_super->blocks_per_group = le32_to_cpu(phy_super->blocks_per_group);
	lext3_super->fnodes_per_group = le32_to_cpu(phy_super->fnodes_per_group);
	lext3_super->fnodes_per_block = blocksize / LEXT3_FNODE_SIZE(super);
	if (lext3_super->blocks_per_group == 0)
		goto fail_recognize;
	if (lext3_super->fnodes_per_block == 0)
		goto fail_recognize;

	lext3_super->fnode_blocks_per_group =
		lext3_super->fnodes_per_group / lext3_super->fnodes_per_block;
	lext3_super->group_desc_per_block =
		blocksize / sizeof(struct lext3_group_desc);
	lext3_super->group_desc_per_block_order =
		log2(lext3_super->group_desc_per_block);
	lext3_super->mount_state = le16_to_cpu(phy_super->state);
	lext3_super->addr_per_block_order = log2(LEXT3_ADDR_PER_BLOCK(super));

	if (lext3_super->blocks_per_group > blocksize * 8) {
		printk (KERN_ERR
			"EXT3-fs: #blocks per group too big: %lu\n",
			lext3_super->blocks_per_group);
		return -ENODEV;
	}

	if (lext3_super->fnodes_per_group > blocksize * 8) {
		printk (KERN_ERR
			"EXT3-fs: #inodes per group too big: %lu\n",
			lext3_super->fnodes_per_group);
		return -ENODEV;
	}

	lext3_super->groups_count = (le32_to_cpu(phy_super->block_count)
		- le32_to_cpu(phy_super->first_data_block)
		+ lext3_super->blocks_per_group - 1)
		/ lext3_super->blocks_per_group;

	return 0;

fail_recognize:
	if (!silent)
		printk(KERN_ERR "VFS: Can't find lext3 filesystem on dev %s.\n",
		       super->blkdev_name);
	return -ENODEV;	
}

/**
 * 检查块组描述符的有效性
 */
static int check_group_descriptors (struct super_block *super)
{
	struct lext3_superblock *lext3_super = super_to_lext3(super);
	struct lext3_group_desc *group_desc = NULL;
	struct lext3_superblock_phy *phy_super;
	unsigned long block;
	int desc_block = 0;
	int i;

	phy_super = lext3_super->phy_super;
	block = le32_to_cpu(phy_super->first_data_block);

	for (i = 0; i < lext3_super->groups_count; i++)
	{
		/**
		 * 块组描述符跨越逻辑块了
		 */
		if ((i % LEXT3_DESC_PER_BLOCK(super)) == 0)
			group_desc = (struct lext3_group_desc *)
					lext3_super->blkbuf_grpdesc[desc_block++]->block_data;
		/**
		 * 检查数据块位图表的块号是否正常
		 */
		if (le32_to_cpu(group_desc->datablock_bitmap) < block ||
		    le32_to_cpu(group_desc->datablock_bitmap) >=
				block + LEXT3_BLOCKS_PER_GROUP(super)) {
			lext3_enconter_error (super,
				"Block bitmap for group %d not in group (block %lu)!",
				i, (unsigned long)le32_to_cpu(group_desc->datablock_bitmap));
			return 0;
		}
		/**
		 * 检查文件节点块位图表的块号是否正常
		 */
		if (le32_to_cpu(group_desc->fnode_bitmap) < block ||
		    le32_to_cpu(group_desc->fnode_bitmap) >=
				block + LEXT3_BLOCKS_PER_GROUP(super)) {
			lext3_enconter_error (super, 
				"Inode bitmap for group %d not in group (block %lu)!",
				i, (unsigned long)le32_to_cpu(group_desc->fnode_bitmap));
			return 0;
		}
		/**
		 * 检查第一个文件节点块号是否正常
		 */
		if (le32_to_cpu(group_desc->first_fnode_block) < block ||
		    le32_to_cpu(group_desc->first_fnode_block)
			+ lext3_super->fnode_blocks_per_group
			>= block + LEXT3_BLOCKS_PER_GROUP(super)) {
			lext3_enconter_error (super,
				"Inode table for group %d not in group (block %lu)!", i,
				(unsigned long)le32_to_cpu(group_desc->first_fnode_block));
			return 0;
		}

		block += LEXT3_BLOCKS_PER_GROUP(super);
		group_desc++;
	}

	phy_super->free_blocks_count = cpu_to_le32(lext3_count_free_blocks(super));
	phy_super->free_fnodes_count=cpu_to_le32(lext3_count_free_fnodes(super));

	return 1;
}

static int load_group_desc(struct super_block *super)
{
	struct lext3_superblock *lext3_super = super_to_lext3(super);
	unsigned long bg, first_data_block, first_meta_bg;
	unsigned long block_num;
	int blkcount_grpdesc;
	int has_super = 0;
	int i, block;

	first_data_block = le32_to_cpu(lext3_super->phy_super->first_data_block);
	first_meta_bg = le32_to_cpu(lext3_super->phy_super->first_meta_block_group);

	/**
	 * 组描述符占用的逻辑块数量
	 */
	blkcount_grpdesc = calc_group_block_count(super);
	lext3_super->blkbuf_grpdesc =
		kmalloc(blkcount_grpdesc * sizeof(struct blkbuf_desc *), PAF_KERNEL);
	if (lext3_super->blkbuf_grpdesc == NULL) {
		printk (KERN_ERR "LEXT3: not enough memory\n");
		return -ENOMEM;
	}
	lext3_super->blkcount_grpdesc = blkcount_grpdesc;

	block_num = lext3_super->super_pos / super->block_size;
	for (i = 0; i < blkcount_grpdesc; i++) {
		if (!feature_has_meta_bg(super)  || (i < first_meta_bg))
			block = block_num + i + 1;
		else {
			bg = lext3_super->group_desc_per_block * i;
			if (lext3_group_has_super(super, bg))
				has_super = 1;
			block = first_data_block + has_super + (bg * lext3_super->blocks_per_group);
		}
		
		lext3_super->blkbuf_grpdesc[i] = blkbuf_read_block(super, block);
		if (!lext3_super->blkbuf_grpdesc[i]) {
			printk (KERN_ERR "LEXT3: can't read group descriptor %d\n", i);
			lext3_super->blkcount_grpdesc = i;
			return -EIO;
		}
	}
	lext3_super->blkcount_grpdesc = blkcount_grpdesc;

	if (!check_group_descriptors (super)) {
		printk (KERN_ERR "EXT3-fs: group descriptors corrupted !\n");
		return -EINVAL;
	}

	return 0;
}

static int load_journal(struct super_block *super,
	unsigned long journal_fnode_num, int silent)
{
	struct lext3_superblock *lext3_super = super_to_lext3(super);
	int journal_revoke;
	int err;

	/**
	 *	1、用户没有强制禁止日志
	 *	2、文件系统有日志功能
	 */
	if (!lext3_test_opt(super, LEXT3_MOUNT_NOLOAD) &&
	    LEXT3_HAS_COMPAT_FEATURE(super, LEXT3_FEATURE_COMPAT_HAS_JOURNAL)) {
		err = lext3_load_journal(super, lext3_super->phy_super);
		if (err)
			return err;
	} else if (journal_fnode_num) {
		err = lext3_create_journal(super, lext3_super->phy_super,
			journal_fnode_num);
		if (err)
			return err;
	} else {
		if (!silent)
			printk (KERN_ERR
				"lext3: No journal on filesystem on %s\n",
				super->blkdev_name);
		return -EINVAL;
	}

	journal_revoke = journal_has_feature(lext3_super->journal,
				0, 0, JFS_FEATURE_INCOMPAT_REVOKE);
	switch (lext3_test_opt(super, LEXT3_MOUNT_DATA_FLAGS)) {
	case 0:
		if (journal_revoke)
			lext3_set_opt(lext3_super->mount_opt, LEXT3_MOUNT_ORDERED_DATA);
		else
			lext3_set_opt(lext3_super->mount_opt, LEXT3_MOUNT_JOURNAL_DATA);

		break;
	case LEXT3_MOUNT_ORDERED_DATA:
	case LEXT3_MOUNT_WRITEBACK_DATA:
		if (!journal_revoke) {
			printk(KERN_ERR "LEXT3: Journal does not support "
			       "requested data journaling mode\n");
			return -EINVAL;
		}
		break;
	case LEXT3_MOUNT_JOURNAL_DATA:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/**
 * 修改磁盘上的文件系统版本号
 */
void lext3_update_revision(struct super_block *super)
{
	struct lext3_superblock_phy *phy_super = super_to_lext3(super)->phy_super;

	if (le32_to_cpu(phy_super->revision) > LEXT3_GOOD_OLD_REV)
		return;

	phy_super->first_fnode = cpu_to_le32(LEXT3_OLD_FIRST_FILENO);
	phy_super->fnode_size = cpu_to_le16(LEXT3_OLD_FNODE_SIZE);
	phy_super->revision = cpu_to_le32(LEXT3_DYNAMIC_REV);
}

void lext3_commit_super (struct super_block *super,
	struct lext3_superblock_phy *phy_super, int sync)
{
	struct blkbuf_desc *blkbuf_super = super_to_lext3(super)->blkbuf_super;

	if (!blkbuf_super)
		return;

	phy_super->modify_time = cpu_to_le32(get_seconds());
	phy_super->free_blocks_count =
		cpu_to_le32(lext3_count_free_blocks(super));
	phy_super->free_fnodes_count =
		cpu_to_le32(lext3_count_free_fnodes(super));

	blkbuf_mark_dirty(blkbuf_super);
	if (sync)
		sync_dirty_block(blkbuf_super);
}

static int setup_super(struct super_block *super,
	struct lext3_superblock_phy *phy_super, int read_only)
{
	struct lext3_superblock *lext3_super = super_to_lext3(super);
	int res = 0;

	if (read_only)
		return res;

	if (le32_to_cpu(phy_super->revision) > LEXT3_MAX_SUPP_REV) {
		printk (KERN_ERR "LEXT3 warning: revision level too high, "
			"forcing read-only mode\n");
		res = MFLAG_RDONLY;
	}

	if (!(lext3_super->mount_state & LEXT3_VALID_FS))
		printk (KERN_WARNING "LEXT3 warning: mounting unchecked fs, "
			"running e2fsck is recommended\n");
	else if ((lext3_super->mount_state & LEXT3_ERROR_FS))
		printk (KERN_WARNING "LEXT3 warning: mounting fs with errors, "
			"running e2fsck is recommended\n");
	else if (check_mount_count(super))
		printk (KERN_WARNING
			"LEXT3 warning: maximal mount count reached, "
			"running e2fsck is recommended\n");
	else if (check_mount_interval(super))
		printk (KERN_WARNING
			"LEXT3 warning: checktime reached, "
			"running e2fsck is recommended\n");

	if (!(__s16) le16_to_cpu(phy_super->max_mount_count))
		phy_super->max_mount_count = cpu_to_le16(LEXT3_DFL_MAX_MNT_COUNT);
	phy_super->mount_count =
		cpu_to_le16(le16_to_cpu(phy_super->mount_count) + 1);
	phy_super->mount_time = cpu_to_le32(get_seconds());

	lext3_update_revision(super);
	LEXT3_SET_INCOMPAT_FEATURE(super, LEXT3_FEATURE_INCOMPAT_RECOVER);

	lext3_commit_super(super, phy_super, 1);

	return res;
}

/**
 * 在mount时调用，用于清理orphan节点，回收空间。
 */
static void cleanup_orphan (struct super_block *super,
	struct lext3_superblock_phy *phy_super)
{
	unsigned int mount_flags = super->mount_flags;

	/**
	 * 没有孤儿文件节点
	 */
	if (!phy_super->last_orphan) {
		return;
	}

	/**
	 * 文件系统存在错误，继续清理可能会进一步破坏系统
	 */
	if (super_to_lext3(super)->mount_state & LEXT3_ERROR_FS) {
		phy_super->last_orphan = 0;
		return;
	}

	/**
	 * 需要清理孤儿节点，先临时性取消只读选项
	 */
	if (mount_flags & MFLAG_RDONLY) {
		printk(KERN_INFO "LEXT3: %s: orphan cleanup on readonly fs\n",
		       super->blkdev_name);
		super->mount_flags &= ~MFLAG_RDONLY;
	}

	/**
	 * 循环处理
	 * 直到没有孤儿节点存在。
	 */
	while (phy_super->last_orphan) {
		struct file_node *fnode;

		/**
		 * 读取孤儿文件节点 
		 */
		fnode = lext3_load_orphan_fnode(super, le32_to_cpu(phy_super->last_orphan));
		if (!fnode) {
			phy_super->last_orphan = 0;
			break;
		}

		/**
		 * 将节点添加到内存链表中。
		 */
		list_insert_front(&fnode_to_lext3(fnode)->orphan,
			&super_to_lext3(super)->orphans);

		/**
		 * 竟然被重新引用了，不能删除
		 */
		if (fnode->link_count) {
			printk(KERN_DEBUG
				"%s: truncating file_node %ld to %Ld bytes\n",
				__FUNCTION__, fnode->node_num, fnode->file_size);
			/**
			 * 整理清除磁盘上的元数据信息
			 */
			lext3_truncate(fnode);
		} else
			printk(KERN_DEBUG
				"%s: deleting unreferenced file node %ld\n",
				__FUNCTION__, fnode->node_num);

		/**
		 * 当没有谁引用文件时
		 * 会回调lext3_delete_fnode，该函数进行孤儿节点的处理
		 */
		loosen_file_node(fnode);
	}

	/**
	 * 恢复原来的装载标志
	 */
	super->mount_flags = mount_flags;
}

static void mark_recovery_complete(struct super_block *super,
	struct lext3_superblock_phy *phy_super)
{
	struct journal *journal = super_to_lext3(super)->journal;

	/**
	 * 在日志中放入一个屏障
	 * 然后再刷新日志，为后续文件系统打好基础
	 */
	journal_lock_updates(journal);
	journal_flush(journal);

	/**
	 * 系统支持日志恢复功能，现在已经恢复日志，系统完整
	 * 如果是只读装载，我们就可以声明文件系统一定完整，下次不必恢复
	 */
	if (LEXT3_HAS_INCOMPAT_FEATURE(super, LEXT3_FEATURE_INCOMPAT_RECOVER) &&
	    super->mount_flags & MFLAG_RDONLY) {
		LEXT3_CLEAR_INCOMPAT_FEATURE(super, LEXT3_FEATURE_INCOMPAT_RECOVER);
		super->dirty = 0;
		/**
		 * 修改了超级块，提交一下。
		 */
		lext3_commit_super(super, phy_super, 1);
	}

	journal_unlock_updates(journal);
}

/**
 * 加载文件系统时，填充lext3文件系统私有数据
 */
static int
lext3_fill_super(struct super_block *super, void *data, int silent)
{
	struct lext3_superblock *lext3_super;
	unsigned long journal_fnode_num = 0;
	struct file_node *fnode_root;
	int needs_recovery;
	int err;
	int i;

	lext3_super = kmalloc(sizeof(*lext3_super), PAF_KERNEL | __PAF_ZERO);
	if (!lext3_super)
		return -ENOMEM;

	super->fs_info = lext3_super;
	/**
	 * 默认将保留块交给root使用，可更改
	 */
	lext3_super->reserve_uid = LEXT3_RESERVE_UID;
	lext3_super->reserve_gid = LEXT3_RESERVE_GID;
	lext3_super->super_pos = LEXT3_SUPERBLOCK_POS;

	err = load_super_block(super, lext3_super, silent);
	if (err)
		goto fail;

	err = recognize_super(super, lext3_super, &journal_fnode_num,
					data, silent);
	if (err)
		goto fail;

	approximate_counter_init(&lext3_super->free_block_count);
	approximate_counter_init(&lext3_super->free_fnode_count);
	approximate_counter_init(&lext3_super->dir_count);
	for (i = 0; i < BLOCKGROUP_LOCK_COUNT; i++)
		smp_lock_init(&lext3_super->group_block.locks[i].lock);
	lext3_super->generation = jiffies;
	smp_lock_init(&lext3_super->gen_lock);
	list_init(&lext3_super->orphans);

	super->root_fnode_cache = NULL;
	super->ops = &lext3_superblock_ops;

	if (load_group_desc(super))
		goto fail_groups;

	needs_recovery = (lext3_super->phy_super->last_orphan != 0) ||
		LEXT3_HAS_INCOMPAT_FEATURE(super, LEXT3_FEATURE_INCOMPAT_RECOVER);

	err = load_journal(super, journal_fnode_num, silent);
	if (err)
		goto fail_journal;

	/**
	 * 读取根文件节点
	 */
	fnode_root = fnode_read(super, LEXT3_ROOT_FILENO);
	super->root_fnode_cache = fnode_cache_alloc_root(fnode_root);
	if (!super->root_fnode_cache) {
		printk(KERN_ERR "LEXT3: get root node failed\n");
		loosen_file_node(fnode_root);
		goto fail_journal;
	}

	if (!S_ISDIR(fnode_root->mode) || !fnode_root->block_count
	    || !fnode_root->file_size) {
		loosen_file_node(fnode_root);
		loosen_filenode_cache(super->root_fnode_cache);
		super->root_fnode_cache = NULL;
		printk(KERN_ERR "LEXT3: corrupt root node, run e2fsck\n");
		goto fail_journal;
	}

	setup_super(super, lext3_super->phy_super,
		super->mount_flags & MFLAG_RDONLY);

	lext3_super->mount_state |= LEXT3_ORPHAN_FS;
	cleanup_orphan(super, lext3_super->phy_super);
	lext3_super->mount_state &= ~LEXT3_ORPHAN_FS;
	mark_recovery_complete(super, lext3_super->phy_super);
	if (needs_recovery)
		printk (KERN_INFO "LEXT3: recovery complete.\n");

	printk (KERN_INFO "LEXT3: mounted filesystem with %s data mode.\n",
		get_journal_type);

	approximate_counter_mod(&lext3_super->free_block_count,
		lext3_count_free_blocks(super));
	approximate_counter_mod(&lext3_super->free_fnode_count,
		lext3_count_free_fnodes(super));
	approximate_counter_mod(&lext3_super->dir_count,
		lext3_count_dirs(super));

	return 0;

fail_journal:
	journal_destroy(lext3_super->journal);

fail_groups:
	for (i = 0; i < lext3_super->blkcount_grpdesc; i++)
		loosen_blkbuf(lext3_super->blkbuf_grpdesc[i]);
	if (lext3_super->blkbuf_grpdesc)
		kfree(lext3_super->blkbuf_grpdesc);

fail:
	lext3_remove_journal_blkdev(lext3_super);
	if (lext3_super->blkbuf_super)
		loosen_blkbuf(lext3_super->blkbuf_super);

	super->fs_info = NULL;
	kfree(lext3_super);

	return -EINVAL;
}

static struct super_block *
lext3_load_filesystem(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return load_common_filesystem(fs_type, flags, dev_name,
		data, lext3_fill_super);
}

static struct file_system_type lext3_fs_type = {
	.name		= "lext3",
	.load_filesystem		= lext3_load_filesystem,
	.unload_filesystem	= unload_common_filesystem,
	.flags	= FS_REQUIRES_DEV,
};

void init_lext3(void)
{
	node_allotter = beehive_create("ext3_inode_cache",
		sizeof(struct lext3_file_node), 0, BEEHIVE_RECLAIM_ABLE, NULL);
	if (!node_allotter)
		panic("fail to init lext3.\n");
	
	register_filesystem(&lext3_fs_type);
}
