#include <dim-sum/blk_dev.h>
#include <dim-sum/err.h>
#include <dim-sum/fs.h>
#include <dim-sum/lext3_fs.h>
#include <dim-sum/stacktrace.h>
#include <dim-sum/writeback.h>

#include "internal.h"

void lext3_journal_abort_handle(struct blkbuf_desc *blkbuf,
	struct journal_handle *handle, int err)
{
	char msg_buf[16];
	const char *msg;

	msg = lext3_error_msg(NULL, err, msg_buf);
	if (!handle->error)
		handle->error = err;

	if (handle_is_aborted(handle))
		return;

	printk(KERN_ERR "aborting transaction: %s\n", msg);
	dump_stack();

	journal_abort_handle(handle);
}

/**
 * 强制将当前事务提交，并等待其结束
 */
int lext3_commit_journal(struct super_block *super)
{
	struct journal *journal;
	int ret;

	/**
	 * 只读装载文件系统，不能提交日志
	 */
	if (super->mount_flags & MFLAG_RDONLY)
		return 0;

	journal = super_to_lext3(super)->journal;
	super->dirty = 0;

	ret = journal_force_commit(journal);

	return ret;
}

struct journal_handle *
lext3_start_journal(struct file_node *fnode, int block_count)
{
	struct super_block *super;
	struct journal *journal;

	/**
	 * 只读文件系统不需要日志
	 */
	super = fnode->super;
	if (super->mount_flags & MFLAG_RDONLY)
		return ERR_PTR(-EROFS);

	/**
	 * 日志异常，设置文件系统为只读
	 */
	journal = super_to_lext3(super)->journal;
	if (journal_is_aborted(journal)) {
		lext3_abort_filesystem(super, "Detected aborted journal");
		return ERR_PTR(-EROFS);
	}

	return journal_start(journal, block_count);
}

/**
 * 完成事务，如果有错误，则在超级块中标记一下
 */
int lext3_stop_journal(struct journal_handle *handle)
{
	struct super_block *super;
	int err;
	int rc;

	super = handle->transaction->journal->private;
	err = handle->error;
	rc = journal_stop(handle);

	if (!err)
		err = rc;

	lext3_std_error(super, err);

	return err;
}

/**
 * 为截断操作开启日志
 */
struct journal_handle *lext3_start_trunc_journal(struct file_node *fnode) 
{
	struct journal_handle *handle;
	unsigned long block_count;

	block_count = lext3_journal_blocks_truncate(fnode);
	handle = lext3_start_journal(fnode, block_count);
	if (!IS_ERR(handle))
		return handle;

	lext3_std_error(fnode->super, PTR_ERR(handle));

	return handle;
}

/**
 * 当数据块已经被释放，但是仍然在日志中时调用
 * 那么需要调用此函数从日志中撤销
 */
int lext3_forget(struct journal_handle *handle, int is_metadata,
	struct file_node *fnode, struct blkbuf_desc *blkbuf, int blocknr)
{
	int journal_type;
	int mount_type;
	int err;

	might_sleep();

	mount_type = lext3_test_opt(fnode->super, LEXT3_MOUNT_DATA_FLAGS);
	journal_type = lext3_get_journal_type(fnode);

	if (mount_type == LEXT3_MOUNT_JOURNAL_DATA ||
	    (!is_metadata && journal_type != JOURNAL_TYPE_FULL)) {
		if (blkbuf)
			return lext3_journal_forget(handle, blkbuf);

		return 0;
	}

	/**
	 * 不对数据块进行日志
	 * 但是是元数据，或者是视同元数据的数据块(如符号链接)
	 */
	err = lext3_journal_revoke(handle, blocknr, blkbuf);
	if (err)
		lext3_abort_filesystem(fnode->super,
			"error %d when attempting revoke", err);

	return err;
}

int lext3_journal_get_create_access(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf)
{
	int err;

	err = journal_get_create_access(handle, blkbuf);
	if (err)
		lext3_journal_abort_handle(blkbuf, handle, err);

	return err;
}

int lext3_journal_get_undo_access(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf, int *credits)
{
	int err;

	err = journal_get_undo_access(handle, blkbuf, credits);
	if (err)
		lext3_journal_abort_handle(blkbuf, handle, err);

	return err;
}

int __lext3_journal_get_write_access(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf, int *credits)
{
	int err;

	err = journal_get_write_access(handle, blkbuf, credits);
	if (err)
		lext3_journal_abort_handle(blkbuf, handle, err);

	return err;
}

/**
 * 当归还数据块时调用
 */
int lext3_journal_forget(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf)
{
	int err;

	err = journal_forget(handle, blkbuf);
	if (err)
		lext3_journal_abort_handle(blkbuf, handle, err);

	return err;
}

int lext3_journal_revoke(struct journal_handle *handle,
	unsigned long blocknr, struct blkbuf_desc *blkbuf)
{
	int err;

	err = journal_revoke(handle, blocknr, blkbuf);
	if (err)
		lext3_journal_abort_handle(blkbuf, handle, err);

	return err;
}

/**
 * 在ordered模式下，标记数据块为脏
 * 这些数据块将会先于元数据日志之前被写入磁盘
 */
int lext3_journal_dirty_data(struct journal_handle *handle,
						struct blkbuf_desc *blkbuf)
{
	int err;

	err = journal_dirty_data(handle, blkbuf);
	if (err)
		lext3_journal_abort_handle(blkbuf, handle, err);

	return err;
}

/**
 * 标记元数据块为脏
 */
int lext3_journal_dirty_metadata(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf)
{
	int err;

	err = journal_dirty_metadata(handle, blkbuf);
	if (err)
		lext3_journal_abort_handle(blkbuf, handle, err);

	return err;
}

void lext3_init_journal_params(struct super_block *super, struct journal *journal)
{
	struct lext3_superblock *lext3_super = super_to_lext3(super);

	if (lext3_super->commit_interval)
		journal->commit_interval = lext3_super->commit_interval;

	smp_lock(&journal->state_lock);
	if (lext3_test_opt(super, LEXT3_MOUNT_BARRIER))
		journal->flags |= JSTATE_BARRIER;
	else
		journal->flags &= ~JSTATE_BARRIER;
	smp_unlock(&journal->state_lock);
}

/**
 * 从块设备中加载日志
 */
static struct journal *
load_journal_dev(struct super_block *super, char *dev_name)
{
	struct lext3_superblock_phy *phy_super;
	int hard_blocksize, blocksize;
	struct blkbuf_desc *blkbuf;
	struct block_device *blkdev;
	unsigned long block_offset;
	unsigned long super_block;
	struct journal *journal;
	bool uuid_match;
	int start;
	int len;

	/**
	 * 打开块设备。获得块设备描述符的指针。
	 * 注意以排它方式打开设备
	 */
	blkdev = blkdev_open_exclude(dev_name, 0, super);
	if (blkdev == NULL)
		return NULL;
	if (IS_ERR(blkdev))
		return (struct journal *)blkdev;

	blocksize = super->block_size;
	hard_blocksize = blkdev_hardsect_size(blkdev);
	if (blocksize < hard_blocksize) {
		printk(KERN_ERR
			"LEXT3: blocksize too small for journal device.\n");
		goto fail_dev;
	}

	super_block = LEXT3_MIN_BLOCK_SIZE / blocksize;
	block_offset = LEXT3_MIN_BLOCK_SIZE % blocksize;
	set_blocksize(blkdev, blocksize);
	/**
	 * 从磁盘中读取超级块
	 */
	if (!(blkbuf = __blkbuf_read_block(blkdev, super_block, blocksize))) {
		printk(KERN_ERR "LEXT3: couldn't read super_block of "
			"external journal\n");
		goto fail_dev;
	}

	phy_super = (struct lext3_superblock_phy *)
		(((char *)blkbuf->block_data) + block_offset);
	/**
	 * 块设备竟然不是日志设备
	 */
	if ((le16_to_cpu(phy_super->magic) != LEXT3_SUPER_MAGIC) ||
	    !(le32_to_cpu(phy_super->feature_incompat) &
	      LEXT3_FEATURE_INCOMPAT_JOURNAL_DEV)) {
		printk(KERN_ERR "LEXT3: external journal has "
					"bad super_block\n");
		loosen_blkbuf(blkbuf);
		goto fail_dev;
	}

	/**
	 * 判断UUID是否匹配，避免误操作破坏文件系统
	 */
	uuid_match = !memcmp(super_to_lext3(super)->phy_super->journal_uuid,
		phy_super->uuid, 16);
	if (!uuid_match) {
		printk(KERN_ERR "LEXT3: journal UUID does not match\n");
		loosen_blkbuf(blkbuf);
		goto fail_dev;
	}

	len = le32_to_cpu(phy_super->block_count);
	start = super_block + 1;
	loosen_blkbuf(blkbuf);

	journal = journal_alloc_dev(blkdev, super->blkdev,start, len, blocksize);
	if (!journal) {
		printk(KERN_ERR "LEXT3: failed to create device journal\n");
		goto fail_dev;
	}
	journal->private = super;

	/**
	 * 读取日志超级块并等待完成
	 */
	submit_block_requests(READ, 1, &journal->super_blkbuf);
	blkbuf_wait_unlock(journal->super_blkbuf);
	if (!blkbuf_is_uptodate(journal->super_blkbuf)) {
		printk(KERN_ERR "LEXT3-fs: I/O error on journal device\n");
		goto fail_journal;
	}

	if (be32_to_cpu(journal->super_block->users) != 1) {
		printk(KERN_ERR "LEXT3: External journal has more than one "
			"user (unsupported) - %d\n",
			be32_to_cpu(journal->super_block->users));
		goto fail_journal;
	}

	super_to_lext3(super)->blkdev_journal = blkdev;
	lext3_init_journal_params(super, journal);

	return journal;
fail_journal:
	journal_destroy(journal);
fail_dev:
	lext3_loosen_blkdev(blkdev);
	return NULL;
}

/**
 * 当日志重放完成后
 * 修改文件系统的标记
 */
void lext3_clear_journal_err(struct super_block *super,
	struct lext3_superblock_phy *phy_super)
{
	struct journal *journal;
	int errno;

	journal = super_to_lext3(super)->journal;

	errno = journal_errno(journal);
	if (errno) {
		super_to_lext3(super)->mount_state |= LEXT3_ERROR_FS;
		phy_super->state |= cpu_to_le16(LEXT3_ERROR_FS);
		lext3_commit_super(super, phy_super, 1);

		journal_clear_err(journal);
	}
}

/**
 * 初始化文件系统时，加载其日志
 */
int lext3_load_journal(struct super_block *super,
	struct lext3_superblock_phy *phy_super)
{
	struct journal *journal;
	int ro_dev;
	int err = 0;

	ro_dev = blkdev_read_only(super->blkdev);

	if (LEXT3_HAS_INCOMPAT_FEATURE(super, LEXT3_FEATURE_INCOMPAT_RECOVER)) {
		if (super->mount_flags & MFLAG_RDONLY) {
			printk(KERN_INFO "LEXT3: INFO: recovery "
					"required on readonly filesystem.\n");
			if (ro_dev) {
				printk(KERN_ERR "LEXT3: write access "
					"unavailable, cannot proceed.\n");
				return -EROFS;
			}
			printk (KERN_INFO "LEXT3: write access will "
					"be enabled during recovery.\n");
		}
	}

	/**
	 * 目前仅仅支持从块设备加载日志
	 */
	if (!(journal = load_journal_dev(super, "/dev/vda/part2")))
		return -EINVAL;

	/**
	 * 要求修正日志格式，使用新版本的日志
	 */
	if (!ro_dev && lext3_test_opt(super, LEXT3_MOUNT_UPDATE_JOURNAL)) {
		/**
		 * 那就修改日志格式吧
		 */
		err = journal_update_format(journal);
		if (err)  {
			printk(KERN_ERR "LEXT3: error updating journal.\n");
			journal_destroy(journal);
			return err;
		}
	}

	/**
	 * 插除或者加载/恢复日志
	 */
	if (!LEXT3_HAS_INCOMPAT_FEATURE(super, LEXT3_FEATURE_INCOMPAT_RECOVER))
		err = journal_wipe(journal, !ro_dev);
	if (!err)
		err = journal_engorge(journal);

	if (err) {
		printk(KERN_ERR "EXT3-fs: error loading journal.\n");
		journal_destroy(journal);
		return err;
	}

	super_to_lext3(super)->journal = journal;
	lext3_clear_journal_err(super, phy_super);

	return 0;
}

int lext3_create_journal(struct super_block *super,
	struct lext3_superblock_phy *phy_super, int journal_inum)
{
	/* TO-DO */
	BUG();
	return -EINVAL;
}
