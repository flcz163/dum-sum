#include <dim-sum/beehive.h>
#include <dim-sum/delay.h>
#include <dim-sum/err.h>
#include <dim-sum/errno.h>
#include <dim-sum/highmem.h>
#include <dim-sum/journal.h>

#include "internal.h"

static int
stick_handle(struct journal *journal, struct journal_handle *handle);

/**
 * 分配日志句柄
 */
static struct journal_handle *alloc_handle(int block_count)
{
	struct journal_handle *handle;

	handle = beehive_alloc(journal_handle_allotter, PAF_NOFS | __PAF_ZERO);
	if (!handle)
		return NULL;

	handle->block_credits = block_count;
	handle->ref_count = 1;

	return handle;
}

/**
 * 将块缓冲区放到不同的链表中
 */
void __journal_putin_list(struct blkbuf_journal_info *blkbuf_jinfo,
	struct transaction *trans, int which_list)
{
	struct double_list *list = NULL;
	struct blkbuf_desc *blkbuf;
	int was_dirty = 0;

	blkbuf = journal_to_blkbuf(blkbuf_jinfo);
	/**
	 * 已经在相应的链表中
	 */
	if (blkbuf_jinfo->transaction && blkbuf_jinfo->which_list == which_list)
		return;

	/**
	 * 在移动链表时，暂时清除脏标志
	 */
	if (which_list == TRANS_QUEUE_METADATA || which_list == TRANS_QUEUE_RESERVED ||
	    which_list == TRANS_QUEUE_META_ORIG || which_list == TRANS_QUEUE_FORGET) {
		if (blkbuf_test_clear_dirty(blkbuf) ||
		    blkbuf_test_clear_journal_dirty(blkbuf))
			was_dirty = 1;
	}

	/**
	 * 从原链表中移除
	 */
	if (blkbuf_jinfo->transaction)
		__journal_takeout_list(blkbuf_jinfo);
	blkbuf_jinfo->transaction = trans;

	switch (which_list) {
	case TRANS_QUEUE_NONE:
		ASSERT(!blkbuf_jinfo->undo_copy);
		ASSERT(!blkbuf_jinfo->bufdata_copy);
		return;
	case TRANS_QUEUE_DIRTY_DATA:
		list = &trans->data_block_list;
		break;
	case TRANS_QUEUE_METADATA:
		trans->metadata_blocks++;
		list = &trans->metadata_list;
		break;
	case TRANS_QUEUE_FORGET:
		list = &trans->forget_list;
		break;
	case TRANS_QUEUE_META_LOG:
		list = &trans->meta_log_list;
		break;
	case TRANS_QUEUE_META_ORIG:
		list = &trans->meta_orig_list;
		break;
	case TRANS_QUEUE_CTRLDATA:
		list = &trans->ctrldata_list;
		break;
	case TRANS_QUEUE_RESERVED:
		list = &trans->reserved_list;
		break;
	case TRANS_QUEUE_LOCKED_DATA:
		list =  &trans->locked_data_list;
		break;
	}

	blkbuf_jinfo->which_list = which_list;
	if (!list)
		list_init(&blkbuf_jinfo->list_trans);
	else
		list_insert_behind(&blkbuf_jinfo->list_trans, list);

	if (was_dirty)
		blkbuf_set_journal_dirty(blkbuf);
}

/**
 * 将日志缓冲区加入到某个链表中。
 */
void journal_putin_list(struct blkbuf_journal_info *blkbuf_jinfo,
	struct transaction *trans, int which_list)
{
	blkbuf_lock_state(journal_to_blkbuf(blkbuf_jinfo));
	smp_lock(&trans->journal->list_lock);
	__journal_putin_list(blkbuf_jinfo, trans, which_list);
	smp_unlock(&trans->journal->list_lock);
	blkbuf_unlock_state(journal_to_blkbuf(blkbuf_jinfo));
}

/**
 * 从事务的链表中移除块缓冲区
 */
void __journal_takeout_list(struct blkbuf_journal_info *blkbuf_jinfo)
{
	struct transaction *trans;
	struct blkbuf_desc *blkbuf;

	trans = blkbuf_jinfo->transaction;
	blkbuf = journal_to_blkbuf(blkbuf_jinfo);

	if (trans)
		assert_smp_lock_is_locked(&trans->journal->list_lock);

	if (blkbuf_jinfo->which_list == TRANS_QUEUE_NONE)
		goto out;

	if (blkbuf_jinfo->which_list == TRANS_QUEUE_METADATA)
		trans->metadata_blocks--;

	list_del_init(&blkbuf_jinfo->list_trans);
	blkbuf_jinfo->which_list = TRANS_QUEUE_NONE;

	if (blkbuf_test_clear_journal_dirty(blkbuf))
		blkbuf_mark_dirty(blkbuf);

out:
	blkbuf_jinfo->transaction = NULL;
}

/**
 * 将缓冲区从当前的链表中摘除。
 */
void journal_takeout_list(struct journal *journal,
	struct blkbuf_journal_info *blkbuf_jinfo)
{
	blkbuf_lock_state(journal_to_blkbuf(blkbuf_jinfo));
	smp_lock(&journal->list_lock);
	__journal_takeout_list(blkbuf_jinfo);
	smp_unlock(&journal->list_lock);
	blkbuf_unlock_state(journal_to_blkbuf(blkbuf_jinfo));
}

/**
 * 当前事务已经完成对缓冲区的处理
 * 将其移动到下一个事务的链表中
 */
void __journal_putin_next_list(struct blkbuf_journal_info *blkbuf_jinfo)
{
	struct smp_lock *list_lock;
	struct blkbuf_desc *blkbuf;
	int was_dirty;

	blkbuf = journal_to_blkbuf(blkbuf_jinfo);
	list_lock = &blkbuf_jinfo->transaction->journal->list_lock;
	ASSERT(blkbuf_is_locked_state(blkbuf));
	if (blkbuf_jinfo->transaction)
		assert_smp_lock_is_locked(list_lock);

	/**
	 * 当前事务不关注此缓冲区
	 * 直接从原事务的缓冲区中摘除并退出
	 */
	if (blkbuf_jinfo->next_trans == NULL) {
		__journal_takeout_list(blkbuf_jinfo);
		return;
	}

	/**
	 * 添加到新事务的链表中
	 */
	was_dirty = blkbuf_test_clear_journal_dirty(blkbuf);
	__journal_takeout_list(blkbuf_jinfo);
	blkbuf_jinfo->transaction = blkbuf_jinfo->next_trans;
	blkbuf_jinfo->next_trans = NULL;
	__journal_putin_list(blkbuf_jinfo, blkbuf_jinfo->transaction, TRANS_QUEUE_METADATA);
	ASSERT(blkbuf_jinfo->transaction->state == TRANS_RUNNING);

	if (was_dirty)
		blkbuf_set_journal_dirty(blkbuf);
}

/**
 * 将缓冲区的控制权移交给下一个事务接着处理
 */
void journal_putin_next_list(struct journal *journal, struct blkbuf_journal_info *blkbuf_jinfo)
{
	struct blkbuf_desc *blkbuf;

	blkbuf = journal_to_blkbuf(blkbuf_jinfo);

	blkbuf_lock_state(blkbuf);
	smp_lock(&journal->list_lock);
	__journal_putin_next_list(blkbuf_jinfo);
	blkbuf_unlock_state(blkbuf);
	/**
	 * 如果没有下一个事务，真的就需要释放相关的日志资源
	 */
	journal_info_detach(blkbuf);
	smp_unlock(&journal->list_lock);

	/**
	 * 当前事务结束对块缓冲区的引用
	 */
	loosen_blkbuf(blkbuf);
}

/**
 * 建立一个日志屏障
 * 阻止更多的写操作，直到已经存在的所有更新操作已经完成
 */
void journal_lock_updates(struct journal *journal)
{
	DEFINE_WAIT(wait);

	smp_lock(&journal->state_lock);
	journal->barrier_count++;

	/**
	 * 等待所有已有屏障完成
	 */
	while (1) {
		struct transaction *trans;

		trans = journal->running_transaction;
		if (!trans)
			break;

		smp_lock(&trans->lock);

		/**
		 * 使用此事务的线程已经全部结束
		 */
		if (!trans->users) {
			smp_unlock(&trans->lock);
			break;
		}

		prepare_to_wait(&journal->wait_updates, &wait,
				TASK_UNINTERRUPTIBLE);
		smp_unlock(&trans->lock);
		smp_unlock(&journal->state_lock);
		schedule();
		finish_wait(&journal->wait_updates, &wait);
		smp_lock(&journal->state_lock);
	}
	smp_unlock(&journal->state_lock);

	/**
	 * 防止多个线程并发的创建事务屏障
	 */
	down(&journal->barrier_sem);
}

/**
 * 释放事务屏障
 */
void journal_unlock_updates(struct journal *journal)
{
	ASSERT(journal->barrier_count);

	up(&journal->barrier_sem);
	smp_lock(&journal->state_lock);
	journal->barrier_count--;
	smp_unlock(&journal->state_lock);
	wake_up(&journal->wait_new_trans);
}

/**
 * 当分配了日志额度，但是后来又不需要了
 * 将其归还给日志系统
 * 这里不能将块缓冲区从保留链表中摘除
 * 相反，应当将它暂时保留在链表中，并且如果真的没有谁使用它
 * 那么在提交此事务时会释放此缓冲区
 */
void journal_putback_credits(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf, int credits)
{
	handle->block_credits += credits;
}

/**
 * 在执行大的文件操作时，已经申请的日志空间额度可能不够
 * 此时需要扩展原子操作的额度
 */
int journal_extend(struct journal_handle *handle, int block_count)
{
	struct transaction *trans;
	struct journal *journal;
	int wanted;
	int ret;

	trans = handle->transaction;
	journal = trans->journal;
	ret = -EIO;
	if (handle_is_aborted(handle))
		goto out;

	ret = 1;

	smp_lock(&journal->state_lock);

	if (handle->transaction->state != TRANS_RUNNING)
		goto unlock_state;

	smp_lock(&trans->lock);
	wanted = trans->reserved_credits + block_count;

	/**
	 * 申请的空间过大
	 */
	if (wanted > journal->max_block_in_trans)
		goto unlock;

	/**
	 * 没有足够多的日志空间了
	 */
	if (wanted > __log_free_space(journal)) {
		goto unlock;
	}

	/* 增加其额度 */
	handle->block_credits += block_count;
	trans->reserved_credits += block_count;

	ret = 0;

unlock:
	smp_unlock(&trans->lock);
unlock_state:
	smp_unlock(&journal->state_lock);
out:
	return ret;
}

/**
 * 在多事务的文件操作(如截断操作)中
 * 如果无法扩展事务，就提交并重启当前事务
 */
int journal_restart(struct journal_handle *handle, int block_count)
{
	struct transaction *trans;
	struct journal *journal;
	int ret;

	trans = handle->transaction;
	journal = trans->journal;

	if (handle_is_aborted(handle))
		return 0;

	ASSERT(trans->users > 0);
	ASSERT(journal_current_handle() == handle);

	smp_lock(&journal->state_lock);
	smp_lock(&trans->lock);
	trans->reserved_credits -= handle->block_credits;
	trans->users--;

	/**
	 * 没有谁在操作此事务了，唤醒等待的日志线程进行回写
	 */
	if (!trans->users)
		wake_up(&journal->wait_updates);
	smp_unlock(&trans->lock);

	/**
	 * 唤醒日志线程，要求它开始提交日志
	 */
	__log_start_commit(journal, trans->trans_id);
	smp_unlock(&journal->state_lock);

	handle->block_credits = block_count;
	ret = stick_handle(journal, handle);

	return ret;
}

/**
 * 块缓冲区失效，但是它在旧的检查点中
 * 因此需要将它记录到事务的forget链表中
 * 直到当前事务提交后再释放
 */
static int __dispose_block(struct blkbuf_journal_info *blkbuf_jinfo,
	struct transaction *trans)
{
	struct blkbuf_desc *blkbuf;
	int may_free = 1;

	blkbuf = journal_to_blkbuf(blkbuf_jinfo);
	__journal_takeout_list(blkbuf_jinfo);

	/**
	 * 在检查点链表中，将其放入事务的forget链表
	 * 由当前事务的检查点接着处理
	 */
	if (blkbuf_jinfo->checkpoint_trans) {
		__journal_putin_list(blkbuf_jinfo, trans, TRANS_QUEUE_FORGET);
		blkbuf_clear_journal_dirty(blkbuf);
		may_free = 0;
	} else {
		journal_info_detach(blkbuf);
		loosen_blkbuf(blkbuf);
	}

	return may_free;
}

/**
 * 使页面中单个块失效
 */
static int
__invalidate_block(struct journal *journal, struct blkbuf_desc *blkbuf)
{
	struct blkbuf_journal_info *blkbuf_jinfo;
	struct transaction *trans, *newest_trans;
	int may_free = 1;
	int ret;

	if (!blkbuf_is_journaled(blkbuf))
		goto clear_block;

	smp_lock(&journal->state_lock);
	blkbuf_lock_state(blkbuf);
	smp_lock(&journal->list_lock);

	blkbuf_jinfo = hold_blkbuf_jinfo(blkbuf);
	if (!blkbuf_jinfo)
		goto unlock;

	/**
	 * 块没有在事务中
	 */
	trans = blkbuf_jinfo->transaction;
	if (trans == NULL) {
		/**
		 * 也没有在检查点链表中
		 * 那么可以直接使块失效
		 */
		if (!blkbuf_jinfo->checkpoint_trans)
			goto loosen;

		/**
		 * 虽然在检查点链表中，但是已经回写完成
		 * 在释放日志块引用时，会做清理工作
		 */
		if (!blkbuf_is_dirty(blkbuf))
			goto loosen;

		if (journal->running_transaction)
			newest_trans = journal->running_transaction;
		else
			newest_trans = journal->committing_transaction;
		/**
		 * 运行到这里，说明块处于检查点链表中
		 * 并且为脏，需要处理
		 */
		if (newest_trans) {
			/**
			 * 放到最新事务的forget链表中
			 * 一旦该事务被提交，块缓冲区就会被释放
			 */
			ret = __dispose_block(blkbuf_jinfo, newest_trans);
			smp_unlock(&journal->list_lock);
			blkbuf_unlock_state(blkbuf);
			smp_unlock(&journal->state_lock);
			journal_info_loosen(blkbuf_jinfo);

			return ret;
		} else {
			blkbuf_clear_journal_dirty(blkbuf);

			goto loosen;
		}
	} else if (trans == journal->committing_transaction) {
		blkbuf_set_freed(blkbuf);
		/**
		 * 当前事务明显不再需要此块
		 */
		if (blkbuf_jinfo->next_trans) {
			ASSERT(blkbuf_jinfo->next_trans ==
					journal->running_transaction);
			blkbuf_jinfo->next_trans = NULL;
		}
		smp_unlock(&journal->list_lock);
		blkbuf_unlock_state(blkbuf);
		smp_unlock(&journal->state_lock);
		journal_info_loosen(blkbuf_jinfo);

		return 0;
	} else {
		/**
		 * 块属于当前运行事务
		 * 元数据已经在事务中，因此可以放心的丢弃块
		 */
		ASSERT(trans == journal->running_transaction);
		may_free = __dispose_block(blkbuf_jinfo, trans);
	}

loosen:
	journal_info_loosen(blkbuf_jinfo);
unlock:
	smp_unlock(&journal->list_lock);
	blkbuf_unlock_state(blkbuf);
	smp_unlock(&journal->state_lock);
clear_block:
	blkbuf_clear_dirty(blkbuf);
	ASSERT(!blkbuf_is_journal_dirty(blkbuf));
	blkbuf_clear_mapped(blkbuf);
	blkbuf_clear_requested(blkbuf);
	blkbuf_clear_new(blkbuf);
	blkbuf->blkdev = NULL;

	return may_free;
}

/**
 * 使页面缓冲区失效，在截断文件时调用
 */
int journal_invalidatepage(struct journal *journal, 
	struct page_frame *page, unsigned long offset)
{
	struct blkbuf_desc *head, *blkbuf;
	unsigned int curr_off = 0;
	int may_free = 1;

	if (!pgflag_locked(page))
		BUG();
	/**
	 * 没有缓冲区，也就没有接受日志管理
	 */
	if (!page_has_blocks(page))
		return 1;

	/**
	 * 遍历页面中所有缓冲区
	 * 试图使缓冲区日志信息失效
	 */
	head = page_first_block(page);
	blkbuf = head;
	do {
		if (offset <= curr_off) {
		 	blkbuf_lock(blkbuf);
			may_free &= __invalidate_block(journal, blkbuf);
			blkbuf_unlock(blkbuf);
		}
		curr_off = curr_off + blkbuf->size;
		blkbuf = blkbuf->next_in_page;
	} while (blkbuf != head);

	/**
	 * 如果是整页失效，那么试图释放页面中所有块描述符
	 */
	if (!offset) {
		if (!may_free || !try_to_free_blocks(page))
			return 0;

		ASSERT(!page_has_blocks(page));
	}

	/**
	 * 返回1表示调用者可以释放页面
	 */
	return 1;
}

static void
__free_one_block(struct journal *journal, struct blkbuf_desc *blkbuf)
{
	struct blkbuf_journal_info *blkbuf_jinfo;

	blkbuf_jinfo = blkbuf_to_journal(blkbuf);

	/**
	 * 缓冲区还没有回写
	 */
	if (blkbuf_is_locked(blkbuf) || blkbuf_is_dirty(blkbuf))
		return;

	if (blkbuf_jinfo->next_trans != 0)
		return;

	smp_lock(&journal->list_lock);
	if (blkbuf_jinfo->transaction && !blkbuf_jinfo->checkpoint_trans) {
		/**
		 * 由日志管理的数据块
		 * 但是不脏，或者说已经回写完毕
		 * 因此可以从日志中删除了
		 */
		if (blkbuf_jinfo->which_list == TRANS_QUEUE_DIRTY_DATA
		    || blkbuf_jinfo->which_list == TRANS_QUEUE_LOCKED_DATA) {
			__journal_takeout_list(blkbuf_jinfo);
			journal_info_detach(blkbuf);
			loosen_blkbuf(blkbuf);
		}
	/**
	 * 在检查点链表中
	 */
	} else if (blkbuf_jinfo->checkpoint_trans && !blkbuf_jinfo->transaction) {
		if (blkbuf_jinfo->which_list == TRANS_QUEUE_NONE) {
			/**
			 * 从检查点链表中摘除
			 */
			__journal_takeout_checkpoint(blkbuf_jinfo);
			journal_info_detach(blkbuf);
			loosen_blkbuf(blkbuf);
		}
	}
	smp_unlock(&journal->list_lock);
}

/**
 * 释放缓存页面时，释放相关的日志缓冲区
 */
int journal_release_page(struct journal *journal, 
	struct page_frame *page, int paf_mask)
{
	struct blkbuf_desc *blkbuf;
	struct blkbuf_desc *head;
	int ret = 0;

	ASSERT(pgflag_locked(page));

	head = page_first_block(page);
	blkbuf = head;
	/**
	 * 遍历页面中的所有块缓冲区
	 */
	do {
		struct blkbuf_journal_info *blkbuf_jinfo;

		/**
		 * 该 块不接受日志管理
		 */
		blkbuf_jinfo = hold_blkbuf_jinfo(blkbuf);
		if (!blkbuf_jinfo)
			continue;

		/**
		 * 释放单个文件块缓冲区
		 */
		blkbuf_lock_state(blkbuf);
		__free_one_block(journal, blkbuf);
		journal_info_loosen(blkbuf_jinfo);
		blkbuf_unlock_state(blkbuf);
		if (blkbuf_is_journaled(blkbuf))
			goto busy;
	} while ((blkbuf = blkbuf->next_in_page) != head);

	/**
	 * 所有块都不存在日志信息了
	 * 调用VFS层释放页面中所有块描述符
	 */
	ret = try_to_free_blocks(page);
busy:
	return ret;
}

/**
 * 当日志操作失败时，将已经加入到队列中缓冲区忘记掉
 * 这是针对数据块来说的。
 * 元数据缓冲区应当调用journal_revoke并间接调用到此。
 */
int journal_forget(struct journal_handle *handle, struct blkbuf_desc *blkbuf)
{
	struct blkbuf_journal_info *blkbuf_jinfo;
	struct transaction *trans;
	struct journal *journal;
	int err = 0;

	trans = handle->transaction;
	journal = trans->journal;

	blkbuf_lock_state(blkbuf);
	smp_lock(&journal->list_lock);

	/**
	 * 块还没有被日志所管理，直接退
	 */
	if (!blkbuf_is_journaled(blkbuf))
		goto out;

	blkbuf_jinfo = blkbuf_to_journal(blkbuf);
	/**
	 * 位图块既不会是元数据块，也不是数据块
	 * 明显是程序逻辑错误，退出
	 */
	if (blkbuf_jinfo->undo_copy) {
		err = -EIO;
		goto out;
	}

	/**
	 * 该缓冲区属于当前事务
	 */
	if (blkbuf_jinfo->transaction == handle->transaction) {
		ASSERT(!blkbuf_jinfo->bufdata_copy);

		/**
		 * 块属于当前事务的一部分
		 * 直接从当前事务的链表中删除
		 */
		blkbuf_clear_dirty(blkbuf);
		blkbuf_clear_journal_dirty(blkbuf);
		__journal_takeout_list(blkbuf_jinfo);

		/**
		 * 有检查点在等待本缓冲区
		 */
		if (blkbuf_jinfo->checkpoint_trans)
			/**
			 * 将其加入到forget队列
			 * 这样检查点就不会处理这个缓冲区了
			 */
			__journal_putin_list(blkbuf_jinfo, trans, TRANS_QUEUE_FORGET);
		else {
			/**
			 * 释放相关资源
			 */
			journal_info_detach(blkbuf);
			loosen_blkbuf(blkbuf);
			if (!blkbuf_is_journaled(blkbuf)) {
				smp_unlock(&journal->list_lock);
				blkbuf_unlock_state(blkbuf);
				blkbuf_forget(blkbuf);

				return 0;
			}
		}
	/** 
	 * 不属于当前事务
	 */
	} else if (blkbuf_jinfo->transaction) {
		ASSERT(blkbuf_jinfo->transaction == journal->committing_transaction);

		/**
		 * 清除此标志，表示本事务不再处理此缓冲区
		 */
		if (blkbuf_jinfo->next_trans) {
			ASSERT(blkbuf_jinfo->next_trans == trans);
			blkbuf_jinfo->next_trans = NULL;
		}
	}

out:
	smp_unlock(&journal->list_lock);
	blkbuf_unlock_state(blkbuf);
	loosen_blkbuf(blkbuf);
	return err;
}

/**
 * 通知JBD，相应的磁盘数据块缓冲区已经修改完毕
 * 仅仅针对ORDERED类型的日志，这样日志会跟踪相应的磁盘块状态
 */
int journal_dirty_data(struct journal_handle *handle, struct blkbuf_desc *blkbuf)
{
	struct blkbuf_journal_info *blkbuf_jinfo;
	struct journal *journal;
	int need_loosen = 0;

	if (handle_is_aborted(handle))
		return 0;

	journal = handle->transaction->journal;
	blkbuf_jinfo = journal_info_hold_alloc(blkbuf);

	blkbuf_lock_state(blkbuf);
	smp_lock(&journal->list_lock);
	/**
	 * 块已经由某个事务管理了
	 * 有两种情况 :
	 *	1、块是当前事务的一部分。这是由于我们曾经作为元数据使用该块
	 *		然后又释放了块并重新将其作为数据块
	 *	2、块是前一个提交事务的一部分。必须保证它不是前一事务元数据块
	 *		此时不需要特别处理。
	 */
	if (blkbuf_jinfo->transaction) {
		/**
		 * 不是当前事务在管理
		 */
		if (blkbuf_jinfo->transaction != handle->transaction) {
			/**
			 * 必然应当属于前一个提交事务
			 */
			ASSERT(blkbuf_jinfo->transaction
				== journal->committing_transaction);

			/**
			 * 在上一个事务中，数据块被作为日志的一部分记录到日志中
			 * 但是在当前事务中，它被write调用作为普通的数据块进行标记
			 * 这种情况下，不需要做特别的处理，崩溃也能正常恢复
			 */

			/**
			 * 不在这三个链表中，那么说明在元数据链表中
			 * 此时不需要做任务事情
			 */
			if (blkbuf_jinfo->which_list != TRANS_QUEUE_NONE &&
			    blkbuf_jinfo->which_list != TRANS_QUEUE_DIRTY_DATA &&
			    blkbuf_jinfo->which_list != TRANS_QUEUE_LOCKED_DATA)
				goto pass;

			/**
			 * 如果缓冲区已经变脏，说明上一个事务已经提交了此缓冲区
			 * 正在等待写入磁盘
			 * 此时不能直接返回并让调用者再次将其置脏
			 * 因为这样可能会导致缓冲区反复为脏，使得上一个事务迟迟不能结束
			 */
			if (blkbuf_is_dirty(blkbuf)) {
				/**
				 * 持有缓冲区引用后再释放锁
				 */
				hold_blkbuf(blkbuf);
				smp_unlock(&journal->list_lock);
				blkbuf_unlock_state(blkbuf);
				need_loosen = 1;
				/**
				 * 同步脏数据到磁盘
				 */
				sync_dirty_block(blkbuf);
				blkbuf_lock_state(blkbuf);
				smp_lock(&journal->list_lock);
			}

			/**
			 * 在休眠期间，事务可能发生了变化
			 * 再次判断一下
			 */
			if (blkbuf_jinfo->transaction != NULL)
				/**
				 * 将它从原事务的链表中删除
				 * 合适的时候再放到相应的链表中
				 */
				__journal_takeout_list(blkbuf_jinfo);
		}

		/**
		 * 如果块被分配然后立即被释放
		 * 那么它有可能存在于元数据链表中
		 */
		if (blkbuf_jinfo->which_list != TRANS_QUEUE_DIRTY_DATA
		    && blkbuf_jinfo->which_list != TRANS_QUEUE_LOCKED_DATA) {
			ASSERT(blkbuf_jinfo->which_list != TRANS_QUEUE_META_ORIG);
			/**
			 * 从原来的链表中摘除
			 * 并放到SyncData链表中，按照ORDERED模式的数据块来管理
			 */
			__journal_takeout_list(blkbuf_jinfo);
			__journal_putin_list(blkbuf_jinfo, handle->transaction,
				TRANS_QUEUE_DIRTY_DATA);
		}
	} else
		/**
		 * 以前没有接受任何事务的管理
		 * 直接加到本事务的SyncData链表
		 */
		__journal_putin_list(blkbuf_jinfo, handle->transaction, TRANS_QUEUE_DIRTY_DATA);

pass:
	smp_unlock(&journal->list_lock);
	blkbuf_unlock_state(blkbuf);
	if (need_loosen)
		loosen_blkbuf(blkbuf);
	journal_info_loosen(blkbuf_jinfo);

	return 0;
}

/**
 * 通知JBD，元数据缓冲区已经修改完成
 * 标记包含元数据的缓冲区包含脏数据
 */
int journal_dirty_metadata(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf)
{
	struct blkbuf_journal_info *blkbuf_jinfo;
	struct transaction *trans;
	struct journal *journal;

	if (handle_is_aborted(handle))
		return -EROFS;

	trans = handle->transaction;
	journal = trans->journal;
	blkbuf_jinfo = blkbuf_to_journal(blkbuf);

	blkbuf_lock_state(blkbuf);

	/**
	 * 已经在当前事务的元数据队列中了
	 */
	if (blkbuf_jinfo->transaction == trans
	    && blkbuf_jinfo->which_list == TRANS_QUEUE_METADATA) {
		ASSERT(blkbuf_jinfo->transaction == journal->running_transaction);
		goto out;
	}

	/**
	 * 设置日志脏(不是缓冲区脏)标志
	 */
	blkbuf_set_journal_dirty(blkbuf);

	/**
	 * 该缓冲区正在提交事务里
	 * 此时不能从链表中移动它
	 * 等待日志提交完成后再接着处理
	 */
	if (blkbuf_jinfo->transaction != trans) {
		ASSERT(blkbuf_jinfo->transaction ==
			journal->committing_transaction);
		ASSERT(blkbuf_jinfo->next_trans == trans);
		goto out;
	}

	ASSERT(blkbuf_jinfo->bufdata_copy == NULL);
	smp_lock(&journal->list_lock);
	/**
	 * 可能会将缓冲区从保留队列中摘除
	 * 然后加到元数据队列
	 */
	__journal_putin_list(blkbuf_jinfo, handle->transaction, TRANS_QUEUE_METADATA);
	smp_unlock(&journal->list_lock);
out:
	blkbuf_unlock_state(blkbuf);
	return 0;
}

/**
 * 将新创建的元数据块缓冲区放到事务中管理
 */
int journal_get_create_access(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf) 
{
	struct blkbuf_journal_info *blkbuf_jinfo;
	struct transaction *trans;
	struct journal *journal;
	bool valid;
	int err;

	trans = handle->transaction;
	journal = trans->journal;
	/**
	 * 获得磁盘块的日志描述符引用
	 */
	blkbuf_jinfo = journal_info_hold_alloc(blkbuf);

	err = -EROFS;
	if (handle_is_aborted(handle))
		goto out;

	err = 0;
	blkbuf_lock_state(blkbuf);
	smp_lock(&journal->list_lock);

	valid = blkbuf_jinfo->transaction == trans ||
		blkbuf_jinfo->transaction == NULL;
	if (!valid)
		valid = (blkbuf_jinfo->which_list == TRANS_QUEUE_FORGET) &&
			(blkbuf_jinfo->transaction == journal->committing_transaction);
	ASSERT(valid);
	ASSERT(blkbuf_jinfo->next_trans == NULL);
	ASSERT(blkbuf_is_locked(journal_to_blkbuf(blkbuf_jinfo)));
	ASSERT(handle->block_credits > 0);
	handle->block_credits--;

	/**
	 * 在这之前，缓冲区没有被日志管理
	 */
	if (blkbuf_jinfo->transaction == NULL) {
		blkbuf_jinfo->transaction = trans;
		/**
		 * 将缓冲区加入到事务的保留缓冲区
		 */
		__journal_putin_list(blkbuf_jinfo, trans, TRANS_QUEUE_RESERVED);
	} else if (blkbuf_jinfo->transaction == journal->committing_transaction)
		/**
		 * 表明当前事务希望接着修改此缓冲区。
		 */
		blkbuf_jinfo->next_trans = trans;

	smp_unlock(&journal->list_lock);
	blkbuf_unlock_state(blkbuf);

	/**
	 * 该缓冲区明显是需要被继续使用了
	 * 应当从撤销表中取消
	 */
	journal_cancel_revoke(handle, blkbuf_jinfo);
	journal_info_loosen(blkbuf_jinfo);

out:
	return err;
}

static int __get_write_access(struct journal_handle *handle,
	struct blkbuf_journal_info *blkbuf_jinfo, int force_copy,
	int *credits) 
{
	struct transaction *trans;
	char *bufdata_copy = NULL;
	struct blkbuf_desc *blkbuf;
	struct journal *journal;
	int need_copy = 0;
	int ret;

	trans = handle->transaction;
	journal = trans->journal;
	blkbuf = journal_to_blkbuf(blkbuf_jinfo);

	if (handle_is_aborted(handle))
		return -EROFS;

repeat:
	blkbuf_lock(blkbuf);
	blkbuf_lock_state(blkbuf);

	/**
	 * 缓冲区脏，可能是工具在修改缓冲区
	 */
	if (blkbuf_is_dirty(blkbuf)) {
		/**
		 * 如果缓冲区与事务关联，说明遇到了逻辑错误
		 */
		if (blkbuf_jinfo->transaction) {
			int which_list;

			ASSERT(blkbuf_jinfo->transaction == trans || 
				blkbuf_jinfo->transaction == journal->committing_transaction);
			ASSERT(!blkbuf_jinfo->next_trans
				|| blkbuf_jinfo->next_trans == trans);

			WARN("Unexpected dirty buffer");

			/**
			 * 脏标志应该由BH_JBDDirty来表示
			 * 因此强制清除块缓冲区的脏标志并设置BH_JBDDirty标志
			 */
			which_list = blkbuf_jinfo->which_list;
			if (which_list == TRANS_QUEUE_METADATA || which_list == TRANS_QUEUE_RESERVED || 
			    which_list == TRANS_QUEUE_META_ORIG || which_list == TRANS_QUEUE_FORGET) {
				if (blkbuf_test_clear_dirty(blkbuf))
					atomic_set_bit(BS_JOURNAL_DIRTY, &blkbuf->state);
			}
		}
 	}

	blkbuf_unlock(blkbuf);

	ret = -EROFS;
	if (handle_is_aborted(handle)) {
		blkbuf_unlock_state(blkbuf);
		goto out;
	}

	ret = 0;
	/**
	 * 当前事务已经在管理此缓冲区了，退出
	 */
	if (blkbuf_jinfo->transaction == trans ||
	    blkbuf_jinfo->next_trans == trans)
		goto done;

	/**
	 * 缓冲区还没有处于日志中，需要添加到日志链表中
	 * 首先判断是否复制过数据
	 * 如果复制过说明处于前面的日志中
	 * 并且前一个事务修改了数据
	 */
	if (blkbuf_jinfo->bufdata_copy) {
		ASSERT(blkbuf_jinfo->next_trans == NULL);
		/**
		 * 接着上一个日志进行处理
		 */
		blkbuf_jinfo->next_trans = trans;

		/**
		 * 减少当前句柄的日志配额
		 */
		ASSERT(handle->block_credits > 0);
		handle->block_credits--;
		if (credits)
			(*credits)++;

		goto done;
	}

	/**
	 * 其他事务在处理该缓冲区
	 */
	if (blkbuf_jinfo->transaction && blkbuf_jinfo->transaction != trans) {
		/**
		 * 上一个事务必然在提交过程中
		 */
		ASSERT(blkbuf_jinfo->next_trans == NULL);
		ASSERT(blkbuf_jinfo->transaction == journal->committing_transaction);

		/**
		 * 上一个事务在使用主缓冲区，而没有复制一份数据
		 */
		if (blkbuf_jinfo->which_list == TRANS_QUEUE_META_ORIG) {
			/**
			 * 等待上一个事务结束
			 * 然后它会唤醒本事务，以开始对bh的写操作。
			 */
			DEFINE_WAIT_BIT(wait, &blkbuf->state, BS_WAIT_LOGGED);
			struct wait_queue *wait_queue;

			wait_queue = bit_waitqueue(&blkbuf->state, BS_WAIT_LOGGED);
			blkbuf_unlock_state(blkbuf);
			/**
			 * 等待上一个事务解除对主缓冲区的引用
			 */
			while (1) {
				prepare_to_wait(wait_queue, &wait.wait,
						TASK_UNINTERRUPTIBLE);
				if (blkbuf_jinfo->which_list != TRANS_QUEUE_META_ORIG)
					break;
				schedule();
			}
			finish_wait(wait_queue, &wait.wait);

			/**
			 * 不过是从头再来:-)
			 */
			goto repeat;
		}

		/**
		 * 上一个事务持有此缓冲区，并且没有处于Forget链表中
		 */
		if (blkbuf_jinfo->which_list != TRANS_QUEUE_FORGET || force_copy) {
			/**
			 * 需要进行复制，因此需要先分配内存
			 */
			if (!bufdata_copy) {
				blkbuf_unlock_state(blkbuf);
				bufdata_copy = jbd_kmalloc(blkbuf->size, PAF_NOFS);
				if (!bufdata_copy) {
					printk(KERN_EMERG "%s: OOM at %d\n",
					       __FUNCTION__, __LINE__);
					ret = -ENOMEM;
					blkbuf_lock_state(blkbuf);
					goto done;
				}

				goto repeat;
			}

			blkbuf_jinfo->bufdata_copy = bufdata_copy;
			bufdata_copy = NULL;
			need_copy = 1;
		}

		blkbuf_jinfo->next_trans = trans;
	}

	/**
	 * 减少当前句柄的日志配额
	 */
	ASSERT(handle->block_credits > 0);
	handle->block_credits--;
	if (credits)
		(*credits)++;

	/**
	 * 块缓冲区还没有处于事务中
	 */
	if (!blkbuf_jinfo->transaction) {
		ASSERT(!blkbuf_jinfo->next_trans);
		blkbuf_jinfo->transaction = trans;
		smp_lock(&journal->list_lock);
		/**
		 * 将缓冲区添加到保留链表中
		 */
		__journal_putin_list(blkbuf_jinfo, trans, TRANS_QUEUE_RESERVED);
		smp_unlock(&journal->list_lock);
	}

done:
	if (need_copy) {
		struct page_frame *page;
		char *source;
		int offset;

		/**
		 * 将源缓冲区的数据复制过来
		 */
		page = blkbuf->page;
		offset = ((unsigned long)blkbuf->block_data) & ~PAGE_MASK;
		source = kmap_atomic(page, KM_USER0);
		memcpy(blkbuf_jinfo->bufdata_copy, source + offset, blkbuf->size);
		kunmap_atomic(source, KM_USER0);
	}
	blkbuf_unlock_state(blkbuf);

	journal_cancel_revoke(handle, blkbuf_jinfo);
out:
	if (bufdata_copy)
		kfree(bufdata_copy);

	return ret;
}

/**
 * 通知日志层，想要对已有的元数据块进行写操作
 */
int journal_get_write_access(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf, int *credits)
{
	struct blkbuf_journal_info *blkbuf_jinfo;
	int ret;

	blkbuf_jinfo = journal_info_hold_alloc(blkbuf);

	/**
	 * 将缓冲区加入到事务的队列中
	 * 参数0表示不强制复制缓冲区内容
	 */
	ret = __get_write_access(handle, blkbuf_jinfo, 0, credits);

	journal_info_loosen(blkbuf_jinfo);

	return ret;
}

/**
 * 获得不可回退权限
 * 操作位图元数据时使用，只要位图块还没有提交，它释放的块就不能被分配
 * 否则在提交日志前崩溃，就会破坏文件系统
 */
int journal_get_undo_access(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf, int *credits)
{
	struct blkbuf_journal_info *blkbuf_jinfo;
	char *undo_copy = NULL;
	int ret;

	blkbuf_jinfo = journal_info_hold_alloc(blkbuf);

	/**
	 * 这里传入1，表示需要强制复制一份源数据
	 * 在分配逻辑块时，需要同时判断日志中的位图块和内存中的位图块
	 */
	ret = __get_write_access(handle, blkbuf_jinfo, 1, credits);
	if (ret)
		goto out;

alloc:
	/**
	 * 对于位图块，必须复制一份数据写到日志中
	 */
	if (!blkbuf_jinfo->undo_copy) {
		undo_copy = jbd_kmalloc(blkbuf->size, PAF_NOFS);
		if (!undo_copy) {
			ret = -ENOMEM;
			goto out;
		}
	}

	blkbuf_lock_state(blkbuf);

	if (!blkbuf_jinfo->undo_copy) {
		if (!undo_copy) {
			blkbuf_unlock_state(blkbuf);
			goto alloc;
		}

		blkbuf_jinfo->undo_copy = undo_copy;
		undo_copy = NULL;
		/**
		 * 对于位图块，总是不会使用高端内存
		 * 这里直接复制即可
		 */
		memcpy(blkbuf_jinfo->undo_copy, blkbuf->block_data, blkbuf->size);
	}

	blkbuf_unlock_state(blkbuf);
out:
	journal_info_loosen(blkbuf_jinfo);

	if (undo_copy)
		kfree(undo_copy);

	return ret;
}

/**
 * 将新创建的事务作为当前日志的事务
 */
static struct transaction *
stick_running_transaction(struct journal *journal,
	struct transaction *trans)
{
	trans->journal = journal;
	trans->state = TRANS_RUNNING;
	trans->trans_id = journal->next_trans;
	journal->next_trans++;
	trans->timeout = jiffies + journal->commit_interval;
	smp_lock_init(&trans->lock);

	/**
	 * 启动定时器，到期后强制提交事务
	 */
	journal->commit_timer->expire = trans->timeout;
	timer_add(journal->commit_timer);

	ASSERT(journal->running_transaction == NULL);
	journal->running_transaction = trans;

	return trans;
}

/**
 * 将事务句柄与日志绑定起来
 */
static int
stick_handle(struct journal *journal, struct journal_handle *handle)
{
	struct transaction *new_trans = NULL;
	struct transaction *trans;
	int block_credits;
	int needed;
	int ret = 0;

	/**
	 * 事务申请的日志块超过了允许的最大数量
	 */
	block_credits = handle->block_credits;
	if (block_credits > journal->max_block_in_trans) {
		printk(KERN_ERR "JBD: %s wants too many credits (%d > %d)\n",
			current->name, block_credits, journal->max_block_in_trans);
		ret = -ENOSPC;
		goto out;
	}

alloc:
	/**
	 * 为日志分配事务描述符
	 */
	if (!journal->running_transaction) {
		new_trans = jbd_kmalloc(sizeof(*new_trans), PAF_NOFS | __PAF_ZERO);
		if (!new_trans) {
			ret = -ENOMEM;
			goto out;
		}
		list_init(&new_trans->reserved_list);
		list_init(&new_trans->locked_data_list);
		list_init(&new_trans->metadata_list);
		list_init(&new_trans->data_block_list);
		list_init(&new_trans->forget_list);
		list_init(&new_trans->checkpoint_list);
		list_init(&new_trans->meta_log_list);
		list_init(&new_trans->meta_orig_list);
		list_init(&new_trans->ctrldata_list);
		list_init(&new_trans->list_checkpoint);
	}

	/**
	 * 循环等待，直到日志空间可用
	 */
	while (1) {
		smp_lock(&journal->state_lock);

		/**
		 * 日志处于错误状态，并且没有谁恢复过
		 */
		if (journal_is_aborted(journal) ||
		    (journal->errno && !(journal->flags & JSTATE_ACK_ERR))) {
			smp_unlock(&journal->state_lock);
			ret = -EROFS; 
			goto out;
		}

		/**
		 * 有任务在创建屏障，必须等待前面的任务结束
		 */
		if (journal->barrier_count) {
			smp_unlock(&journal->state_lock);
			/**
			 * 等待直到所有屏障都已经完成
			 */
			cond_wait(journal->wait_new_trans,
				journal->barrier_count == 0);
			continue;
		}

		if (!journal->running_transaction) {
			if (!new_trans) {
				smp_unlock(&journal->state_lock);
				goto alloc;
			}

			/**
			 * 将新分配的事务作为日志的当前事务
			 */
			stick_running_transaction(journal, new_trans);
			new_trans = NULL;
		}

		trans = journal->running_transaction;

		/**
		 * TNND :-)
		 * 好事多磨，都到这一步了，事务竟然被锁住了
		 * 也就是说，日志线程正在准备写入此事务，需要开启新事务
		 * 再等等，再等等
		 */
		if (trans->state == TRANS_PREPARE_COMMIT) {
			DEFINE_WAIT(wait);

			/**
			 * 这时为什么不能用cond_wait ?
			 */
			prepare_to_wait(&journal->wait_new_trans, &wait,
				TASK_UNINTERRUPTIBLE);
			smp_unlock(&journal->state_lock);
			schedule();
			finish_wait(&journal->wait_new_trans, &wait);

			continue;
		}

		/*
		 * 开始判断事务空间，先获得事务的锁
		 */
		smp_lock(&trans->lock);
		needed = trans->reserved_credits + block_credits;

		/**
		 * 所需要保留的空间不足，必须等待前面的日志操作完成
		 */
		if (needed > journal->max_block_in_trans) {
			DEFINE_WAIT(wait);

			smp_unlock(&trans->lock);
			prepare_to_wait(&journal->wait_new_trans, &wait,
				TASK_UNINTERRUPTIBLE);
			/**
			 * 先唤醒日志守护线程，写入日志
			 */
			__log_start_commit(journal, trans->trans_id);
			smp_unlock(&journal->state_lock);
			/**
			 * 等待事务完成
			 */
			schedule();
			finish_wait(&journal->wait_new_trans, &wait);
			continue;
		}

		/**
		 * 在没有强制进行检查点操作时，提交日志的代码不能检查日志空间
		 * 提交代码也并不能进行检查点操作，因此为会导致死锁
		 * 因此，在获得日志操作句柄时，必须确保必要的空间
		 */
		if (__log_free_space(journal) < journal_needed_space(journal)) {
			smp_unlock(&trans->lock);
			/**
			 * 强制进行检查点操作，直到可用空间满足日志需求
			 */
			__journal_wait_space(journal);
			smp_unlock(&journal->state_lock);
			continue;
		}

		break;
	}

	/**
	 * 万事俱备，将句柄与事务绑定起来
	 */
	handle->transaction = trans;
	trans->reserved_credits += block_credits;
	/**
	 * users->句柄数量
	 * users_sequence->句柄序号
	 */
	trans->users++;
	trans->users_sequence++;
	smp_unlock(&trans->lock);
	smp_unlock(&journal->state_lock);
out:
	if (new_trans)
		kfree(new_trans);
	return ret;
}

/**
 * 开始一个日志操作
 * 获得原子操作描述符，如果已经有一个，则直接返回。
 * 如果没有事务，则创建一个。
 */
struct journal_handle *journal_start(struct journal *journal, int block_count)
{
	struct journal_handle *handle;
	int err;

	handle = journal_current_handle();
	/**
	 * 没有日志支持，只读文件系统
	 */
	if (!journal)
		return ERR_PTR(-EROFS);

	/**
	 * 当前线程已经有日志句柄了
	 * 添加引用计数后退出
	 */
	if (handle) {
		ASSERT(handle->transaction->journal == journal);
		handle->ref_count++;
		return handle;
	}

	/**
	 * 否则分配一个新日志句柄
	 */
	handle = alloc_handle(block_count);
	if (!handle)
		return ERR_PTR(-ENOMEM);

	journal_set_current_handle(handle);

	/**
	 * 在当前运行的事务中，启动一个新原子操作。
	 * 如果没有运行的事务，或者事务过大，则新创建一个。
	 */
	err = stick_handle(journal, handle);
	if (err < 0) {
		beehive_free(journal_handle_allotter, handle);
		current->journal_info = NULL;
		handle = ERR_PTR(err);
	}

	return handle;
}

/**
 * 线程对日志句柄的操作已经完成。
 * 将原子操作与事务断开连接，调整事务的额度。
 * 如果原子操作是同步的，则同步等待事务完成。
 */
int journal_stop(struct journal_handle *handle)
{
	struct transaction *trans;
	int users_sequence, err;
	struct journal *journal;

	trans = handle->transaction;
	journal = trans->journal;
	ASSERT(trans->users > 0);
	ASSERT(journal_current_handle() == handle);

	if (handle_is_aborted(handle))
		err = -EIO;
	else
		err = 0;

	/**
	 * 递减引用计数，如果还没有完全释放句柄则退出
	 */
	handle->ref_count--;
	if (handle->ref_count > 0) {
		return err;
	}

	/**
	 * 对于同步句柄，后面要同步提交日志
	 * 为了效率，这里等待一下其他句柄，尽量一次性多提交一点
	 */
	if (handle->sync) {
		do {
			users_sequence = trans->users_sequence;
			msleep(1);
		} while (users_sequence != trans->users_sequence);
	}

	current->journal_info = NULL;
	smp_lock(&journal->state_lock);
	smp_lock(&trans->lock);
	/**
	 * 减少日志保留额度及日志句柄数量
	 */
	trans->reserved_credits -= handle->block_credits;
	trans->users--;
	if (trans->users == 0) {
		/**
		 * 当前事务已经没有更新者，可以唤醒提交任务了
		 */
		wake_up(&journal->wait_updates);
		/**
		 * 有线程在等待实施IO屏障，目前句柄全部结束
		 * 可以处理IO屏障了
		 */
		if (journal->barrier_count)
			wake_up(&journal->wait_new_trans);
	}

	/**
	 * 以下几种情况均需要提交日志
	 *	1、句柄是同步的
	 *	2、日志保留额度过多，这个条件目前看来不会成立
	 *	3、事务老化
	 */
	if (handle->sync
	    || trans->reserved_credits > journal->max_block_in_trans
	    || time_after_eq((unsigned long)jiffies, trans->timeout)) {
		trans_id_t trans_id;

		trans_id = trans->trans_id;
		smp_unlock(&trans->lock);
		/**
		 * 唤醒日志线程，处理该事务
		 */
		__log_start_commit(journal, trans_id);
		smp_unlock(&journal->state_lock);

		/**
		 * 需要同步等待日志提交完成
		 */
		if (handle->sync && !(current->flags & TASKFLAG_RECLAIM))
			err = log_wait_commit(journal, trans_id);
	} else {
		smp_unlock(&trans->lock);
		smp_unlock(&journal->state_lock);
	}

	beehive_free(journal_handle_allotter, handle);
	return err;
}
