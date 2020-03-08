#include <dim-sum/beehive.h>
#include <dim-sum/bug.h>
#include <dim-sum/errno.h>
#include <dim-sum/journal.h>
#include <dim-sum/init.h>

struct journal_revoke_item
{
	struct hash_list_node node;
	trans_id_t trans_id;
	unsigned long block_num;
};

static struct beehive_allotter *revoke_item_allotter;
static struct beehive_allotter *revoke_table_allotter;

static inline int __hash(struct journal *journal, unsigned long block)
{
	struct journal_revoke_table *table;
	unsigned long tmp;
	int hash_order;

	table = journal->cur_revoke_table;
	hash_order = table->hash_order;
	tmp = block << (hash_order - 6);
	tmp ^= (block >> 13);
	tmp ^= (block << (hash_order - 12));

	return tmp & (table->hash_size - 1);
}

/**
 * 在撤销表中插入一个条目
 */
static int putin_hash(struct journal *journal,
	unsigned long block_num, trans_id_t trans_id)
{
	struct journal_revoke_item *item;
	struct hash_list_bucket *hash_list;
	int bucket;

	do {
		item = beehive_alloc(revoke_item_allotter, PAF_NOFS);
		if (!item)
			continue;

		item->trans_id = trans_id;
		item->block_num = block_num;
		bucket = __hash(journal, block_num);
		hash_list = &journal->cur_revoke_table->hash_table[bucket];
		smp_lock(&journal->revoke_lock);
		hlist_add_head(&item->node, hash_list);
		smp_unlock(&journal->revoke_lock);

		return 0;
	} while (journal_oom_retry);

	return -ENOMEM;
}

/**
 * 在哈希表中查找撤销表记录
 */
static struct journal_revoke_item *
find_in_hash(struct journal *journal, unsigned long block_num)
{
	struct journal_revoke_item *item;
	struct hash_list_bucket *hash_list;
	struct hash_list_node *node;
	unsigned long bucket;

	bucket = __hash(journal, block_num);
	hash_list = &journal->cur_revoke_table->hash_table[bucket];

	smp_lock(&journal->revoke_lock);
	hlist_for_each(node, hash_list) {
		item = container_of(node, struct journal_revoke_item, node);

		if (item->block_num == block_num) {
			smp_unlock(&journal->revoke_lock);
			return item;
		}
	}
	smp_unlock(&journal->revoke_lock);

	return NULL;
}

/**
 * 清空撤销表中的所有记录
 */
void journal_clear_revoke(struct journal *journal)
{
	struct journal_revoke_table *revoke;
	struct hash_list_bucket *hash_list;
	struct journal_revoke_item *item;
	int i;

	revoke = journal->cur_revoke_table;

	for (i = 0; i < revoke->hash_size; i++) {
		hash_list = &revoke->hash_table[i];

		while (!hlist_is_empty(hash_list)) {
			item = hlist_first_container(hash_list,
				struct journal_revoke_item, node);

			hlist_del(&item->node);
			beehive_free(revoke_item_allotter, item);
		}
	}
}

/**
 * 判断一个块是否在日志的撤销块中
 */
int journal_test_revoke(struct journal *journal, 
	unsigned long block_num, trans_id_t trans_id)
{
	struct journal_revoke_item *item;

	item = find_in_hash(journal, block_num);
	if (!item)
		return 0;

	/**
	 * 撤销块对当前事务无效，以前事务中的撤销块
	 */
	if (tid_gt(trans_id, item->trans_id))
		return 0;

	return 1;
}

int journal_set_revoke(struct journal *journal, 
	unsigned long block_num, trans_id_t trans_id)
{
	struct journal_revoke_item *item;

	/**
	 * 撤销块在哈希表中存在吗? 
	 */
	item = find_in_hash(journal, block_num);
	if (item) {
		/**
		 * 新的事务序号更大吗? 
		 */
		if (tid_gt(trans_id, item->trans_id))
			item->trans_id = trans_id;

		return 0;
	} 
	/**
	 * 还不存在，直接插入到哈希表
	 */
	return putin_hash(journal, block_num, trans_id);
}

/**
 * 不再撤销块缓冲区
 * 相应的磁盘块被重新分配使用了
 */
void journal_cancel_revoke(struct journal_handle *handle,
	struct blkbuf_journal_info *blkbuf_jinfo)
{
	struct journal_revoke_item *item;
	struct blkbuf_desc *blkbuf;
	struct journal *journal;
	int need_del;

	journal = handle->transaction->journal;
	blkbuf = journal_to_blkbuf(blkbuf_jinfo);

	if (blkbuf_test_set_revokevalid(blkbuf))
		need_del = blkbuf_test_clear_revoked(blkbuf);
	else {
		need_del = 1;
		blkbuf_clear_revoked(blkbuf);
	}

	/**
	 * 需要从哈希表中查找并删除
	 */
	if (need_del) {
		struct blkbuf_desc *alias;

		item = find_in_hash(journal, blkbuf->block_num_dev);
		if (item) {
			smp_lock(&journal->revoke_lock);
			hlist_del_init(&item->node);
			smp_unlock(&journal->revoke_lock);
			beehive_free(revoke_item_allotter, item);
		}

		/**
		 * 查找缓冲区别名，将它的撤销标志也删除
		 */
		alias = __blkbuf_find_block(blkbuf->blkdev,
			blkbuf->block_num_dev, blkbuf->size);
		if (alias) {
			if (alias != blkbuf)
				blkbuf_clear_revoked(alias);
			loosen_blkbuf(alias);
		}
	}
}

/**
 * 当切换事务时，切换撤销表
 */
void journal_switch_revoke_table(struct journal *journal)
{
	int i;

	if (journal->cur_revoke_table == journal->revoke_tables[0])
		journal->cur_revoke_table = journal->revoke_tables[1];
	else
		journal->cur_revoke_table = journal->revoke_tables[0];

	for (i = 0; i < journal->cur_revoke_table->hash_size; i++) 
		hash_list_init_bucket(&journal->cur_revoke_table->hash_table[i]);
}

/**
 * 提交撤销块的写请求
 */
static void submit_revoke_block(struct journal *journal, 
	struct blkbuf_journal_info *blkbuf_jinfo, int offset)
{
	struct journal_revoke_header *header;
	struct blkbuf_desc *blkbuf;

	blkbuf = journal_to_blkbuf(blkbuf_jinfo);
	if (journal_is_aborted(journal)) {
		loosen_blkbuf(blkbuf);
		return;
	}

	header = (struct journal_revoke_header *)blkbuf->block_data;
	header->size = cpu_to_be32(offset);
	/**
	 * jwrite标志表示写入日志块，日志提交过程需要等待这样的块完成
	 */
	blkbuf_set_write_journal(blkbuf);
	blkbuf_set_dirty(blkbuf);
	submit_block_requests(WRITE, 1, &blkbuf);
}


/**
 * 向日志中写一条撤销记录
 * 如果块缓冲区已经满了，就提交块请求
 * 如果还没有撤销块缓冲区，就建立一个
 */
static void record_item(struct journal *journal,
	struct transaction *transaction,
	struct blkbuf_journal_info **pblkbuf_jinfo,
	int *poffset, struct journal_revoke_item *item)
{
	struct blkbuf_journal_info *blkbuf_jinfo;
	struct journal_header *header;
	struct blkbuf_desc *blkbuf;
	int offset;

	/**
	 * 如果日志被中止，那么不向磁盘写
	 * 但是上层遍历哈希表的过程仍然需要
	 * 因为需要释放撤销记录
	 */
	if (journal_is_aborted(journal))
		return;

	blkbuf_jinfo = *pblkbuf_jinfo;
	offset = *poffset;

	/**
	 * 如果撤销块已经满了，就提交
	 */
	if (blkbuf_jinfo) {
		if (offset == journal->block_size) {
			submit_revoke_block(journal, blkbuf_jinfo, offset);
			blkbuf_jinfo = NULL;
		}
	}

	if (!blkbuf_jinfo) {
		blkbuf_jinfo = journal_alloc_block(journal);
		if (!blkbuf_jinfo)
			return;

		blkbuf = journal_to_blkbuf(blkbuf_jinfo);
		header = (struct journal_header *)&blkbuf->block_data[0];
		header->magic = cpu_to_be32(JFS_MAGIC_NUMBER);
		header->type = cpu_to_be32(JFS_REVOKE_BLOCK);
		header->trans_id = cpu_to_be32(transaction->trans_id);

		/**
		 * 加入日志控制块链表
		 */
		journal_putin_list(blkbuf_jinfo, transaction, TRANS_QUEUE_CTRLDATA);

		offset = sizeof(struct journal_revoke_header);
		*pblkbuf_jinfo = blkbuf_jinfo;
	}

	/**
	 * 向控制块缓冲区内写入撤销块号
	 * 并移动位置指针
	 */
	* ((__be32 *)(&journal_to_blkbuf(blkbuf_jinfo)->block_data[offset])) =
		cpu_to_be32(item->block_num);
	offset += 4;
	*poffset = offset;
}

/**
 * 向日志中写入所有撤销记录
 */
void journal_revoke_write(struct journal *journal,
	struct transaction *transaction)
{
	struct blkbuf_journal_info *blkbuf_jinfo;
	struct journal_revoke_table *revoke;
	struct hash_list_bucket *hash_list;
	struct journal_revoke_item *item;
	int i, offset;

	blkbuf_jinfo = NULL; 
	offset = 0;

	/**
	 * 备份撤销表才是我们需要写入的
	 */
	if (journal->cur_revoke_table == journal->revoke_tables[0])
		revoke = journal->revoke_tables[1];
	else
		revoke = journal->revoke_tables[0];

	/**
	 * 遍历撤销表中的所有哈希桶
	 */
	for (i = 0; i < revoke->hash_size; i++) {
		hash_list = &revoke->hash_table[i];

		while (!hlist_is_empty(hash_list)) {
			item = hlist_first_container(hash_list,
				struct journal_revoke_item, node);

			record_item(journal, transaction, &blkbuf_jinfo, &offset, item);
			hlist_del(&item->node);
			beehive_free(revoke_item_allotter, item);
		}
	}

	if (blkbuf_jinfo)
		submit_revoke_block(journal, blkbuf_jinfo, offset);
}

/** 
 * 从日志中撤销一个特定的元数据块
 */
int journal_revoke(struct journal_handle *handle, unsigned long block_num,
	struct blkbuf_desc *blkbuf_orig)
{
	struct blkbuf_desc *blkbuf = NULL;
	struct block_device *blkdev;
	struct journal *journal;
	int err;

	might_sleep();

	/**
	 * 如果mount时不支持撤销表，则直接退出
	 */
	journal = handle->transaction->journal;
	if (!journal_set_features(journal, 0, 0, JFS_FEATURE_INCOMPAT_REVOKE))
		return -EINVAL;

	blkdev = journal->fs_blkdev;
	blkbuf = blkbuf_orig;
	if (!blkbuf)
		/**
		 * 在内存中查找对应的缓冲区
		 */
		blkbuf = __blkbuf_find_block(blkdev, block_num, journal->block_size);

	/**
	 * 块缓冲区不存在的情况下，就必然没有在日志中
	 * 否则需要在日志中做一些处理
	 */
	if (blkbuf) {
		if (blkbuf_is_revoked(blkbuf)) {
			if (!blkbuf_orig)
				loosen_blkbuf(blkbuf);
			return -EIO;
		}
		/**
		 * 第一次设置撤销标志
		 */
		blkbuf_set_revoked(blkbuf);
		blkbuf_set_revokevalid(blkbuf);
		/**
		 * 加入到forget表中
		 */
		if (blkbuf_orig)
			journal_forget(handle, blkbuf_orig);
		else
			loosen_blkbuf(blkbuf);
	}

	/**
	 * 将块加入到撤销表中
	 */
	err = putin_hash(journal, block_num,
		handle->transaction->trans_id);

	return err;
}

/**
 * 在分配新的日志时，初始化其撤销表
 */
int journal_init_revoke(struct journal *journal, int size)
{
	int order, tmp;

	ASSERT(journal->revoke_tables[0] == NULL);
	ASSERT((size & (size - 1)) == 0);

	order = 0;
	tmp = size;
	while((tmp >>= 1UL) != 0UL)
		order++;

	journal->revoke_tables[0] =
		beehive_alloc(revoke_table_allotter, PAF_KERNEL);
	if (!journal->revoke_tables[0])
		return -ENOMEM;
	journal->cur_revoke_table = journal->revoke_tables[0];
	journal->cur_revoke_table->hash_size = size;
	journal->cur_revoke_table->hash_order = order;

	journal->cur_revoke_table->hash_table =
		kmalloc(size * sizeof(struct double_list), PAF_KERNEL);
	if (!journal->cur_revoke_table->hash_table) {
		beehive_free(revoke_table_allotter, journal->revoke_tables[0]);
		journal->cur_revoke_table = NULL;
		return -ENOMEM;
	}

	for (tmp = 0; tmp < size; tmp++)
		hash_list_init_bucket(&journal->cur_revoke_table->hash_table[tmp]);

	journal->revoke_tables[1] =
		beehive_alloc(revoke_table_allotter, PAF_KERNEL);
	if (!journal->revoke_tables[1]) {
		kfree(journal->revoke_tables[0]->hash_table);
		beehive_free(revoke_table_allotter, journal->revoke_tables[0]);
		return -ENOMEM;
	}

	journal->cur_revoke_table = journal->revoke_tables[1];
	journal->cur_revoke_table->hash_size = size;
	journal->cur_revoke_table->hash_order = order;
	journal->cur_revoke_table->hash_table =
		kmalloc(size * sizeof(struct double_list), PAF_KERNEL);
	if (!journal->cur_revoke_table->hash_table) {
		kfree(journal->revoke_tables[0]->hash_table);
		beehive_free(revoke_table_allotter, journal->revoke_tables[0]);
		beehive_free(revoke_table_allotter, journal->revoke_tables[1]);
		journal->cur_revoke_table = NULL;
		return -ENOMEM;
	}

	for (tmp = 0; tmp < size; tmp++)
		hash_list_init_bucket(&journal->cur_revoke_table->hash_table[tmp]);

	smp_lock_init(&journal->revoke_lock);

	return 0;
}

int __init init_journal_revoke(void)
{
	revoke_item_allotter = beehive_create("revoke item",
		sizeof(struct journal_revoke_item), 0, BEEHIVE_HWCACHE_ALIGN, NULL);
	if (revoke_item_allotter == NULL)
		return -ENOMEM;

	revoke_table_allotter = beehive_create("revoke table",
		sizeof(struct journal_revoke_table), 0, 0, NULL);
	if (revoke_table_allotter == NULL) {
		beehive_destroy(revoke_item_allotter);
		revoke_item_allotter = NULL;
		return -ENOMEM;
	}

	return 0;
}
