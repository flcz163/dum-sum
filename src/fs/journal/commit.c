#include <dim-sum/blk_dev.h>
#include <dim-sum/journal.h>

/**
 * 将当前运行事务切换为待提交事务
 */
static void
switch_running_trans(struct journal *journal, struct transaction *trans)
{
	struct blkbuf_journal_info *blkbuf_jinfo;

	/**
	 * 在切换事务状态前，先获得日志的状态锁
	 */
	smp_lock(&journal->state_lock);

	/**
	 * 准备提交状态，新申请的日志句柄将不再与当前事务绑定
	 */
	trans->state = TRANS_PREPARE_COMMIT;

	smp_lock(&trans->lock);
	/**
	 * 等待已经存在的原子操作完成。
	 */
	while (trans->users) {
		DEFINE_WAIT(wait);

		prepare_to_wait(&journal->wait_updates, &wait,
					TASK_UNINTERRUPTIBLE);
		/**
		 * 还有进程在使用此事务提交日志，等待其结束
		 */
		if (trans->users) {
			smp_unlock(&trans->lock);
			smp_unlock(&journal->state_lock);
			schedule();
			smp_lock(&journal->state_lock);
			smp_lock(&trans->lock);
		}
		finish_wait(&journal->wait_updates, &wait);
	}
	smp_unlock(&trans->lock);

	ASSERT(trans->reserved_credits <= journal->max_block_in_trans);

	/**
	 * 在初始化事务时，有一些预留的块缓存区对象。
	 * 调用者在这些缓冲区上申请了日志操作权限
	 * 但是后来并没有修改块级别区的内容，因此日志模块不用处理它
	 * 当一个大的文件操作(如文件截断)将操作分散在多个事务中时
	 * 会出现这种情况
	 */
	while (!list_is_empty(&trans->reserved_list)) {
		blkbuf_jinfo = list_first_container(&trans->reserved_list,
			struct blkbuf_journal_info, list_trans);

		/**
		 * 获取undo权限时，会复制一份位图块
		 * 这里将其释放
		 */
		if (blkbuf_jinfo->undo_copy) {
			struct blkbuf_desc *blkbuf = journal_to_blkbuf(blkbuf_jinfo);

			/**
			 * 获取缓冲区状态锁以后再次判断
			 */
			blkbuf_lock_state(blkbuf);
			if (blkbuf_jinfo->undo_copy) {
				kfree(blkbuf_jinfo->undo_copy);
				blkbuf_jinfo->undo_copy = NULL;
			}
			blkbuf_unlock_state(blkbuf);
		}

		/**
		 * 从链表中摘除此缓冲区
		 * 如果下一个事务继续处理此缓冲区，则放进下一个事务的链表中
		 * 否则将其释放
		 */
		journal_putin_next_list(journal, blkbuf_jinfo);
	}

	smp_lock(&journal->list_lock);
	/**
	 * 搜索检查点链表，将其中完成回写的块释放掉
	 * 以回收一部分内存
	 */
	__journal_clean_checkpoint_list(journal);
	smp_unlock(&journal->list_lock);

	/**
	 * 切换日志的撤销表，原撤销表属于提交事务
	 * 新撤销表属于当前事务
	 */
	journal_switch_revoke_table(journal);

	/**
	 * 将当前运行的事务切换为提交事务
	 */
	journal->running_transaction = NULL;
	journal->committing_transaction = trans;
	/**
	 * 修改事务状态，准备同步写入数据块了(data=ordered)
	 */
	trans->state = TRANS_SYNCDATA;
	/**
	 * 将事务的起始块号设置为日志的可用块号
	 */
	trans->start_block_num = journal->free_block_num;
	/**
	 * 已经可以开始新事务了
	 * 唤醒等待的事务
	 */
	wake_up(&journal->wait_new_trans);
	smp_unlock(&journal->state_lock);
}

/**
 * 一次性向块设备层提交数据块请求
 */
static void submit_data_block_bulk(struct blkbuf_desc **blkbuf_bulk, int buf_count)
{
	int i;

	for (i = 0; i < buf_count; i++) {
		blkbuf_bulk[i]->finish_io = blkbuf_finish_write_block;
		submit_block_request(WRITE, blkbuf_bulk[i]);
	}
}

/*
 *  Submit all the data buffers to disk
 */
static void submit_data_blocks(struct journal *journal,
				struct transaction *trans)
{
	struct blkbuf_journal_info *blkbuf_jinfo;
	struct blkbuf_desc **blkbuf_bulk;
	struct blkbuf_desc *blkbuf;
	int buf_count = 0;
	int locked;

	/**
	 * 在休眠期间，日志数据块链表可能会发生变化
	 * 因此休眠后需要重新获得获得锁并重试
	 *
	 * 即使在日志中止的情况下，也需要回写数据
	 */
	blkbuf_bulk = journal->blkbuf_bulk;
	smp_lock(&journal->list_lock);

	while (!list_is_empty(&trans->data_block_list)) {
		blkbuf_jinfo = list_first_container(&trans->data_block_list,
			struct blkbuf_journal_info, list_trans);
		blkbuf = journal_to_blkbuf(blkbuf_jinfo);
		locked = 0;

		hold_blkbuf(blkbuf);
		/**
		 * 缓冲区为脏，确实需要提交
		 */
		if (blkbuf_is_dirty(blkbuf)) {
			/**
			 * 正常情况下，锁的顺序是先锁住缓冲区再锁住日志链表
			 * 这里顺序相反，为了避免死锁，使用trylock接口
			 */
			if (blkbuf_try_lock(blkbuf)) {
				/**
				 * 无法锁住缓冲区，必须释放链表锁再重试
				 */
				smp_unlock(&journal->list_lock);
				/**
				 * 首先把已经准备好的缓冲区提交了
				 */
				submit_data_block_bulk(blkbuf_bulk, buf_count);
				buf_count = 0;
				/**
				 * 按正常顺序先锁住缓冲区再锁住链表
				 */
				blkbuf_lock(blkbuf);
				smp_lock(&journal->list_lock);
			}

			locked = 1;
		}

		/**
		 * 锁缓冲区状态位又失败
		 */
		if (!blkbuf_trylock_state(blkbuf)) {
			/**
			 * 按正常顺序再申请一次锁
			 */
			smp_unlock(&journal->list_lock);
			schedule();
			blkbuf_lock_state(blkbuf);
			smp_lock(&journal->list_lock);
		}
		/**
		 * 在休眠的过程中，块缓冲区已经从数据块链表中移除
		 */
		if (!blkbuf_is_journaled(blkbuf)
			|| blkbuf_jinfo->transaction != trans
			|| blkbuf_jinfo->which_list != TRANS_QUEUE_DIRTY_DATA) {
			blkbuf_unlock_state(blkbuf);
			if (locked)
				blkbuf_unlock(blkbuf);
			loosen_blkbuf(blkbuf);
			continue;
		}

		/**
		 * 如果块缓冲区为脏，并且由当前过程清除了脏标志
		 * 那么就需要当前进程将其回写
		 */
		if (locked && blkbuf_test_clear_dirty(blkbuf)) {
			/**
			 * 写入到临时数组中，待提交
			 */
			blkbuf_bulk[buf_count] = blkbuf;
			buf_count++;
			/**
			 * 写入锁定链表，等待回写
			 */
			__journal_putin_list(blkbuf_jinfo, trans, TRANS_QUEUE_LOCKED_DATA);
			blkbuf_unlock_state(blkbuf);

			/**
			 * 临时缓冲区满，应当批量提交一次IO请求
			 */
			if (buf_count == journal->tags_in_block) {
				smp_unlock(&journal->list_lock);
				submit_data_block_bulk(blkbuf_bulk, buf_count);
				buf_count = 0;
				smp_lock(&journal->list_lock);
				continue;
			}
		/**
		 * 本流程没有锁定任务，但是其他流程锁定了待回写
		 */
		} else if (!locked && blkbuf_is_locked(blkbuf)) {
			/**
			 * 写入锁定链表，等待回写完成
			 */
			__journal_putin_list(blkbuf_jinfo, trans, TRANS_QUEUE_LOCKED_DATA);
			blkbuf_unlock_state(blkbuf);
			loosen_blkbuf(blkbuf);
		} else {
			/**
			 * 干净缓冲区，可能是其他流程已经回写完成了
			 * 直接从链表中摘除即可
			 */
			__journal_takeout_list(blkbuf_jinfo);
			blkbuf_unlock_state(blkbuf);
			if (locked)
				blkbuf_unlock(blkbuf);
			/**
			 * 从日志模块中脱离，注意是释放了两次缓冲区引用!
			 */
			journal_info_detach(blkbuf); 
			loosen_blkbuf(blkbuf);
			loosen_blkbuf(blkbuf);
		}
	}

	smp_unlock(&journal->list_lock);

	submit_data_block_bulk(blkbuf_bulk, buf_count);
}

/**
 * 在data=ordered模式下，等待脏数据块回写完毕
 */
static int
wait_data_blocks(struct journal *journal, struct transaction *trans)
{
	struct blkbuf_journal_info *blkbuf_jinfo;
	int err = 0;

	smp_lock(&journal->list_lock);

	/**
	 * 上一步提交过程中，提交了数据块IO请求并放到此链表
	 * 此处遍历该链表，并等待这些块传输完毕
	 */
	while (!list_is_empty(&trans->locked_data_list)) {
		struct blkbuf_desc *blkbuf;

		blkbuf_jinfo = list_last_container(&trans->locked_data_list,
			struct blkbuf_journal_info, list_trans);
		blkbuf = journal_to_blkbuf(blkbuf_jinfo);
		hold_blkbuf(blkbuf);

		/**
		 * 仍然处于锁定状态，表示IO传输还未完成
		 */
		if (blkbuf_is_locked(blkbuf)) {
			smp_unlock(&journal->list_lock);
			/**
			 * 等待IO传输完毕
			 */
			blkbuf_wait_unlock(blkbuf);
			/**
			 * 如果解锁后发现数据并不是最新的，说明发生了IO传输错误
			 */
			if (unlikely(!blkbuf_is_uptodate(blkbuf)))
				err = -EIO;
			smp_lock(&journal->list_lock);
		}

		/**
		 * 获得数据块的状态锁，必要时休眠重试
		 */
		if (!blkbuf_trylock_state(blkbuf)) {
			smp_unlock(&journal->list_lock);
			schedule();
			blkbuf_lock_state(blkbuf);
			smp_lock(&journal->list_lock);
		}
		/**
		 * 在休眠期间，缓冲区所在链表位置可能已经发生了变化
		 */
		if (blkbuf_is_journaled(blkbuf)
		    && blkbuf_jinfo->which_list == TRANS_QUEUE_LOCKED_DATA) {
			/**
			 * 从链表中摘除
			 */
			__journal_takeout_list(blkbuf_jinfo);
			blkbuf_unlock_state(blkbuf);
			/**
			 * 释放描述符
			 */
			journal_info_detach(blkbuf);
			/**
			 * 与journal_info_detach对应
			 */
			loosen_blkbuf(blkbuf);
		} else
			blkbuf_unlock_state(blkbuf);

		/**
		 * 与本函数前面的调用对应
		 */
		loosen_blkbuf(blkbuf);
	}

	smp_unlock(&journal->list_lock);

	return err;
}

/**
 * 当元数据块被写入日志后的回调
 */
static void metadata_finish_journal(struct blkbuf_desc *blkbuf, int uptodate)
{
	if (uptodate)
		blkbuf_set_uptodate(blkbuf);
	else
		blkbuf_clear_uptodate(blkbuf);

	blkbuf_unlock(blkbuf);
}

/**
 * 一次性向块设备层提交元数据块请求
 * !第一个块是日志描述符块!
 */
static void submit_metadata_bulk(struct blkbuf_desc **blkbuf_bulk, int buf_count)
{
	int i;

	for (i = 0; i < buf_count; i++) {
		struct blkbuf_desc *blkbuf = blkbuf_bulk[i];

		blkbuf_lock(blkbuf);
		blkbuf_clear_dirty(blkbuf);
		blkbuf_set_uptodate(blkbuf);
		blkbuf->finish_io = metadata_finish_journal;
		submit_block_request(WRITE, blkbuf);
	}
}

/**
 * 提交本事务的元数据块
 */
static void
submit_metadata(struct journal *journal, struct transaction *trans)
{
	struct blkbuf_desc *blkbuf_orig, *blkbuf_log;
	struct blkbuf_journal_info *blkbuf_jinfo_new;
	struct blkbuf_journal_info *ctrlblk_jinfo;
	struct blkbuf_journal_info *blkbuf_jinfo;
	struct blkbuf_desc *blkbuf_bulk[64];
	struct journal_block_tag *tag = NULL;
	struct journal_header *header;
	unsigned long block_num;
	char *tagp = NULL;
	int space_left = 0;
	int first_tag = 0;
	int buf_count;
	int tag_flag;
	int flags;
	int err = 0;

	/**
	 * 所有数据块写入磁盘后，开始写入元数据块到日志中
	 * 首先标记事务状态
	 */
	trans->state = TRANS_COMMIT_METADATA;

	ctrlblk_jinfo = NULL;
	buf_count = 0;
	/**
	 * 遍历元数据链表
	 */
	while (!list_is_empty(&trans->metadata_list)) {
		blkbuf_jinfo = list_first_container(&trans->metadata_list,
			struct blkbuf_journal_info, list_trans);

		/**
		 * 特殊情况，日志被异常中止，忽略事务中的元数据。
		 */
		if (journal_is_aborted(journal)) {
			/**
			 * 移交给下一个事务处理，如果有的话
			 */
			journal_putin_next_list(journal, blkbuf_jinfo);
			/**
			 * 如果是最后一条元数据
			 * 那么需要提交请求以释放分配的控制块缓冲区
			 */
			if (list_is_empty(&trans->metadata_list)) {
				submit_metadata_bulk(blkbuf_bulk, buf_count);
				ctrlblk_jinfo = NULL;
				buf_count = 0;
			}

			continue;
		}

		/**
		 * 目前还没有日志描述符块。
		 */
		if (!ctrlblk_jinfo) {
			struct blkbuf_desc *blkbuf_ctrl;

			ASSERT(buf_count == 0);

			/**
			 * 分配一个日志控制块
			 */
			ctrlblk_jinfo = journal_alloc_block(journal);
			/**
			 * 无法分配控制块描述符，日志过程无法继续
			 */
			if (!ctrlblk_jinfo) {
				__journal_abort_hard(journal);
				continue;
			}

			blkbuf_ctrl = journal_to_blkbuf(ctrlblk_jinfo);
			header = (struct journal_header *)&blkbuf_ctrl->block_data[0];
			header->magic = cpu_to_be32(JFS_MAGIC_NUMBER);
			header->type = cpu_to_be32(JFS_DESCRIPTOR_BLOCK);
			header->trans_id = cpu_to_be32(trans->trans_id);

			tagp = &blkbuf_ctrl->block_data[sizeof(struct journal_header)];
			space_left = blkbuf_ctrl->size - sizeof(struct journal_header);
			first_tag = 1;
			blkbuf_set_write_journal(blkbuf_ctrl);
			blkbuf_set_dirty(blkbuf_ctrl);
			/**
			 * 注意
			 * 这里将描述符块加到临时数组中
			 * 该描述符块位于元数据块之前
			 */
			blkbuf_bulk[buf_count] = blkbuf_ctrl;
			buf_count++;

			/**
			 * 前面将撤销块写到控制块链表
			 * 这里将元数据控制块加入控制块链表
			 */
			journal_putin_list(ctrlblk_jinfo, trans, TRANS_QUEUE_CTRLDATA);
		}

		/**
		 * 计算元数据块应当放到哪一个日志块中。
		 */
		err = journal_next_log_block(journal, &block_num);
		/**
		 * 如果日志空间出现问题，那么继续处理下一个元数据块
		 * 并进入异常流程，这会消耗掉链表中所有元数据块
		 */
		if (err) {
			__journal_abort_hard(journal);
			continue;
		}

		blkbuf_orig = journal_to_blkbuf(blkbuf_jinfo);
		/**
		 * 递减事务可用块数量，计算日志空间时需要
		 */
		trans->reserved_credits--;
		accurate_inc(&blkbuf_orig->ref_count);
		atomic_set_bit(BS_WRITE_JOURNAL, &blkbuf_orig->state);

		/**
		 * 准备元数据到日志缓冲区中。
		 */
		flags = journal_prepare_metadata_block(trans,
			blkbuf_jinfo, &blkbuf_jinfo_new, block_num);
		blkbuf_log = journal_to_blkbuf(blkbuf_jinfo_new);
		atomic_set_bit(BS_WRITE_JOURNAL, &blkbuf_log->state);
		/**
		 * 这是需要提交到日志中的元数据，可能经过了转义
		 */
		blkbuf_bulk[buf_count] = blkbuf_log;
		buf_count++;

		tag_flag = 0;
		if (flags & 1)
			tag_flag |= JTAG_FLAG_ESCAPE;
		if (!first_tag)
			tag_flag |= JTAG_FLAG_SAME_UUID;

		/**
		 * 构建描述符块中的元数据标签
		 */
		tag = (struct journal_block_tag *)tagp;
		tag->target_block_num = cpu_to_be32(blkbuf_orig->block_num_dev);
		tag->flags = cpu_to_be32(tag_flag);
		tagp += sizeof(struct journal_block_tag);
		space_left -= sizeof(struct journal_block_tag);

		/**
		 * 如果是第一个标签，那么控制块里面需要为标签加上UUID
		 */
		if (first_tag) {
			memcpy (tagp, journal->uuid, 16);
			tagp += 16;
			space_left -= 16;
			first_tag = 0;
		}

		/**
		 * 如果满足如下条件之一，则提交一次
		 *	1、描述符块中包含的块过多
		 *	2、所有元数据块都已经处理完
		 *	3、剩余描述符空间已经不足容纳一个完整的标签(+16不必要)
		 */
		if (buf_count == ARRAY_SIZE(blkbuf_bulk) ||
		    list_is_empty(&trans->metadata_list) ||
		    space_left < sizeof(struct journal_block_tag) + 16) {
			tag->flags |= cpu_to_be32(JTAG_FLAG_LAST_TAG);

			/**
			 * 积累了足够多的元数据块，一次性提交
			 */
			submit_metadata_bulk(blkbuf_bulk, buf_count);
			ctrlblk_jinfo = NULL;
			buf_count = 0;
		}
	}
}

static int
wait_metadata(struct journal *journal, struct transaction *trans)
{
	struct blkbuf_journal_info *blkbuf_jinfo;
	int err = 0;

	/**
	 * 遍历链表，等待所有元数据(转义)写入到日志中
	 */
	while (!list_is_empty(&trans->meta_log_list)) {
		struct blkbuf_desc *blkbuf;

		/**
		 * 注意:这里是取尾节点
		 */
		blkbuf_jinfo = list_last_container(&trans->meta_log_list,
			struct blkbuf_journal_info, list_trans);
		blkbuf = journal_to_blkbuf(blkbuf_jinfo);

		/**
		 * 磁盘块IO处于锁定状态，说明IO还没有完成
		 */
		if (blkbuf_is_locked(blkbuf)) {
			/**
			 * 等待块IO完成
			 */
			blkbuf_wait_unlock(blkbuf);
			continue;
		}

		/**
		 * 运行到这里，说明写入操作完成
		 * 写入失败了，严重的IO错误
		 */
		if (unlikely(!blkbuf_is_uptodate(blkbuf)))
			err = -EIO;

		/**
		 * 清除此标志，表示日志写入完成
		 */
		blkbuf_clear_write_journal(blkbuf);
		/**
		 * 从日志链表中摘除
		 */
		journal_takeout_list(journal, blkbuf_jinfo);

		journal_info_loosen(blkbuf_jinfo);
		loosen_blkbuf(blkbuf);
		/**
		 * 该缓冲区仅仅被日志所使用，临时构建的
		 * 这里应该没有谁引用了
		 */
		ASSERT(accurate_read(&blkbuf->ref_count) == 0);
		free_blkbuf_desc(blkbuf);

              /**
               * 元数据日志块对应的原始元数据缓冲区
               * 清除其write标记
               */
		blkbuf_jinfo = list_last_container(&trans->meta_orig_list,
			struct blkbuf_journal_info, list_trans);
		blkbuf = journal_to_blkbuf(blkbuf_jinfo);
		atomic_clear_bit(BS_WRITE_JOURNAL, &blkbuf->state);
		ASSERT(blkbuf_is_journal_dirty(blkbuf));

		/**
		 * 将其放到Forget链表中
		 * 用于检查点处理，必须等待所有块都写入文件系统后才能结束检查点
		 */
		journal_putin_list(blkbuf_jinfo, trans, TRANS_QUEUE_FORGET);
		 /**
		  * 现在，我们已经用完了块缓冲区
		  * 可以唤醒等待写这个缓冲区的线程了
		  * 那个线程正在调用do_get_write_access以获得写权限
		  */
		wake_up_bit(&blkbuf->state, BS_WAIT_LOGGED);
		loosen_blkbuf(blkbuf);
	}

	ASSERT(list_is_empty(&trans->meta_orig_list));

	/**
	 * 等待撤销表和描述符块
	 */
	while (!list_is_empty(&trans->ctrldata_list)) {
		struct blkbuf_desc *blkbuf;

		blkbuf_jinfo = list_last_container(&trans->ctrldata_list,
			struct blkbuf_journal_info, list_trans);
		blkbuf = journal_to_blkbuf(blkbuf_jinfo);
		if (blkbuf_is_locked(blkbuf)) {
			blkbuf_wait_unlock(blkbuf);
			continue;
		}

		if (unlikely(!blkbuf_is_uptodate(blkbuf)))
			err = -EIO;

		blkbuf_clear_write_journal(blkbuf);
		journal_takeout_list(journal, blkbuf_jinfo);
		journal_info_loosen(blkbuf_jinfo);
		loosen_blkbuf(blkbuf);
	}

	return err;
}

/**
 * 完成事务的元数据、数据块、控制块回写
 * 提交最后的日志完成块
 */
static int
write_commitblock(struct journal *journal, struct transaction *trans)
{
	struct blkbuf_journal_info *ctrlblk_jinfo;
	struct journal_header *header;
	struct blkbuf_desc *blkbuf;
	int barrier_done = 0;
	int err = 0;
	int ret;

	/**
	 * 获得一个日志描述符。
	 * 该描述符标记事务已经提交。
	 */
	ctrlblk_jinfo = journal_alloc_block(journal);
	/**
	 * 如果获取不了日志块，那就只能中止日志了
	 */
	if (!ctrlblk_jinfo) {
		__journal_abort_hard(journal);
		err = -ENOSPC;
		goto out;
	}

	blkbuf = journal_to_blkbuf(ctrlblk_jinfo);
	/**
	 * 标记描述符，表示它是一个提交描述符。
	 */	
	header = (struct journal_header *)blkbuf->block_data;
	header->magic = cpu_to_be32(JFS_MAGIC_NUMBER);
	header->type = cpu_to_be32(JFS_COMMIT_BLOCK);
	header->trans_id = cpu_to_be32(trans->trans_id);

	blkbuf_set_dirty(blkbuf);
	/**
	 * 日志所在的磁盘支持IO屏障
	 */
	if (journal->flags & JSTATE_BARRIER) {
		/**
		 * 必须标记本次IO的屏障属性
		 * 防止与前后的操作乱序
		 */
		blkbuf_set_ordered(blkbuf);
		barrier_done = 1;
	}
	/**
	 * 将提交描述符写入到日志中。
	 */
	ret = sync_dirty_block(blkbuf);

	/**
	 * EOPNOTSUPP表示设备不支持屏障操作
	 * 这时，我们也没有办法
	 * 另外一种可能性，是它本身就不乱序
	 */
	if (ret == -EOPNOTSUPP && barrier_done) {
		/**
		 * 设备不支持，去除此标志
		 * 自求多福吧，我们暂且认为设备不会乱序
		 * 实际上，目前很少有设备会乱序
		 */
		smp_lock(&journal->state_lock);
		journal->flags &= ~JSTATE_BARRIER;
		smp_unlock(&journal->state_lock);

		/**
		 * 清除标记后再次提交
		 */
		blkbuf_clear_ordered(blkbuf);
		blkbuf_set_uptodate(blkbuf);
		blkbuf_set_dirty(blkbuf);
		ret = sync_dirty_block(blkbuf);
	}
	if (unlikely(ret == -EIO))
		err = -EIO;

	loosen_blkbuf(blkbuf);
	journal_info_loosen(ctrlblk_jinfo);

	/**
	 * 提交块已经写入完毕，现在可以进行检查点处理了。
	 */
out:
	return err;
}

/*
 * When an ext3-ordered file is truncated, it is possible that many pages are
 * not sucessfully freed, because they are attached to a committing transaction.
 * After the transaction commits, these pages are left on the LRU, with no
 * ->space, and with attached buffers.  These pages are trivially reclaimable
 * by the VM, but their apparent absence upsets the VM accounting, and it makes
 * the numbers in /proc/meminfo look odd.
 *
 * So here, we have a buffer which has just come off the forget list.  Look to
 * see if we can strip all buffers from the backing page.
 *
 * Called under lock_journal(), and possibly under journal_datalist_lock.  The
 * caller provided us with a ref against the buffer, and we drop that here.
 */
static void release_buffer_page(struct blkbuf_desc *blkbuf)
{
	struct page_frame *page;

	if (blkbuf_is_dirty(blkbuf))
		goto nope;
	if (accurate_read(&blkbuf->ref_count) != 1)
		goto nope;
	page = blkbuf->page;
	if (!page)
		goto nope;
	if (page->cache_space)
		goto nope;

	/* OK, it's a truncated page */
	if (pgflag_test_and_set_locked(page))
		goto nope;

	page_cache_hold(page);
	loosen_blkbuf(blkbuf);
	try_to_free_blocks(page);
	unlock_page(page);
	loosen_page_cache(page);
	return;

nope:
	loosen_blkbuf(blkbuf);
}

/**
 * 提交事务完成后，做一些收尾工作
 */
static void
commit_tail(struct journal *journal, struct transaction *trans)
{
	struct blkbuf_journal_info *blkbuf_jinfo;
	struct transaction *checkpoint_trans;
	struct blkbuf_desc *blkbuf;

	/**
	 * 一旦事务完成，该链表中的缓冲区即可以放弃
	 */
	while (!list_is_empty(&trans->forget_list)) {
		blkbuf_jinfo = list_first_container(&trans->forget_list,
			struct blkbuf_journal_info, list_trans);
		blkbuf = journal_to_blkbuf(blkbuf_jinfo);
		blkbuf_lock_state(blkbuf);
		ASSERT(blkbuf_jinfo->transaction == journal->running_transaction
			|| blkbuf_jinfo->transaction == trans);

		/**
		 * 释放其持有的备份数据。
		 */
		if (blkbuf_jinfo->undo_copy) {
			kfree(blkbuf_jinfo->undo_copy);
			blkbuf_jinfo->undo_copy = NULL;
			if (blkbuf_jinfo->bufdata_copy) {
				blkbuf_jinfo->undo_copy = blkbuf_jinfo->bufdata_copy;
				blkbuf_jinfo->bufdata_copy = NULL;
			}
		} else if (blkbuf_jinfo->bufdata_copy) {
			kfree(blkbuf_jinfo->bufdata_copy);
			blkbuf_jinfo->bufdata_copy = NULL;
		}

		smp_lock(&journal->list_lock);
		/**
		 * 如果该缓冲区处于其他事务的检查点中
		 * 则需要将其摘除并移动到当前事务的检查点
		 */
		checkpoint_trans = blkbuf_jinfo->checkpoint_trans;
		if (checkpoint_trans)
			__journal_takeout_checkpoint(blkbuf_jinfo);

		/**
		 * 缓冲区在被日志记录，同时又被释放
		 * 它应当是通过journal_forget进去日志的
		 * 此时它为脏，但是不应当被回写
		 * 这不仅与性能相关，更重要的是避免别名问题
		 */
		if (blkbuf_is_freed(blkbuf)) {
			blkbuf_clear_freed(blkbuf);
			blkbuf_clear_journal_dirty(blkbuf);
		}

		/**
		 * 当缓冲区被标记为脏的时候，才重新放到检查点链表中
		 * 这样当缓冲区写入到文件系统以后，就可以执行检查点操作
		 */
		if (blkbuf_is_journal_dirty(blkbuf)) {
			/**
			 * 加入到事务的检查点链表
			 */
			__journal_insert_checkpoint(blkbuf_jinfo, trans);
			__journal_putin_next_list(blkbuf_jinfo);
			blkbuf_unlock_state(blkbuf);
		/**
		 * 缓冲区不脏，也就不用加入到检查点链表了
		 */
		} else {
			ASSERT(!blkbuf_is_dirty(blkbuf));
			ASSERT(blkbuf_jinfo->next_trans == NULL);
			/**
			 * 从现有链表中摘除，并释放
			 */
			__journal_takeout_list(blkbuf_jinfo);
			blkbuf_unlock_state(blkbuf);
			journal_info_detach(blkbuf);
			/**
			 * 与journal_info_detach匹配的loosen调用在这里
			 */
			release_buffer_page(blkbuf);
		}
		smp_unlock(&journal->list_lock);
	}

	ASSERT(trans->state == TRANS_COMMIT_METADATA);

	smp_lock(&journal->state_lock);
	smp_lock(&journal->list_lock);
	/**
	 * 标记当前事务处理完成。
	 */
	trans->state = TRANS_FINISHED;
	ASSERT(trans == journal->committing_transaction);
	/**
	 * 记录提交点。
	 */
	journal->committed_id = trans->trans_id;
	journal->committing_transaction = NULL;
	smp_unlock(&journal->state_lock);

	/**
	 * 如果事务没有需要回写的缓冲区
	 * 即所有脏元数据都已经回写到文件系统中
	 * 则可以直接丢弃事务，否则将其添加到检查点链表中
	 */
	if (list_is_empty(&trans->checkpoint_list))
		__journal_drop_transaction(journal, trans);
	else
		list_insert_behind(&trans->list_checkpoint,
			&journal->checkpoint_transactions);

	/**
	 * 注意
	 * 这里并不处理检查点
	 * 当日志没有空间，或者umount时
	 * 才会真正去处理检查点以回收日志空间
	 * 也就是说，内存中有不少磁盘块缓冲区对象
	 */
	smp_unlock(&journal->list_lock);
}

/**
 * 提交日志的主函数。
 * 由日志线程调用。
 */
void journal_commit_transaction(struct journal *journal)
{
	struct transaction *commit_transaction;
	int err = 0;

	/**
	 * 执行到这里，说明在记录新的事务日志
	 * 以前写入的超级块已经失效，需要再次写入
	 * 并等待超级块更新结束，保证前一个事务的完整性
	 */
	if (journal->flags & JSTATE_FLUSHED)
		journal_update_superblock(journal, 1);

	ASSERT(journal->running_transaction != NULL);
	ASSERT(journal->committing_transaction == NULL);
	ASSERT(journal->running_transaction->state == TRANS_RUNNING);

	/**
	 * 第一个阶段
	 * 将事务从运行状态转换为锁定状态。
	 * 这意味着事务不再接受新的原子操作。
	 * 因为日志线程可能是到期了，需要强制结束当前事务。
	 */
	commit_transaction = journal->running_transaction;
	journal_debug(1, "JBD: commit phase 1,"
			" starting commit of transaction %d\n",
			commit_transaction->trans_id);
	switch_running_trans(journal, commit_transaction);

	/**
	 * 第二阶段，将数据缓存块写入到磁盘。
	 */
	journal_debug (1, "JBD: commit phase 2, sync data blocks\n");
	submit_data_blocks(journal, commit_transaction);
	err = wait_data_blocks(journal, commit_transaction);
	if (err)
		__journal_abort_hard(journal);
	ASSERT(list_is_empty(&commit_transaction->data_block_list));

	/**
	 * 构建撤销表
	 * 会将撤销记录写到LogCtl链表中
	 */
	journal_debug(1, "JBD: commit phase 3, write revoke items\n");
	journal_revoke_write(journal, commit_transaction);

	/**
	 * 将事务元数据提交到日志中
	 */
	journal_debug(1, "JBD: commit phase 4, submit metadata\n");
	submit_metadata(journal, commit_transaction);

	/**
	 * 等待文件系统元数据和日志元数据被写入到日志中
	 */
	err = wait_metadata(journal, commit_transaction);
	if (err)
		goto abort;
	if (journal_is_aborted(journal))
		goto abort;

	/**
	 * 运行到此，所有数据块已经保存到磁盘中。
	 * 并且元数据已经保存到日志中。
	 * 可以写入提交块，标记事务结束
	 */
	journal_debug(1, "JBD: commit phase 5, write commit block\n");
	write_commitblock(journal, commit_transaction);

	/**
	 * 日志写入完毕，做一些收尾工作
	 * 例如修改日志超级块，标记检查点
	 */
	ASSERT(list_is_empty(&commit_transaction->data_block_list));
	ASSERT(list_is_empty(&commit_transaction->metadata_list));
	ASSERT(list_is_empty(&commit_transaction->checkpoint_list));
	ASSERT(list_is_empty(&commit_transaction->meta_log_list));
	ASSERT(list_is_empty(&commit_transaction->meta_orig_list));
	ASSERT(list_is_empty(&commit_transaction->ctrldata_list));

abort:
	if (err)
		__journal_abort_hard(journal);
	commit_tail(journal, commit_transaction);

	/**
	 * 唤醒线程，这些线程在等待日志处理完毕
	 */
	wake_up(&journal->wait_commit_done);
}
