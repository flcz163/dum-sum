#include <dim-sum/beehive.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/block_buf.h>
#include <dim-sum/delay.h>
#include <dim-sum/err.h>
#include <dim-sum/errno.h>
#include <dim-sum/highmem.h>
#include <dim-sum/journal.h>

#include "internal.h"

#define MIN_LOG_RESERVED_BLOCKS 32

static struct beehive_allotter *blkbuf_jinfo_allotter;
struct beehive_allotter *journal_handle_allotter;
static int read_superblock(struct journal *journal);

int journal_blocks_per_page(struct file_node *fnode)
{
	return 1 << (PAGE_CACHE_SHIFT - fnode->super->block_size_order);
}

void * __journal_kmalloc (size_t size, int flags, int retry)
{
	return kmalloc(size, flags | (retry ? __PAF_NOFAIL : 0));
}

/**
 * 计算日志中可用的空闲块数量
 */
int __log_free_space(struct journal *journal)
{
	int left = journal->free_blocks;

	assert_smp_lock_is_locked(&journal->state_lock);

	left -= MIN_LOG_RESERVED_BLOCKS;
	if (left <= 0)
		return 0;
	left -= (left >> 3);

	return left;
}

static const char *journal_dev_name(struct journal *journal, char *buffer)
{
	struct block_device *blkdev;

	blkdev = journal->blkdev;

	return format_block_devname(blkdev, buffer);
}

int journal_errno(struct journal *journal)
{
	int err;

	smp_lock(&journal->state_lock);
	if (journal->flags & JSTATE_ABORT)
		err = -EROFS;
	else
		err = journal->errno;
	smp_unlock(&journal->state_lock);

	return err;
}

void __journal_abort_hard(struct journal *journal)
{
	struct transaction *transaction;
	char dev_name[BDEVNAME_SIZE];

	if (journal->flags & JSTATE_ABORT)
		return;

	printk(KERN_ERR "Aborting journal on device %s.\n",
		journal_dev_name(journal, dev_name));

	smp_lock(&journal->state_lock);

	journal->flags |= JSTATE_ABORT;
	transaction = journal->running_transaction;
	if (transaction)
		__log_start_commit(journal, transaction->trans_id);

	smp_unlock(&journal->state_lock);
}

void __journal_abort_soft (struct journal *journal, int errno)
{
	if (journal->flags & JSTATE_ABORT)
		return;

	if (!journal->errno)
		journal->errno = errno;

	__journal_abort_hard(journal);

	/**
	 * 记录到超级块中，并等待超级块写入完毕
	 */
	if (errno)
		journal_update_superblock(journal, 1);
}

void journal_abort(struct journal *journal, int errno)
{
	__journal_abort_soft(journal, errno);
}

/**
 * 获得日志块的引用
 */
struct blkbuf_journal_info *hold_blkbuf_jinfo(struct blkbuf_desc *blkbuf)
{
	struct blkbuf_journal_info *blkbuf_jinfo = NULL;

	journal_info_lock(blkbuf);
	if (blkbuf_is_journaled(blkbuf)) {
		blkbuf_jinfo = blkbuf_to_journal(blkbuf);
		blkbuf_jinfo->ref_count++;
	}
	journal_info_unlock(blkbuf);
	return blkbuf_jinfo;
}

/**
 * 检查日志超级块中，是否已经支持了某个功能
 */
int journal_check_used_features (struct journal *journal, unsigned long compat,
				 unsigned long ro, unsigned long incompat)
{
	struct journal_super_phy *super;

	if (!compat && !ro && !incompat)
		return 1;

	if (journal->format_version == 1)
		return 0;

	super = journal->super_block;

	if (((be32_to_cpu(super->feature_compat) & compat) == compat) &&
	    ((be32_to_cpu(super->feature_ro_compat) & ro) == ro) &&
	    ((be32_to_cpu(super->feature_incompat) & incompat) == incompat))
		return 1;

	return 0;
}

/**
 * 检查是否支持特定功能
 */
int journal_has_feature (struct journal *journal, unsigned long compat,
	unsigned long ro, unsigned long incompat)
{
	if (!compat && !ro && !incompat)
		return 1;

	if (journal->format_version != 2)
		return 0;

	if ((compat & JFS_KNOWN_COMPAT_FEATURES) == compat &&
	    (ro & JFS_KNOWN_ROCOMPAT_FEATURES) == ro &&
	    (incompat & JFS_KNOWN_INCOMPAT_FEATURES) == incompat)
		return 1;

	return 0;
}

/**
 * 在超级块中，标记特定的功能选项
 */
int journal_set_features (struct journal *journal, unsigned long compat,
	unsigned long ro, unsigned long incompat)
{
	struct journal_super_phy *super;

	if (journal_check_used_features(journal, compat, ro, incompat))
		return 1;

	if (!journal_has_feature(journal, compat, ro, incompat))
		return 0;

	super = journal->super_block;
	super->feature_compat |= cpu_to_be32(compat);
	super->feature_ro_compat |= cpu_to_be32(ro);
	super->feature_incompat |= cpu_to_be32(incompat);

	return 1;
}

/**
 * 将日志的逻辑块转换为磁盘上的块号
 */
int journal_map_block(struct journal *journal,
	unsigned long block_num, unsigned long *retp)
{
	/**
	 * 目前仅仅支持设备日志，不支持文件节点内的日志
	 * 逻辑块号和物理块号一一对应
	 */
	*retp = block_num;

	return 0;
}

/**
 * 得到日志中下一个可用块号
 */
int journal_next_log_block(struct journal *journal, unsigned long *retp)
{
	unsigned long block_num;

	smp_lock(&journal->state_lock);
	ASSERT(journal->free_blocks > 1);

	block_num = journal->free_block_num;
	journal->free_block_num++;
	if (journal->free_block_num == journal->last_block_log)
		journal->free_block_num = journal->first_block_log;
	journal->free_blocks--;
	smp_unlock(&journal->state_lock);

	/**
	 * 将日志设备逻辑块号映射到物理块号
	 */
	return journal_map_block(journal, block_num, retp);
}

/**
 * 申请一块日志磁盘块
 */
struct blkbuf_journal_info *journal_alloc_block(struct journal *journal)
{
	struct blkbuf_desc *blkbuf;
	unsigned long block_num;
	int err;

	err = journal_next_log_block(journal, &block_num);
	if (err)
		return NULL;

	blkbuf = __blkbuf_find_alloc(journal->blkdev, block_num, journal->block_size);
	blkbuf_lock(blkbuf);
	memset(blkbuf->block_data, 0, journal->block_size);
	blkbuf_set_uptodate(blkbuf);
	blkbuf_unlock(blkbuf);

	return journal_info_hold_alloc(blkbuf);
}

/**
 * 准备将元数据块写入日志
 * 将原始块缓冲区和日志块缓冲区分别放到不同的链表中
 */
int journal_prepare_metadata_block(struct transaction *trans,
	struct blkbuf_journal_info  *blkbuf_jinfo_orig,
	struct blkbuf_journal_info **blkbuf_jinfo_new,
	int block_num)
{
	struct blkbuf_desc *blkbuf_orig;
	struct blkbuf_desc *blkbuf_new;
	struct blkbuf_journal_info *new;
	struct page_frame *new_page;
	unsigned int new_offset;
	char *mapped_data;
	int copied = 0;
	int do_escape = 0;
	__be32 first;

	blkbuf_orig = journal_to_blkbuf(blkbuf_jinfo_orig);
	ASSERT(blkbuf_is_journal_dirty(blkbuf_orig));
	/**
	 * 这是写日志块的磁盘缓冲区
	 */
	blkbuf_new = alloc_blkbuf_desc(PAF_NOFS | __PAF_NOFAIL);

	/*
	 * 如果事务已经复制过一份元数据，则使用该拷贝进行提交
	 */
	blkbuf_lock_state(blkbuf_orig);

try:
	if (blkbuf_jinfo_orig->bufdata_copy) {
		copied = 1;
		new_page = linear_virt_to_page(blkbuf_jinfo_orig->bufdata_copy);
		new_offset = offset_in_page(blkbuf_jinfo_orig->bufdata_copy);
	} else {
		new_page = journal_to_blkbuf(blkbuf_jinfo_orig)->page;
		new_offset = offset_in_page(journal_to_blkbuf(blkbuf_jinfo_orig)->block_data);
	}

	mapped_data = kmap_atomic(new_page, KM_USER0);
	first = *(__be32 *)(mapped_data + new_offset);
	if (first == cpu_to_be32(JFS_MAGIC_NUMBER))
		do_escape = 1;
	kunmap_atomic(mapped_data, KM_USER0);

	/**
	 * 前几个字节与魔法值冲突，需要转义
	 * 因此需要复制缓冲区
	 */
	if (do_escape && !copied) {
		char *tmp;

		blkbuf_unlock_state(blkbuf_orig);
		tmp = jbd_rep_kmalloc(blkbuf_orig->size, PAF_NOFS);
		blkbuf_lock_state(blkbuf_orig);
		if (blkbuf_jinfo_orig->bufdata_copy) {
			kfree(tmp);
			goto try;
		}

		blkbuf_jinfo_orig->bufdata_copy = tmp;
		mapped_data = kmap_atomic(new_page, KM_USER0);
		memcpy(tmp, mapped_data + new_offset,
			journal_to_blkbuf(blkbuf_jinfo_orig)->size);
		kunmap_atomic(mapped_data, KM_USER0);

		new_page = linear_virt_to_page(tmp);
		new_offset = offset_in_page(tmp);
		copied = 1;
	}

	/**
	 * 将提交到日志中的块转义，直接将魔法值清0即可
	 */
	if (do_escape) {
		mapped_data = kmap_atomic(new_page, KM_USER0);
		*((unsigned int *)(mapped_data + new_offset)) = 0;
		kunmap_atomic(mapped_data, KM_USER0);
	}

	blkbuf_new->state = 0;
	blkbuf_init(blkbuf_new, NULL, NULL);
	accurate_set(&blkbuf_new->ref_count, 1);
	blkbuf_unlock_state(blkbuf_orig);

	/**
	 * 为新缓冲区分配日志描述符
	 */
	new = journal_info_hold_alloc(blkbuf_new);
	blkbuf_set_page(blkbuf_new, new_page, new_offset);
	new->transaction = NULL;
	blkbuf_new->size = journal_to_blkbuf(blkbuf_jinfo_orig)->size;
	blkbuf_new->blkdev = trans->journal->blkdev;
	blkbuf_new->block_num_dev = block_num;
	blkbuf_set_mapped(blkbuf_new);
	blkbuf_set_dirty(blkbuf_new);

	*blkbuf_jinfo_new = new;

	/*
	 * 将原始的缓冲区和日志缓冲区分别放到不同的链表中
	 */
	journal_putin_list(blkbuf_jinfo_orig, trans, TRANS_QUEUE_META_ORIG);
	journal_putin_list(new, trans, TRANS_QUEUE_META_LOG);

	return do_escape | (copied << 1);
}

/**
 * 获得块缓冲区的日志信息描述符引用
 * 必要时分配描述符
 */
struct blkbuf_journal_info *
journal_info_hold_alloc(struct blkbuf_desc *blkbuf)
{
	struct blkbuf_journal_info *blkbuf_jinfo;
	struct blkbuf_journal_info *new = NULL;

repeat:
	if (!blkbuf_is_journaled(blkbuf)) {
		while (!new) {
			msleep(1);
			new = beehive_alloc(blkbuf_jinfo_allotter, PAF_NOFS | __PAF_ZERO);
			list_init(&new->list_trans);
			list_init(&new->list_checkpoint);
		}
	}

	/**
	 * 必须要获得块缓冲区的锁后重新判断
	 */
	journal_info_lock(blkbuf);
	if (blkbuf_is_journaled(blkbuf))
		blkbuf_jinfo = blkbuf_to_journal(blkbuf);
	else {
		ASSERT((accurate_read(&blkbuf->ref_count) > 0) ||
			(blkbuf->page && blkbuf->page->cache_space));

		/**
		 * 必须要分配描述符，没办法需要释放锁重试
		 */
		if (!new) {
			journal_info_unlock(blkbuf);
			goto repeat;
		}

		/**
		 * 将日志描述信息与块缓冲区绑定起来
		 */
		blkbuf_jinfo = new;
		new = NULL;
		blkbuf_set_journaled(blkbuf);
		blkbuf->private = blkbuf_jinfo;
		blkbuf_jinfo->blkbuf = blkbuf;
		hold_blkbuf(blkbuf);
	}
	blkbuf_jinfo->ref_count++;
	journal_info_unlock(blkbuf);

	if (new)
		beehive_free(blkbuf_jinfo_allotter, new);

	return blkbuf->private;
}

static void __journal_info_detach(struct blkbuf_desc *blkbuf)
{
	struct blkbuf_journal_info *blkbuf_jinfo;

	blkbuf_jinfo = blkbuf_to_journal(blkbuf);
	ASSERT(blkbuf_jinfo->ref_count >= 0);

	hold_blkbuf(blkbuf);
	if (blkbuf_jinfo->ref_count == 0) {
		if (blkbuf_jinfo->transaction == NULL &&
		    blkbuf_jinfo->next_trans == NULL &&
		    blkbuf_jinfo->checkpoint_trans == NULL) {
			ASSERT(blkbuf_is_journaled(blkbuf));
			ASSERT(journal_to_blkbuf(blkbuf_jinfo) == blkbuf);

			if (blkbuf_jinfo->bufdata_copy)
				kfree(blkbuf_jinfo->bufdata_copy);

			if (blkbuf_jinfo->undo_copy)
				kfree(blkbuf_jinfo->undo_copy);

			blkbuf->private = NULL;
			blkbuf_jinfo->blkbuf = NULL;
			blkbuf_clear_journaled(blkbuf);
			loosen_blkbuf(blkbuf);

			beehive_free(blkbuf_jinfo_allotter, blkbuf_jinfo);
		}
	}
}

/*
 * 解除块缓冲区与日志描述符之间的绑定
 */
void journal_info_detach(struct blkbuf_desc *blkbuf)
{
	journal_info_lock(blkbuf);
	__journal_info_detach(blkbuf);
	journal_info_unlock(blkbuf);
}

/**
 * 递减日志描述符的引用计数
 * 如果为0则解除与块缓冲区之间的绑定
 */
void journal_info_loosen(struct blkbuf_journal_info *blkbuf_jinfo)
{
	struct blkbuf_desc *blkbuf;

	blkbuf = journal_to_blkbuf(blkbuf_jinfo);

	journal_info_lock(blkbuf);

	ASSERT(blkbuf_jinfo->ref_count > 0);
	blkbuf_jinfo->ref_count--;

	if (!blkbuf_jinfo->ref_count && !blkbuf_jinfo->transaction) {
		__journal_info_detach(blkbuf);
		loosen_blkbuf(blkbuf);
	}

	journal_info_unlock(blkbuf);
}

/**
 * 更新磁盘上的超级块
 */
void journal_update_superblock(struct journal *journal, int wait)
{
	struct journal_super_phy *super;
	struct blkbuf_desc *blkbuf;

	super = journal->super_block;
	blkbuf = journal->super_blkbuf;
	/*
	 * 如果下列条件成立，说明没有必要更新超级块
	 *	1、日志数据已经全部写入文件系统，不需要恢复日志数据
	 *	2、日志中没有新的事务
	 */
	if (super->inuse_block_first == 0
	    && journal->oldest_trans_id == journal->next_trans)
		goto out;

	/**
	 * 将日志描述符的字段复制到超级块对象中
	 */
	smp_lock(&journal->state_lock);
	super->trans_id = cpu_to_be32(journal->oldest_trans_id);
	super->inuse_block_first = cpu_to_be32(journal->inuse_block_first);
	super->errno = cpu_to_be32(journal->errno);
	smp_unlock(&journal->state_lock);

	/**
	 * 标记缓冲区为脏
	 * 并同步写脏块或者提交块请求但是不等待
	 */
	blkbuf_mark_dirty(blkbuf);
	if (wait)
		sync_dirty_block(blkbuf);
	else
		submit_block_requests(WRITE, 1, &blkbuf);

out:
	smp_lock(&journal->state_lock);
	if (super->inuse_block_first)
		journal->flags &= ~JSTATE_FLUSHED;
	else
		journal->flags |= JSTATE_FLUSHED;
	smp_unlock(&journal->state_lock);
}

/**
 * 唤醒日志线程，处理事务
 */
int __log_start_commit(struct journal *journal, trans_id_t target)
{
	/**
	 * 想要提交的请求还没有真正被提交
	 */
	if (!tid_geq(journal->committing_id, target)) {
		/**
		 * 标记请求的ID号
		 */
		journal->committing_id = target;
		/**
		 * 唤醒journal_damon日志线程，开始干活了:)
		 */
		wake_up(&journal->wait_commit);

		return 1;
	}

	return 0;
}

int log_start_commit(struct journal *journal, trans_id_t trans_id)
{
	int ret;

	smp_lock(&journal->state_lock);
	ret = __log_start_commit(journal, trans_id);
	smp_unlock(&journal->state_lock);

	return ret;
}

/**
 * 等待某个事务完成
 */
int log_wait_commit(struct journal *journal, trans_id_t trans_id)
{
	int err = 0;

	smp_lock(&journal->state_lock);
	/**
	 * 当前事务还没有被提交
	 */
	while (tid_gt(trans_id, journal->committed_id)) {
		/**
		 * 唤醒日志线程
		 */
		wake_up(&journal->wait_commit);
		smp_unlock(&journal->state_lock);

		/**
		 * 等待日志线程完成后将当前任务唤醒
		 */
		cond_wait(journal->wait_commit_done,
				!tid_gt(trans_id, journal->committed_id));
		smp_lock(&journal->state_lock);
	}
	smp_unlock(&journal->state_lock);

	if (unlikely(journal_is_aborted(journal))) {
		printk(KERN_EMERG "journal commit I/O error\n");
		err = -EIO;
	}

	return err;
}

/**
 * 将日志中的数据刷新到磁盘并清空
 */
int journal_flush(struct journal *journal)
{
	struct transaction *transaction = NULL;
	unsigned long old_tail;
	int err = 0;

	smp_lock(&journal->state_lock);

	/**
	 * 当前有日志在运行
	 */
	if (journal->running_transaction) {
		/**
		 * 强制提交当前日志
		 */
		transaction = journal->running_transaction;
		__log_start_commit(journal, transaction->trans_id);
	} else if (journal->committing_transaction)
		transaction = journal->committing_transaction;

	/**
	 * 等待最后一个日志提交完成
	 */
	if (transaction) {
		trans_id_t trans_id = transaction->trans_id;

		smp_unlock(&journal->state_lock);
		log_wait_commit(journal, trans_id);
	} else
		smp_unlock(&journal->state_lock);
	
	/**
	 * 对所有已经提交并完成的事务
	 * 进行检查点操作，这会将文件系统回写
	 */
	smp_lock(&journal->list_lock);
	while (!err && !list_is_empty(&journal->checkpoint_transactions)) {
		smp_unlock(&journal->list_lock);
		err = journal_checkpoint(journal);
		smp_lock(&journal->list_lock);
	}
	smp_unlock(&journal->list_lock);

	journal_checkpoint_finish(journal);

	smp_lock(&journal->state_lock);
	old_tail = journal->inuse_block_first;
	/**
	 * 标记日志不需要恢复，因为已经全部写入并清空检查点
	 */
	journal->inuse_block_first = 0;
	smp_unlock(&journal->state_lock);
	/**
	 * 回写超级块
	 */
	journal_update_superblock(journal, 1);
	smp_lock(&journal->state_lock);
	/**
	 * 这里可以强制设置为1，恢复以前的值是考虑性能
	 */
	journal->inuse_block_first = old_tail;

	ASSERT(!journal->running_transaction);
	ASSERT(!journal->committing_transaction);
	ASSERT(list_is_empty(&journal->checkpoint_transactions));
	ASSERT(journal->free_block_num == journal->inuse_block_first);
	ASSERT(journal->oldest_trans_id == journal->next_trans);
	smp_unlock(&journal->state_lock);

	return err;
}

/**
 * 确保当前任务没有处于任务中
 * 然后启动一个同步事务强制提交当前任务
 */
int journal_force_commit(struct journal *journal)
{
	struct journal_handle *handle;
	int ret;

	handle = journal_start(journal, 1);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	handle->sync = 1;
	ret = journal_stop(handle);

	return ret;
}

/**
 * 当没有磁盘空间时，上层调用者调用此函数
 * 强制将日志提交到磁盘，以释放磁盘块
 */
int journal_force_commit_nested(struct journal *journal)
{
	struct transaction *transaction = NULL;
	trans_id_t trans_id;

	smp_lock(&journal->state_lock);
	/**
	 * 如果当前有事务在运行中，并且当前进程没有处理事务
	 * 那么就提交当前事务
	 */
	if (journal->running_transaction && !current->journal_info) {
		transaction = journal->running_transaction;
		__log_start_commit(journal, transaction->trans_id);
	} else if (journal->committing_transaction)
		/**
		 * 否则仅仅等待上一个事务
		 */
		transaction = journal->committing_transaction;

	/**
	 * 没有事务可提交，完了:(真的没空间了
	 */
	if (!transaction) {
		smp_unlock(&journal->state_lock);
		return 0;
	}

	trans_id = transaction->trans_id;
	smp_unlock(&journal->state_lock);
	/**
	 * 等待日志提交完成
	 */
	log_wait_commit(journal, trans_id);

	return 1;
}

/**
 * 提交当前事务，但是并不等待其完成
 */
int journal_start_commit(struct journal *journal, trans_id_t *ptrans_id)
{
	int ret = 0;

	smp_lock(&journal->state_lock);
	if (journal->running_transaction) {
		trans_id_t trans_id = journal->running_transaction->trans_id;

		ret = __log_start_commit(journal, trans_id);
		if (ret && ptrans_id)
			*ptrans_id = trans_id;
	} else if (journal->committing_transaction && ptrans_id) {
		/*
		 * 返回当前正在提交的事务ID，上层可以等待此事务完成
		 */
		*ptrans_id = journal->committing_transaction->trans_id;
		ret = 1;
	}
	smp_unlock(&journal->state_lock);

	return ret;
}

/** 
 * 当日志恢复完毕后，清除错误状态
 */
int journal_clear_err(struct journal *journal)
{
	int err = 0;

	smp_lock(&journal->state_lock);
	if (journal->flags & JSTATE_ABORT)
		err = -EROFS;
	else
		journal->errno = 0;
	smp_unlock(&journal->state_lock);

	return err;
}

/**
 * 修改超级块中版本格式，以支持当前版本
 */
int journal_update_format (struct journal *journal)
{
	struct journal_super_phy *super;
	int offset, block_size;
	int err;

	err = read_superblock(journal);
	if (err)
		return err;
	super = journal->super_block;

	switch (be32_to_cpu(super->header.type)) {
	/**
	 * 当前版本
	 */
	case JFS_SUPER_BLOCK_V2:
		return 0;
	/**
	 * balabala，开始升级V1
	 */
	case JFS_SUPER_BLOCK_V1:
		printk(KERN_WARNING "JBD: Converting super_block "
			"from version 1 to 2.\n");

		/**
		 * 将V2版本的字段清空
		 */
		offset = offsetof(struct journal_super_phy, feature_compat);
		block_size = be32_to_cpu(super->block_size);
		memset(&super->feature_compat, 0, block_size - offset);
		super->users = cpu_to_be32(1);
		super->header.type = cpu_to_be32(JFS_SUPER_BLOCK_V2);
		journal->format_version = 2;

		/**
		 * 将超级块置脏并同步写入到磁盘
		 */
		blkbuf_mark_dirty(journal->super_blkbuf);
		sync_dirty_block(journal->super_blkbuf);

		return 0;
	/**
	 * 更高版本，不能动
	 */
	default:
		break;
	}

	return -EINVAL;
}

/**
 * 定期唤醒日志守护线程
 */
static int wakeup_timeout(void *arg)
{
	struct task_desc *task = (struct task_desc *)arg;

	wake_up_process(task);

	return 0;
}

static int journal_demon(void *arg)
{
	struct journal *journal = (struct journal *) arg;
	struct transaction *transaction;
	struct timer timer;
	DEFINE_WAIT(wait);
	int should_sleep;

	/**
	 * 当有日志需要回写时，启动此定时器
	 * 五秒内唤醒本线程回写
	 */
	timer_init(&timer);
	timer.data = current;
	timer.handle = wakeup_timeout;
	journal->commit_timer = &timer;

	/**
	 * 守护线程初始化完毕
	 * 唤醒文件系统装载过程。继续其他装载工作
	 */
	journal->demon_task = current;
	wake_up(&journal->wait_commit_done);

	printk(KERN_INFO "journal demon starting.  Commit interval "
		"%ld seconds\n", journal->commit_interval / HZ);

	smp_lock(&journal->state_lock);

	while (1) {
		/**
		 * 要退出日志处理了，结束本线程
		 */
		if (journal->flags & JSTATE_UNMOUNT)
			goto unmount;

		journal_debug(1, "commit_sequence=%d, commit_request=%d\n",
			journal->committed_id, journal->committing_id);

		/**
		 * 希望提交的事务编号，与已经提交的编号不相等
		 * 说明有提交事务的需求
		 */
		if (journal->committed_id != journal->committing_id) {
			smp_unlock(&journal->state_lock);
			synchronize_timer_del(journal->commit_timer);
			/**
			 * 提交事务
			 */
			journal_commit_transaction(journal);
			smp_lock(&journal->state_lock);
			continue;
		}

		/**
		 * 文件系统可能正在等待日志提交
		 * 唤醒等待线程
		 */
		wake_up(&journal->wait_commit_done);

		prepare_to_wait(&journal->wait_commit, &wait, TASK_INTERRUPTIBLE);
		transaction = journal->running_transaction;
		should_sleep = !transaction ||
			time_before((unsigned long)jiffies, transaction->timeout);
		if (should_sleep) {
			smp_unlock(&journal->state_lock);
			schedule();
			smp_lock(&journal->state_lock);
		}
		finish_wait(&journal->wait_commit, &wait);

		/**
		 * 当时事务超时了，强制提交
		 */
		transaction = journal->running_transaction;
		if (transaction &&
		    time_after_eq((unsigned long)jiffies, transaction->timeout))
			/**
		    	 * 将当前运行事务作为待提交事务
		    	 */
			journal->committing_id = transaction->trans_id;
	}

unmount:
	smp_unlock(&journal->state_lock);
	synchronize_timer_del(journal->commit_timer);
	journal->demon_task = NULL;
	wake_up(&journal->wait_commit_done);
	journal_debug(1, "Journal thread exiting.\n");

	return 0;
}

/**
 * 读取日志超级块
 */
static int read_superblock(struct journal *journal)
{
	struct journal_super_phy *super;
	struct blkbuf_desc *blkbuf;
	int err;

	err = -EIO;
	blkbuf = journal->super_blkbuf;

	ASSERT(blkbuf != NULL);
	/**
	 * 数据还没有从磁盘上读取过
	 */
	if (!blkbuf_is_uptodate(blkbuf)) {
		/**
		 * 提交读请求并等待读完成
		 */
		submit_block_requests(READ, 1, &blkbuf);
		blkbuf_wait_unlock(blkbuf);
		/**
		 * 读失败，还是退了吧
		 */
		if (!blkbuf_is_uptodate(blkbuf)) {
			printk (KERN_ERR "JBD: IO error reading journal super block\n");
			goto fail;
		}
	}

	/**
	 * 从磁盘缓冲区中开始读数据了
	 */
	super = journal->super_block;
	err = -EINVAL;
	if (super->header.magic != cpu_to_be32(JFS_MAGIC_NUMBER) ||
	    super->block_size != cpu_to_be32(journal->block_size)) {
		printk(KERN_WARNING "JBD: no valid journal super block found\n");
		goto fail;
	}

	switch(be32_to_cpu(super->header.type)) {
	case JFS_SUPER_BLOCK_V1:
		journal->format_version = 1;
		break;
	case JFS_SUPER_BLOCK_V2:
		journal->format_version = 2;
		break;
	default:
		printk(KERN_WARNING "JBD: unrecognised super block format ID\n");
		goto fail;
	}

	if (be32_to_cpu(super->block_end) < journal->block_end)
		journal->block_end = be32_to_cpu(super->block_end);
	else if (be32_to_cpu(super->block_end) > journal->block_end) {
		printk (KERN_WARNING "JBD: journal file too short\n");
		goto fail;
	}

	return 0;

fail:
	loosen_blkbuf(blkbuf);
	journal->super_blkbuf = NULL;

	return err;
}

/**
 * 从磁盘中加载日志超级块内容
 */
static int load_superblock(struct journal *journal)
{
	struct journal_super_phy *super;
	int err;

	/**
	 * 从磁盘中读取超级块的内容
	 */
	err = read_superblock(journal);
	if (err)
		return err;

	super = journal->super_block;

	journal->oldest_trans_id = be32_to_cpu(super->trans_id);
	journal->inuse_block_first = be32_to_cpu(super->inuse_block_first);
	journal->first_block_log = be32_to_cpu(super->first_block_log);
	journal->last_block_log = be32_to_cpu(super->block_end);
	journal->errno = be32_to_cpu(super->errno);

	return 0;
}

/**
 * write = 0
 *	仅仅略过恢复过程
 */
int journal_wipe(struct journal *journal, int write)
{
	int err = 0;

	ASSERT (!(journal->flags & JSTATE_LOADED));

	err = load_superblock(journal);
	if (err)
		return err;

	/**
	 * 为0表示全部checkpoint了，不需要恢复
	 */
	if (!journal->inuse_block_first)
		return err;

	printk (KERN_WARNING "JBD: %s recovery information on journal\n",
		write ? "Clearing" : "Ignoring");

	/**
	 * 扫描日志，但是并不执行恢复操作
	 */
	err = journal_recovery_ignore(journal);
	/**
	 * 回写日志超级块，使日志失效
	 */
	if (write)
		journal_update_superblock(journal, 1);

 	return err;
}

static void journal_start_demon(struct journal *journal)
{
	kthread_create(journal_demon, journal, 30, "journal");
	cond_wait(journal->wait_commit_done, journal->demon_task != 0);
}

/**
 * 当日志恢复完成后
 * 重置内存及磁盘，开始新的日志
 */
static int journal_reset(struct journal *journal)
{
	struct journal_super_phy *super;
	unsigned int first, last;

	super = journal->super_block;
	first = be32_to_cpu(super->first_block_log);
	last = be32_to_cpu(super->block_end);

	journal->first_block_log = first;
	journal->last_block_log = last;
	journal->free_block_num = first;
	journal->inuse_block_first = first;
	journal->free_blocks = last - first;

	journal->oldest_trans_id = journal->next_trans;
	journal->committed_id = journal->next_trans - 1;
	journal->committing_id = journal->committed_id;
	journal->max_block_in_trans = journal->block_end / 4;

	/**
	 * 更新超级块并写入磁盘
	 */
	journal_update_superblock(journal, 1);
	/**
	 * 启动并启动守护线程
	 */
	journal_start_demon(journal);

	return 0;
}

/**
 * 读取日志，并且执行恢复过程
 */
int journal_engorge(struct journal *journal)
{
	struct journal_super_phy *super;
	int err;

	err = load_superblock(journal);
	if (err)
		return err;

	/**
	 * 如果有高版本不兼容的标志就退出
	 * 以免破坏日志和文件系统
	 */
	if (journal->format_version >= 2) {
		unsigned long unknown;

		super = journal->super_block;
		unknown = super->feature_ro_compat &
			~cpu_to_be32(JFS_KNOWN_ROCOMPAT_FEATURES);
		unknown |= super->feature_incompat &
		     ~cpu_to_be32(JFS_KNOWN_INCOMPAT_FEATURES);
		if (unknown) {
			printk (KERN_WARNING "JBD: Unrecognised features on journal\n");
			return -EINVAL;
		}
	}

	/**
	 * 真正的恢复日志
	 */
	if (journal_recover_engorge(journal))
		goto fail;

	/**
	 * 恢复完成，重置内存和磁盘中的数据
	 */
	if (journal_reset(journal))
		goto fail;

	journal->flags &= ~JSTATE_ABORT;
	journal->flags |= JSTATE_LOADED;

	return 0;

fail:
	printk (KERN_WARNING "JBD: recovery failed\n");
	return -EIO;
}

/**
 * 分配日志描述符，做一点最简单的初始化
 */
static struct journal *alloc_journal (void)
{
	struct journal *journal;
	int err;

	journal = kmalloc(sizeof(*journal), PAF_KERNEL | __PAF_ZERO);
	if (!journal)
		return NULL;

	init_waitqueue(&journal->wait_new_trans);
	init_waitqueue(&journal->wait_logspace);
	init_waitqueue(&journal->wait_commit_done);
	init_waitqueue(&journal->wait_checkpoint);
	init_waitqueue(&journal->wait_commit);
	init_waitqueue(&journal->wait_updates);
	sema_init(&journal->barrier_sem, 1);
	sema_init(&journal->checkpoint_sem, 1);
	list_init(&journal->checkpoint_transactions);
	smp_lock_init(&journal->revoke_lock);
	smp_lock_init(&journal->list_lock);
	smp_lock_init(&journal->state_lock);

	journal->commit_interval = (HZ * JBD_DEFAULT_MAX_COMMIT_AGE);
	/**
	 * 恢复成功后清楚此标记
	 */
	journal->flags = JSTATE_ABORT;

	/**
	 * 初始化撤销表
	 */
	err = journal_init_revoke(journal, JOURNAL_REVOKE_BUCKETS);
	if (err) {
		kfree(journal);

		return NULL;
	}

	return journal;
}

/**
 * 分配一个日志结构
 * 该日志保存在单独的块设备中，而不是保存在文件节点中
 */
struct journal *
journal_alloc_dev(struct block_device *blkdev, struct block_device *fs_blkdev,
	int block_start, int block_end, int block_size)
{
	struct blkbuf_desc *blkbuf;
	struct journal *journal;
	int tag_count;

	journal = alloc_journal();
	if (!journal)
		return NULL;

	journal->block_start = block_start;
	journal->block_end = block_end;
	journal->block_size = block_size;
	tag_count = journal->block_size / sizeof(struct journal_block_tag);
	journal->tags_in_block = tag_count;
	journal->blkbuf_bulk = kmalloc(tag_count * sizeof(struct blkbuf_desc*),
							PAF_KERNEL);
	if (!journal->blkbuf_bulk) {
		printk(KERN_ERR "%s: Cant allocate memory for commit thread\n",
			__FUNCTION__);
		kfree(journal);

		return NULL;
	}

	journal->blkdev = blkdev;
	journal->fs_blkdev = fs_blkdev;

	blkbuf = __blkbuf_find_alloc(journal->blkdev,
		block_start, journal->block_size);
	journal->super_blkbuf = blkbuf;
	journal->super_block = (struct journal_super_phy *)blkbuf->block_data;

	return journal;
}

void journal_destroy(struct journal *journal)
{
	/* TO-DO */
}

int __init init_journal(void)
{
	int ret;

	ret = init_journal_revoke();
	if (ret)
		return ret;

	blkbuf_jinfo_allotter = beehive_create("blkbuf_journal_info",
		sizeof(struct blkbuf_journal_info), 0, 0, NULL);
	if (blkbuf_jinfo_allotter == 0) {
		printk(KERN_EMERG "JBD: no memory for blkbuf_journal_info cache\n");
		return -ENOMEM;
	}

	journal_handle_allotter = beehive_create("journal_handle",
		sizeof(struct journal_handle), 0, 0, NULL);
	if (journal_handle_allotter == NULL) {
		printk(KERN_EMERG "JBD: failed to create handle cache\n");
		return -ENOMEM;
	}

	return ret;
}
