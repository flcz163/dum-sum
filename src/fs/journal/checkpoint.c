#include <dim-sum/errno.h>
#include <dim-sum/journal.h>

#include "internal.h"

#define NR_BATCH	64

static void sync_blkbuf_state(struct journal *journal, struct blkbuf_desc *blkbuf)
{
	hold_blkbuf(blkbuf);
	smp_unlock(&journal->list_lock);
	blkbuf_lock_state(blkbuf);
	blkbuf_unlock_state(blkbuf);
	loosen_blkbuf(blkbuf);
}

/**
 * 当缓冲区被写入磁盘，或者需要从一个事务的检查点链表移动到另外一个事务
 * 则调用此函数从当前事务的检查点链表中摘除
 */
void __journal_takeout_checkpoint(struct blkbuf_journal_info *blkbuf_jinfo)
{
	struct transaction *trans;
	struct journal *journal;

	/**
	 * 如果缓冲区不在事务的检查点链表中则退出
	 */
	trans = blkbuf_jinfo->checkpoint_trans;
	if (trans == NULL)
		return;

	/**
	 * 从检查点链表中摘除
	 */
	journal = trans->journal;
	blkbuf_jinfo->checkpoint_trans = NULL;
	list_del_init(&blkbuf_jinfo->list_checkpoint);

	/**
	 * 事务的检查点链表还不空，直接退出
	 */
	if (!list_is_empty(&trans->checkpoint_list))
		return;

	/**
	 * 对于当前提交任务来说，应当由提交过程来决定是否释放事务
	 * 还在引用中
	 */
	if (trans == journal->committing_transaction)
		return;

	/**
	 * 彻底释放事务
	 */
	__journal_drop_transaction(journal, trans);

	wake_up(&journal->wait_logspace);
}

/**
 * 从事务中释放一个检查点缓冲区
 */
static int __try_free_blkbuf(struct blkbuf_journal_info *blkbuf_jinfo)
{
	struct blkbuf_desc *blkbuf;
	int ret = 0;

	blkbuf = journal_to_blkbuf(blkbuf_jinfo);
	if (blkbuf_jinfo->which_list == TRANS_QUEUE_NONE
	    && !blkbuf_is_locked(blkbuf) && !blkbuf_is_dirty(blkbuf)) {
		/**
		 * 从事务的检查点链表中摘除缓冲区
		 */
		__journal_takeout_checkpoint(blkbuf_jinfo);
		blkbuf_unlock_state(blkbuf);
		journal_info_detach(blkbuf);
		loosen_blkbuf(blkbuf);
		ret = 1;
	} else
		blkbuf_unlock_state(blkbuf);

	return ret;
}

/**
 * 执行完检查点以后，彻底丢弃事务
 */
void __journal_drop_transaction(struct journal *journal, struct transaction *trans)
{
	assert_smp_lock_is_locked(&journal->list_lock);
	list_del_init(&trans->list_checkpoint);

	ASSERT(trans->state == TRANS_FINISHED);
	ASSERT(list_is_empty(&trans->metadata_list));
	ASSERT(list_is_empty(&trans->data_block_list));
	ASSERT(list_is_empty(&trans->forget_list));
	ASSERT(list_is_empty(&trans->meta_log_list));
	ASSERT(list_is_empty(&trans->meta_orig_list));
	ASSERT(list_is_empty(&trans->ctrldata_list));
	ASSERT(list_is_empty(&trans->checkpoint_list));
	ASSERT(trans->users == 0);
	ASSERT(journal->committing_transaction != trans);
	ASSERT(journal->running_transaction != trans);

	kfree(trans);
}

/**
 * 对于已经提交到日志的缓冲区块
 * 将其加入到事务的检查点链表中
 */
void __journal_insert_checkpoint(struct blkbuf_journal_info *blkbuf_jinfo,
	struct transaction *trans)
{
	struct blkbuf_desc *blkbuf;

	blkbuf = journal_to_blkbuf(blkbuf_jinfo);
	ASSERT(blkbuf_is_dirty(blkbuf) || blkbuf_is_journal_dirty(blkbuf));
	ASSERT(blkbuf_jinfo->checkpoint_trans == NULL);

	blkbuf_jinfo->checkpoint_trans = trans;
	list_insert_front(&blkbuf_jinfo->list_checkpoint, &trans->checkpoint_list);
}

/**
 * 查找所有已经回写到文件系统中的块
 * 并释放它们
 * 在开始提交日志前调用，此快速释放一部分内存
 */
int __journal_clean_checkpoint_list(struct journal *journal)
{
	struct transaction *trans, *last_trans, *next_trans;
	int ret = 0;

	if (list_is_empty(&journal->checkpoint_transactions))
		goto out;

	trans = list_first_container(&journal->checkpoint_transactions,
		struct transaction, list_checkpoint);
	last_trans = list_last_container(&journal->checkpoint_transactions,
		struct transaction, list_checkpoint);
	next_trans = trans;
	/**
	 * 遍历日志的检查点事务链表
	 */
	do {
		trans = next_trans;
		next_trans = list_next_entry(trans, list_checkpoint);
		/**
		 * 事务存在检查点
		 */
		if (!list_is_empty(&trans->checkpoint_list)) {
			struct blkbuf_journal_info *blkbuf_jinfo;
			struct blkbuf_journal_info *last_jinfo;
			struct blkbuf_journal_info *next_jinfo;

			blkbuf_jinfo = list_first_container(&trans->checkpoint_list,
				struct blkbuf_journal_info, list_checkpoint);
			next_jinfo = blkbuf_jinfo;
			last_jinfo = list_last_container(&trans->checkpoint_list,
				struct blkbuf_journal_info, list_checkpoint);
			/**
			 * 遍历检查点链表，试图释放其中每一个缓冲区
			 */
			do {
				blkbuf_jinfo = next_jinfo;
				next_jinfo = list_next_entry(blkbuf_jinfo, list_checkpoint);
				if (blkbuf_trylock_state(journal_to_blkbuf(blkbuf_jinfo)))
					ret += __try_free_blkbuf(blkbuf_jinfo);
			} while (blkbuf_jinfo != last_jinfo);
		}
	} while (trans != last_trans);

out:
	return ret;
}

/**
 * 完成检查点，因此可以更新磁盘超级块中的值
 * 如空闲块数、起始块、最老事务编号
 */
int journal_checkpoint_finish(struct journal *journal)
{
	unsigned long	block_num, freed; 
	trans_id_t first_trans_id;
	struct transaction *trans;

	smp_lock(&journal->state_lock);
	smp_lock(&journal->list_lock);
	trans = NULL;
	/**
	 * 有检查点，则找到链表头事务
	 */
	if (!list_is_empty(&journal->checkpoint_transactions)) {
		trans = list_first_container(&journal->checkpoint_transactions,
			struct transaction, list_checkpoint);
		first_trans_id = trans->trans_id;
		block_num = trans->start_block_num;
	} else if ((trans = journal->committing_transaction) != NULL) {
		first_trans_id = trans->trans_id;
		block_num = trans->start_block_num;
	} else if ((trans = journal->running_transaction) != NULL) {
		first_trans_id = trans->trans_id;
		block_num = journal->free_block_num;
	} else {
		first_trans_id = journal->next_trans;
		block_num = journal->free_block_num;
	}
	smp_unlock(&journal->list_lock);

	/**
	 * block_num == 0表示完全 干净的日志
	 */
	ASSERT(block_num != 0);

	/**
	 * 不需要移动日志尾部指针
	 */
	if (journal->oldest_trans_id == first_trans_id) {
		smp_unlock(&journal->state_lock);
		return 1;
	}

	/**
	 * 修正日志可用空间，注意处理日志空间回绕
	 */
	freed = block_num - journal->inuse_block_first;
	if (block_num < journal->inuse_block_first)
		freed = freed + journal->last_block_log - journal->first_block_log;

	journal->free_blocks += freed;
	journal->oldest_trans_id = first_trans_id;
	journal->inuse_block_first = block_num;
	smp_unlock(&journal->state_lock);
	/**
	 * 将最新的值更新到超级块，并写入到磁盘
	 */
	if (!(journal->flags & JSTATE_ABORT))
		journal_update_superblock(journal, 1);

	return 0;
}

/**
 * 成批提交
 */
static void
__submit_bulk(struct journal *journal, struct blkbuf_desc **blkbufs, int *batch_count)
{
	int i;

	smp_unlock(&journal->list_lock);
	submit_block_requests(WRITE, *batch_count, blkbufs);
	smp_lock(&journal->list_lock);
	for (i = 0; i < *batch_count; i++) {
		struct blkbuf_desc *blkbuf;

		blkbuf = blkbufs[i];
		blkbuf_clear_write_journal(blkbuf);
		loosen_blkbuf(blkbuf);
	}

	*batch_count = 0;
}

/**
 * 准备提交一个磁盘块
 */
static int __prepare_submit_one_block(struct journal *journal,
	struct blkbuf_journal_info *blkbuf_jinfo,
	struct blkbuf_desc **blkbufs, int *batch_count,
	int *drop_count)
{
	struct blkbuf_desc *blkbuf;
	int ret = 0;

	blkbuf = journal_to_blkbuf(blkbuf_jinfo);
	if (blkbuf_is_dirty(blkbuf)
	    && !blkbuf_is_locked(blkbuf)
	    && blkbuf_jinfo->which_list == TRANS_QUEUE_NONE) {
		ASSERT(blkbuf_jinfo->transaction == NULL);

		hold_blkbuf(blkbuf);
		blkbuf_set_write_journal(blkbuf);
		blkbufs[*batch_count] = blkbuf;
		(*batch_count)++;
		blkbuf_unlock_state(blkbuf);
		/**
		 * 临时缓冲区满，上层需要继续循环
		 */
		if (*batch_count == NR_BATCH) {
			__submit_bulk(journal, blkbufs, batch_count);
			ret = 1;
		}
	} else {
		int last = 0;

		/**
		 * 如果事务中最后一个缓冲区被移除了，那么上层要转到下一个事务
		 */
		if (blkbuf_jinfo->list_checkpoint.next == 
		    &blkbuf_jinfo->transaction->checkpoint_list)
			last = 1;

		if (__try_free_blkbuf(blkbuf_jinfo)) {
			(*drop_count)++;
			ret = last;
		}
	}

	return ret;
}

/**
 * 等待事务内的检查点缓冲区完成
 * 然后做一些清理工作
 */
static int
__wait_checkpoint(struct journal *journal, struct transaction *trans)
{
	struct blkbuf_journal_info *blkbuf_jinfo, *next_jinfo, *last_jinfo;
	struct blkbuf_desc *blkbuf;
	int ret = 0;

	assert_smp_lock_is_locked(&journal->list_lock);
	if (list_is_empty(&trans->checkpoint_list))
		return 0;

	blkbuf_jinfo = list_first_container(&trans->checkpoint_list,
		struct blkbuf_journal_info, list_checkpoint);
	last_jinfo = list_last_container(&trans->checkpoint_list,
		struct blkbuf_journal_info, list_checkpoint);
	next_jinfo = blkbuf_jinfo;
	do {
		blkbuf_jinfo = next_jinfo;
		blkbuf = journal_to_blkbuf(blkbuf_jinfo);
		if (blkbuf_is_locked(blkbuf)) {
			hold_blkbuf(blkbuf);
			smp_unlock(&journal->list_lock);
			blkbuf_wait_unlock(blkbuf);
			loosen_blkbuf(blkbuf);
			goto out;
		}

		/*
		 * 这个可能获取不到锁吗??
		 */
		if (!blkbuf_trylock_state(blkbuf)) {
			sync_blkbuf_state(journal, blkbuf);
			goto out;
		}

		/**
		 * 如果缓冲区被其他事务使用了
		 * 那么等待该事务完成
		 */
		if (blkbuf_jinfo->transaction != NULL) {
			trans_id_t trans_id;

			trans_id = blkbuf_jinfo->transaction->trans_id;
			smp_unlock(&journal->list_lock);
			blkbuf_unlock_state(blkbuf);
			log_start_commit(journal, trans_id);
			log_wait_commit(journal, trans_id);
			goto out;
		}

		next_jinfo = list_next_entry(blkbuf_jinfo, list_checkpoint);
		/**
		 * 成功写入，将缓冲区从链表中取出
		 */
		if (!blkbuf_is_dirty(blkbuf) && !blkbuf_is_journal_dirty(blkbuf)) {
			__journal_takeout_checkpoint(blkbuf_jinfo);
			blkbuf_unlock_state(blkbuf);
			journal_info_detach(blkbuf);
			loosen_blkbuf(blkbuf);
			ret = 1;
		} else
			blkbuf_unlock_state(blkbuf);

		blkbuf_jinfo = next_jinfo;
	} while (blkbuf_jinfo != last_jinfo);

	return ret;

out:
	smp_lock(&journal->list_lock);
	return 1;
}

/**
 * 执行检查点操作
 * 一次处理一个事务，如果空间仍然不足，上层会继续调用本函数
 */
int journal_checkpoint(struct journal *journal)
{
	struct blkbuf_desc *blkbufs[NR_BATCH];
	int batch_count = 0;
	int ret;

	/**
	 * 清理不需要检查点的事务
	 */
	ret = journal_checkpoint_finish(journal);
	if (ret <= 0)
		return ret;

	smp_lock(&journal->list_lock);
	while (!list_is_empty(&journal->checkpoint_transactions)) {
		struct blkbuf_journal_info *last_jinfo, *next_jinfo;
		struct blkbuf_journal_info *blkbuf_jinfo;
		int cleanup_ret, retry = 0;
		struct transaction *trans;
		int drop_count = 0;
		trans_id_t this_tid;

		trans = list_first_container(&journal->checkpoint_transactions,
			struct transaction, list_checkpoint);
		this_tid = trans->trans_id;
		blkbuf_jinfo = list_first_container(&trans->checkpoint_list,
			struct blkbuf_journal_info, list_checkpoint);
		last_jinfo = list_last_container(&trans->checkpoint_list,
			struct blkbuf_journal_info, list_checkpoint);
		next_jinfo = blkbuf_jinfo;
		/**
		 * 遍历事务的检查点链表
		 * 直到处理完所有缓冲区或者临时缓冲区数组满
		 */
		do {
			struct blkbuf_desc *blkbuf;

			blkbuf_jinfo = next_jinfo;
			next_jinfo = list_next_entry(blkbuf_jinfo, list_checkpoint);
			blkbuf = journal_to_blkbuf(blkbuf_jinfo);
			/**
			 * 如果缓冲区状态锁冲突，则退出重试
			 */
			if (!blkbuf_trylock_state(blkbuf)) {
				sync_blkbuf_state(journal, blkbuf);
				smp_lock(&journal->list_lock);
				retry = 1;
				break;
			}

			retry = __prepare_submit_one_block(journal, blkbuf_jinfo, blkbufs, &batch_count, &drop_count);
		} while (blkbuf_jinfo != last_jinfo && !retry);

		if (batch_count)
			__submit_bulk(journal, blkbufs, &batch_count);

		/**
		 * 如果在回写过程中，检查点链表发生变化
		 * 则需要重新判断一下
		 */
		if (list_is_empty(&journal->checkpoint_transactions))
			break;
		if (list_first_container(&journal->checkpoint_transactions,
		    struct transaction, list_checkpoint) != trans)
			break;

		if (retry)
			continue;
		if (trans->trans_id != this_tid)
			continue;

		cleanup_ret = __wait_checkpoint(journal, trans);
		ASSERT(drop_count != 0 || cleanup_ret != 0);
		/**
		 * 处理完一个事务的检查点，退出
		 * 否则继续处理这一个事务
		 */
		if (list_first_container(&journal->checkpoint_transactions,
		    struct transaction, list_checkpoint) != trans)
			break;
	}

	smp_unlock(&journal->list_lock);

	ret = journal_checkpoint_finish(journal);
	if (ret < 0)
		return ret;

	return 0;
}

/**
 * 新建事务时，如果发现日志空间不足
 * 则调用此函数，等待空闲的日志空间
 */
void __journal_wait_space(struct journal *journal)
{
	int block_count;

	assert_smp_lock_is_locked(&journal->state_lock);
	/**
	 * 计算顺利完成日志所需要的空间
	 * 包含一个完整的新事务以及正在提交事务的空间
	 */
	block_count = journal_needed_space(journal);
	while (__log_free_space(journal) < block_count) {
		if (journal->flags & JSTATE_ABORT)
			return;

		smp_unlock(&journal->state_lock);
		down(&journal->checkpoint_sem);
		/**
		 * 在等待信号量期间，日志空间可能发生了变化
		 * 重新判断一下
		 */
		smp_lock(&journal->state_lock);
		block_count = journal_needed_space(journal);
		if (__log_free_space(journal) < block_count) {
			smp_unlock(&journal->state_lock);
			/**
			 * 执行检查点操作，以释放空间
			 */
			journal_checkpoint(journal);
			smp_lock(&journal->state_lock);
		}
		up(&journal->checkpoint_sem);
	}
}
