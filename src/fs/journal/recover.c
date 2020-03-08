#include <dim-sum/beehive.h>
#include <dim-sum/block_buf.h>
#include <dim-sum/bug.h>
#include <dim-sum/errno.h>
#include <dim-sum/journal.h>

/**
 * 最大的预读块缓冲区数量
 */
#define RA_MAXBUF 8
/**
 * PASS_SCAN:
 *	查找到日志末端。
 * PASS_REVOKE:
 *	查找日志中所有撤销块。
 * PASS_REPLAY:
 *	将所有未被撤销的块写入到磁盘，以确保一致性。
 */
enum pass_type {
	PASS_SCAN,
	PASS_REVOKE,
	PASS_REPLAY
};

/**
 * 恢复日志所用信息
 */
struct recovery_info 
{
	/**
	 * 起止事务号
	 */
	trans_id_t start_transaction;
	trans_id_t end_transaction;
	int replay_count;
	int revoke_count;
	int revoke_hits;
};

/**
 * 向前移动日志块号，如果达到尾部则回绕
 */
static inline void advance_journal_block(struct journal *journal,
	unsigned long *pblock, int count)
{
	unsigned long block = *pblock + count;

	block = *pblock + count;
	if (block>= journal->last_block_log)
		block -= journal->last_block_log - journal->first_block_log;

	*pblock = block;
}

/**
 * 批量解除块缓冲区的引用
 */
void journal_loosen_blkbuf_bulk(struct blkbuf_desc *bufs[], int count)
{
	int i;

	for (i = 0; i < count; i++)
		loosen_blkbuf (bufs[i]);
}

/**
 * 预读日志块
 */
static int do_readahead(struct journal *journal, unsigned int start)
{
	struct blkbuf_desc *bufs[RA_MAXBUF];
	unsigned int end, buf_count, block;
	struct blkbuf_desc *blkbuf;
	unsigned long block_num;
	int err;

	/**
	 * 向后预读256K数据，确保块号不越界
	 */
	end = start + (256 * 1024 / journal->block_size);
	if (end > journal->block_end)
		end = journal->block_end;

	buf_count = 0;
	for (block = start; block < end; block++) {
		err = journal_map_block(journal, block, &block_num);
		if (err) {
			printk (KERN_ERR "JBD: bad block at offset %u\n", block);
			goto out;
		}

		blkbuf = __blkbuf_find_alloc(journal->blkdev, block_num, journal->block_size);
		if (!blkbuf) {
			err = -ENOMEM;
			goto out;
		}

		/**
		 * 需要从磁盘读数据
		 */
		if (!blkbuf_is_uptodate(blkbuf) && !blkbuf_is_locked(blkbuf)) {
			bufs[buf_count] = blkbuf;
			buf_count++;
			/**
			 * 缓冲区较多，提交读请求
			 */
			if (buf_count == RA_MAXBUF) {
				submit_block_requests(READ, buf_count, bufs);
				journal_loosen_blkbuf_bulk(bufs, buf_count);
				buf_count = 0;
			}
		} else
			loosen_blkbuf(blkbuf);
	}

	if (buf_count)
		submit_block_requests(READ, buf_count, bufs);
	err = 0;
out:
	if (buf_count) 
		journal_loosen_blkbuf_bulk(bufs, buf_count);

	return err;
}

/**
 * 从日志中读取块的内容
 */
static int read_journal_block(struct blkbuf_desc **pblkbuf,
	struct journal *journal, unsigned int block)
{
	struct blkbuf_desc *blkbuf;
	unsigned long block_num;
	int err;

	*pblkbuf = NULL;

	/**
	 * 逻辑块超出日志边界
	 */
	if (block >= journal->block_end) {
		printk(KERN_ERR "JBD: corrupted journal super_block\n");
		return -EIO;
	}

	/**
	 * 得到日志块在磁盘上的块号
	 * 对于块设备日志来说，逻辑块号与物理块号是一致的
	 */
	err = journal_map_block(journal, block, &block_num);
	if (err) {
		printk (KERN_ERR "JBD: bad block at offset %u\n", block);
		return err;
	}

	/**
	 * 查找磁盘块缓冲区，如果没有就创建一个
	 */
	blkbuf = __blkbuf_find_alloc(journal->blkdev, block_num, journal->block_size);
	if (!blkbuf)
		return -ENOMEM;

	/**
	 * 如果缓冲区中的数据不是最新的
	 * 并且还没有提交IO请求，就进行预读
	 */
	if (!blkbuf_is_uptodate(blkbuf)) {
		if (!blkbuf_is_requested(blkbuf))
			do_readahead(journal, block);
		/**
		 * 等待读操作完成
		 */
		blkbuf_wait_unlock(blkbuf);
	}

	/**
	 * 如果仍然不是最新的，说明发生了IO错误
	 */
	if (!blkbuf_is_uptodate(blkbuf)) {
		printk (KERN_ERR "JBD: Failed to read block at offset %u\n",
			block);
		loosen_blkbuf(blkbuf);
		return -EIO;
	}

	*pblkbuf = blkbuf;
	return 0;
}

/**
 * 统计一个描述符块里面有多少个描述符
 */
static int count_tags(struct blkbuf_desc *blkbuf, int size)
{
	struct journal_block_tag *tag;
	int pos, ret = 0;

	pos = sizeof(struct journal_header);
	while ((pos + sizeof(struct journal_block_tag)) <= size) {
		tag = (struct journal_block_tag *)&blkbuf->block_data[pos];
		pos += sizeof(struct journal_block_tag);
		/**
		 * 有UUID标记，后移16字节
		 */
		if (!(tag->flags & cpu_to_be32(JTAG_FLAG_SAME_UUID)))
			pos += 16;

		ret++;
		if (tag->flags & cpu_to_be32(JTAG_FLAG_LAST_TAG))
			break;
	}

	return ret;
}

/**
 * 将日志回放到文件系统磁盘中
 */
static int do_replay(struct journal *journal, struct blkbuf_desc *blkbuf,
	unsigned long *pnext_block, struct recovery_info *info,
	unsigned int next_trans_id)
{
	struct blkbuf_desc *blkbuf_new;
	struct blkbuf_desc *blkbuf_old;
	struct journal_block_tag *tag;
	int err = 0;
	int flags;
	int pos;

	/**
	 * 这里开始执行回放操作
	 * 先读出日志块的头
	 */
	pos = sizeof(struct journal_header);
	/**
	 * 遍历一个整块，找描述符块的所有tag
	 */
	while ((pos + sizeof(struct journal_block_tag)) <= journal->block_size) {
		unsigned long data_block;

		tag = (struct journal_block_tag *)&blkbuf->block_data[pos];
		flags = be32_to_cpu(tag->flags);

		/**
		 * 获得tag对应的数据块号，然后后移块号
		 */
		data_block = *pnext_block;
		advance_journal_block(journal, pnext_block, 1);
		/**
		 * 将要恢复的数据块原始数据读到内存中
		 */
		err = read_journal_block(&blkbuf_old, journal, data_block);
		if (err)
			printk (KERN_ERR "JBD: IO error %d recovering "
				"block %ld in log\n", err, data_block);
		else {
			unsigned long block_num;

			ASSERT(blkbuf_old != NULL);
			/**
			 * 在目标文件系统中的磁盘块号
			 */
			block_num = be32_to_cpu(tag->target_block_num);

			/**
			 * 要恢复的块在撤销块哈希表中
			 * 不用恢复，略过
			 */
			if (journal_test_revoke(journal, block_num, next_trans_id)) {
				loosen_blkbuf(blkbuf_old);
				info->revoke_hits++;
				goto advance;
			}

			/**
			 * 查找目标块缓存，如果没有就分配一个缓存块
			 */
			blkbuf_new = __blkbuf_find_alloc(journal->fs_blkdev,
				block_num, journal->block_size);
			if (blkbuf_new == NULL) {
				printk(KERN_ERR "JBD: Out of memory during recovery.\n");
				loosen_blkbuf(blkbuf);
				loosen_blkbuf(blkbuf_old);
				return -ENOMEM;
			}

			/**
			 * 锁定目标块，并将待恢复数据复制过去
			 * 防止与回写过程冲突
			 */
			blkbuf_lock(blkbuf_new);
			memcpy(blkbuf_new->block_data, blkbuf_old->block_data,
				journal->block_size);
			/**
			 * 数据块前4个字节正好与魔法数相等
			 * 因此日志中的数据被转义了
			 */
			if (flags & JTAG_FLAG_ESCAPE)
				*((__be32 *)blkbuf->block_data) =
					cpu_to_be32(JFS_MAGIC_NUMBER);

			/**
			 * 这里仅仅是标记脏，以后统一回写磁盘
			 */
			blkbuf_set_uptodate(blkbuf_new);
			blkbuf_mark_dirty(blkbuf_new);
			info->replay_count++;

			blkbuf_unlock(blkbuf_new);
			loosen_blkbuf(blkbuf_old);
			loosen_blkbuf(blkbuf_new);
		}

advance:
		/**
		 * 移动tag指针到下一个tag
		 */
		pos += sizeof(struct journal_block_tag);
		if (!(flags & JTAG_FLAG_SAME_UUID))
			pos += 16;

		if (flags & JTAG_FLAG_LAST_TAG)
			break;
	}

	loosen_blkbuf(blkbuf);

	return err;
}

/**
 * 扫描撤销块中的数据，将其记录到内存哈希表中
 */
static int
scan_revoke_records(struct journal *journal, struct blkbuf_desc *blkbuf,
	trans_id_t trans_id, struct recovery_info *info)
{
	struct journal_revoke_header *header;
	int offset, size;

	header = (struct journal_revoke_header *)blkbuf->block_data;
	offset = sizeof(struct journal_revoke_header);
	size = be32_to_cpu(header->size);

	WARN_ON(size > journal->block_size, "bad revoke header size");
	/**
	 * 遍历磁盘上所有字节
	 */
	while (offset < size) {
		unsigned long block;
		int err;

		block = be32_to_cpu(*((__be32 *) (blkbuf->block_data + offset)));
		offset += 4;
		/**
		 * 记录撤销块信息到内存中
		 */
		err = journal_set_revoke(journal, block, trans_id);
		if (err)
			return err;

		info->revoke_count++;
	}
	return 0;
}

static int
scan_journal(struct journal *journal, struct recovery_info *info,
	enum pass_type pass)
{
	struct journal_super_phy *super;
	struct journal_header *header;
	unsigned int first_id, next_id;
	struct blkbuf_desc *blkbuf;
	unsigned long next_block;
	unsigned int trans_id;
	int header_type;
	int err;

	/**
	 * 从超级块中读取事务编号
	 * 才及事务块结束编号
	 */
	super = journal->super_block;
	next_id = be32_to_cpu(super->trans_id);
	next_block = be32_to_cpu(super->inuse_block_first);
	first_id = next_id;

	if (pass == PASS_SCAN)
		info->start_transaction = first_id;

	/**
	 * 遍历所有块，依次处理每个事务
	 * 确保事务的完整性
	 */
	while (1) {
		/**
		 * 不是第一遍遍历，因此我们已经知道最后一个事务号
		 * 如果已经是最后一个事务就退出
		 */
		if (pass != PASS_SCAN)
			if (tid_geq(next_id, info->end_transaction))
				break;

		/**
		 * 从日志块设备中读取当前块内容到内存中
		 * 该块应当是日志块头
		 */
		err = read_journal_block(&blkbuf, journal, next_block);
		if (err)
			goto fail;

		/**
		 * 移动到下一个逻辑块，应当指向日志数据块
		 */
		advance_journal_block(journal, &next_block, 1);

		header = (struct journal_header *)blkbuf->block_data;
		/**
		 * 不是日志描述块
		 * 注意在提交日志时，元数据块是转义了的
		 */
		if (header->magic != cpu_to_be32(JFS_MAGIC_NUMBER)) {
			loosen_blkbuf(blkbuf);
			break;
		}

		/**
		 * 描述块类型及事务序号
		 */
		header_type = be32_to_cpu(header->type);
		trans_id = be32_to_cpu(header->trans_id);

		/**
		 * 和预期序号不符，退
		 */
		if (trans_id != next_id) {
			loosen_blkbuf(blkbuf);
			break;
		}

		/**
		 * 有效的日志块头
		 */
		switch(header_type) {
		/**
		 * 描述符块，后跟元数据
		 */
		case JFS_DESCRIPTOR_BLOCK:
			if (pass != PASS_REPLAY) {
				/**
				 * 不是回放阶段，直接计算数据块有多少
				 * 并跳过后面的数据块
				 */
				advance_journal_block(journal, &next_block,
					count_tags(blkbuf, journal->block_size));
				loosen_blkbuf(blkbuf);
				continue;
			}

			/**
			 * 执行回放过程，恢复日志数据
			 */
			err = do_replay(journal, blkbuf, &next_block, info, next_id);
			if (err)
				goto fail;

			continue;

		/**
		 * 提交块，应当开启下一个事务
		 */
		case JFS_COMMIT_BLOCK:
			loosen_blkbuf(blkbuf);
			next_id++;
			continue;

		/**
		 * 撤销块
		 */
		case JFS_REVOKE_BLOCK: 
			if (pass != PASS_REVOKE) { 
				loosen_blkbuf(blkbuf);
				continue;
			}

			/**
			 * 第二步
			 * 将撤销块记录到内存中
			 */
			err = scan_revoke_records(journal, blkbuf,
						  next_id, info);
			loosen_blkbuf(blkbuf);
			if (err)
				goto fail;

			continue;

		/**
		 * 撞上鬼了
		 */
		default:
			goto done;
		}
	}

 done:
	/**
	 * 第一遍遍历
	 * 记录下起止事务号
	 */
	if (pass == PASS_SCAN)
		info->end_transaction = next_id;
	else {
		if (info->end_transaction != next_id)
			printk (KERN_ERR "JBD: recovery pass %d ended at "
				"transaction %u, expected %u\n",
				pass, next_id, info->end_transaction);
	}

	return 0;

 fail:
	return err;
}

/**
 * 开始日志，并且清除并忽略现有的日志记录
 */
int journal_recovery_ignore(struct journal *journal)
{
	struct recovery_info info;
	int err;

	memset (&info, 0, sizeof(info));

	/**
	 * 执行一次扫描过程，得到最后一个事务编号
	 */
	err = scan_journal(journal, &info, PASS_SCAN);
	if (err) {
		printk(KERN_ERR "JBD: error %d scanning journal\n", err);
		journal->next_trans++;
	} else
		journal->next_trans = info.end_transaction + 1;

	journal->inuse_block_first = 0;

	return err;
}

/**
 * 日志恢复主函数
 * 当装载一个文件系统时调用
 */
int journal_recover_engorge(struct journal *journal)
{
	struct journal_super_phy *super;
	struct recovery_info info;
	int err;

	memset(&info, 0, sizeof(info));
	super = journal->super_block;

	/**
	 * 文件系统被正常卸载
	 * 正常情况下日志块从1开始，不可能为0
	 */
	if (!super->inuse_block_first) {
		/**
		 * 递增日志序号，退出
		 */
		journal->next_trans = be32_to_cpu(super->trans_id) + 1;
		return 0;
	}

	/**
	 * 找到日志的终点和起点
	 */
	err = scan_journal(journal, &info, PASS_SCAN);

	/**
	 * 找到撤销块，将其信息读到哈希表中
	 */
	if (!err)
		err = scan_journal(journal, &info, PASS_REVOKE);

	/**
	 * 回放操作，恢复数据
	 */
	if (!err)
		err = scan_journal(journal, &info, PASS_REPLAY);

	/**
	 * 清空撤销表
	 * 同步文件系统磁盘块数据
	 */
	journal->next_trans = info.end_transaction + 1;
	journal_clear_revoke(journal);
	blkdev_sync(journal->fs_blkdev);

	return err;
}
