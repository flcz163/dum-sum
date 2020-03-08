#include <dim-sum/beehive.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/block_buf.h>
#include <dim-sum/fs.h>
#include <dim-sum/lext3_fs.h>

#include "internal.h"

static inline bool block_overlap(unsigned long block,
	unsigned long first, unsigned long count)
{
	return (block >= first) && (block <= first + count - 1);
}

static inline bool
verify_group_block(struct super_block *super, struct lext3_group_desc *group,
	unsigned long start, unsigned long block_count)
{
	unsigned long block, count;

	block = le32_to_cpu(group->datablock_bitmap);
	if (block_overlap (block, start, block_count))
		return false;

	block = le32_to_cpu(group->fnode_bitmap);
	if (block_overlap (block, start, block_count))
		return false;

	block = le32_to_cpu(group->first_fnode_block);
	count = super_to_lext3(super)->fnode_blocks_per_group;
	if (block_overlap (start, block, count))
		return false;

	block = le32_to_cpu(group->first_fnode_block);
	count = super_to_lext3(super)->fnode_blocks_per_group;
	if (block_overlap (start + block_count - 1, block, count))
		return false;

	return true;
}

static inline struct lext3_group_desc *
first_group_desc(struct blkbuf_desc *blkbuf)
{
	return (struct lext3_group_desc *)blkbuf->block_data; 
}

struct lext3_group_desc *lext3_get_group_desc(struct super_block *super,
	unsigned int block_group, struct blkbuf_desc **pblkbuf)
{
	struct lext3_superblock *lext3_super;
	struct blkbuf_desc *blkbuf;
	unsigned long block_index;
	unsigned long offset;

	lext3_super = super_to_lext3(super);

	/**
	 * 块组编号过大
	 */
	if (block_group >= lext3_super->groups_count) {
		lext3_enconter_error (super, "block_group >= groups_count - "
			"block_group = %d, groups_count = %lu", block_group,
			super_to_lext3(super)->groups_count);
		return NULL;
	}

	/**
	 * 计算块组号在哪个磁盘块中，以及在块中的偏移
	 */
	block_index = block_group >> lext3_super->group_desc_per_block_order;
	offset = block_group & (LEXT3_DESC_PER_BLOCK(super) - 1);
	if (!super_to_lext3(super)->blkbuf_grpdesc[block_index]) {
		lext3_enconter_error (super, "Group descriptor not loaded - "
			    "block_group = %d, group_desc = %lu, desc = %lu",
			     block_group, block_index, offset);
		return NULL;
	}

	blkbuf = lext3_super->blkbuf_grpdesc[block_index];
	if (pblkbuf)
		*pblkbuf = blkbuf;

	return first_group_desc(blkbuf) + offset;
}

/**
 * 读取块组内的位图数据块
 */
static struct blkbuf_desc *
lext3_read_bitmapblock(struct super_block *super, unsigned int block_group)
{
	struct lext3_group_desc *group_desc;
	struct blkbuf_desc *blkbuf = NULL;

	group_desc = lext3_get_group_desc(super, block_group, NULL);
	if (!group_desc)
		return NULL;

	/**
	 * 读取数据位图的块号
	 */
	blkbuf = blkbuf_read_block(super, le32_to_cpu(group_desc->datablock_bitmap));
	if (!blkbuf)
		lext3_enconter_error (super, "Cannot read block bitmap - "
			"block_group = %d, block_bitmap = %u", block_group,
			le32_to_cpu(group_desc->datablock_bitmap));

	return blkbuf;
}

/**
 * 校验数据块号是否正常
 */
static inline bool
verify_data_block(struct lext3_superblock_phy *super_phy,
	unsigned long block, unsigned long count)
{
	if (block < le32_to_cpu(super_phy->first_data_block))
		return false;

	if (block + count < block)
		return false;

	if (block + count > le32_to_cpu(super_phy->block_count))
		return false;

	return true;
}

static int has_free_blocks(struct lext3_superblock *lext3_super)
{
	int free_blocks, root_blocks;

	free_blocks =
		approximate_counter_read_positive(&lext3_super->free_block_count);
	root_blocks = le32_to_cpu(lext3_super->phy_super->reserve_blocks);

	if (free_blocks < root_blocks + 1)
		return 0;

	return 1;
}

/**
 * 判断某个块是否可以分配
 * 考虑到日志，需要同时考察提交日志中的位图和内存中的位图
 */
static int test_allocatable(int offset, struct blkbuf_desc *blkbuf)
{
	int ret;
	struct blkbuf_journal_info *blkbuf_jinfo = blkbuf_to_journal(blkbuf);

	/**
	 * 内存块中，相应位为1，不能分配
	 */
	if (lext3_test_bit(offset, blkbuf->block_data))
		return 0;

	/**
	 * 在日志锁的保护下判断提交块的状态
	 */
	blkbuf_lock_state(blkbuf);

	/**
	 * 没有日志在申请其undo访问
	 */
	if (!blkbuf_jinfo->undo_copy)
		ret = 1;
	else
		/**
		 * 待提交的块中，是否允许分配?
		 */
		ret = !lext3_test_bit(offset, blkbuf_jinfo->undo_copy);

	blkbuf_unlock_state(blkbuf);

	return ret;
}

/**
 * 在块组位图中，查找可用块
 * 要同时考虑位图块和日志中的提交块
 * 首先从预期的块开始查找最匹配的块
 * 然后在位图中查找空闲的8个连续块
 * 最后在位图中查找空闲的单个块
 */
static int
find_free_block(int expect, struct blkbuf_desc *blkbuf, int max)
{
	struct blkbuf_journal_info *blkbuf_jinfo;
	char *pstart, *pnext;
	int found, next;
	int start;

	if (expect > 0) {
		int end;

		end = (expect + 63) & ~63;
		if (end > max)
			end = max;

		/**
		 * 从预期的块查找最多64个块
		 * 找到空闲的块
		 */
		found = lext3_find_next_zero_bit(blkbuf->block_data, end, expect);
		/**
		 * 内存块中查找到可用块
		 * 在日志提交块中再判断一下是否可以分配
		 */
		if (found < end && test_allocatable(found, blkbuf))
			return found;
	}

	/**
	 * 从预期的块后面，查找连续8个空闲块
	 */
	start = expect;
	if (start < 0)
		start = 0;

	/**
	 * 查找8个连续空闲块
	 */
	pstart = ((char *)blkbuf->block_data) + (start >> 3);
	pnext = memscan(pstart, 0, (max - start + 7) >> 3);
	next = (pnext - (char *)blkbuf->block_data) << 3;

	if (next < max && next >= expect && test_allocatable(next, blkbuf))
		return next;

	/**
	 * 没有连续的块，那么查找单个的块
	 */
	blkbuf_jinfo = blkbuf_to_journal(blkbuf);

	while (start < max) {
		/**
		 * 首先在内存中查找
		 */
		next = lext3_find_next_zero_bit(blkbuf->block_data, max, start);
		if (next >= max)
			return -1;

		/**
		 * 再次在日志提交块中确认
		 */
		if (test_allocatable(next, blkbuf))
			return next;

		/**
		 * 在日志提交块中确认不通过
		 * 那么将起始查找位置后移
		 */
		blkbuf_lock_state(blkbuf);
		if (blkbuf_jinfo->undo_copy)
			start = lext3_find_next_zero_bit(blkbuf_jinfo->undo_copy,
				max, next);
		blkbuf_unlock_state(blkbuf);
	}

	return -1;
}

/*
 * 分配数据块的主函数
 * 在一个块组的位图里面查找空闲块
 */
static int
__lext3_alloc_block(struct super_block *super, struct journal_handle *handle,
	unsigned int group, struct blkbuf_desc *blkbuf_bitmap, int excepted_block,
	int *perr)
{
	struct blkbuf_journal_info *blkbuf_jinfo;
	int credits = 0;
	int start, end;
	int ret = 0;
	bool got;
	int err;
	int i;

	*perr = 0;

	/*
	 * 获得位图块的undo权限
	 */
	err = lext3_journal_get_undo_access(handle, blkbuf_bitmap, &credits);
	if (err) {
		*perr = err;
		return -1;
	}

	if (excepted_block > 0)
		start = excepted_block;
	else
		start = 0;
	end = LEXT3_BLOCKS_PER_GROUP(super);
	ASSERT(start <= end);

repeat:
	if (excepted_block < 0
	    || !test_allocatable(excepted_block, blkbuf_bitmap)) {
	    	/**
	    	 * 在块组中查找可用的块
	    	 */
		excepted_block = find_free_block(start, blkbuf_bitmap, end);
		/**
		 * 完全没有空闲块，退
		 */
		if (excepted_block < 0) {
			ret = -1;
			goto out;
		}

		/**
		 * 往前面找几个空闲块
		 */
		for (i = 0; i < 7; i++) {
			if (!test_allocatable(excepted_block - 1, blkbuf_bitmap))
				break;

			excepted_block--;
			if (excepted_block <= start)
				break;
		}
	}

	start = excepted_block;

	blkbuf_jinfo = blkbuf_to_journal(blkbuf_bitmap);
	/**
	 * 首先标记内存中的位图
	 */
	if (lext3_set_bit_atomic(excepted_block, blkbuf_bitmap->block_data))
		got = 0;
	else {
		/**
		 * 获得事务锁的情况下，判断事务中的提交块
		 */
		blkbuf_lock_state(blkbuf_bitmap);
		if (blkbuf_jinfo->undo_copy &&
		    lext3_test_bit(excepted_block, blkbuf_jinfo->undo_copy)) {
		    	/**
		    	 * 事务提交块不允许分配此块，清除内存标记
		    	 */
			lext3_clear_bit_atomic(excepted_block,
				blkbuf_bitmap->block_data);
			got = 0;
		} else
			got = 1;

		blkbuf_unlock_state(blkbuf_bitmap);
	}
	
	if (!got) {
		/**
		 * 点背，预期的块被别人抢走了，试下一个块
		 */
		start++;
		excepted_block++;
		if (start >= end) {
			ret = -1;
			goto out;
		}

		goto repeat;
	}

	ret = excepted_block;

	if (ret >= 0) {
		err = lext3_journal_dirty_metadata(handle, blkbuf_bitmap);
		if (err) {
			*perr = err;
			return -1;
		}

		return ret;
	}

out:
	journal_putback_credits(handle, blkbuf_bitmap, credits);

	return ret;
}

/*
 * 在期望的目标块附近分配一个空闲块
 */
int lext3_alloc_block(struct journal_handle *handle, struct file_node *fnode,
	unsigned long excepted_block, int *perr)
{
	struct blkbuf_desc *blkbuf_bitmap = NULL;
	struct lext3_superblock_phy *super_phy;
	struct lext3_superblock *lext3_super;
	struct blkbuf_desc *blkbuf_group;
	struct lext3_group_desc *group;
	unsigned long groups_count;
	struct super_block *super;
	int offset, ret_block;
	int free_blocks;
	int group_num;
	int err, ret;
	int i;

	*perr = -ENOSPC;
	super = fnode->super;
	if (!super)
		return 0;

	lext3_super = super_to_lext3(super);
	super_phy = lext3_super->phy_super;

	if (!has_free_blocks(lext3_super)) {
		*perr = -ENOSPC;
		goto fail;
	}

	if (excepted_block < le32_to_cpu(super_phy->first_data_block) ||
	    excepted_block >= le32_to_cpu(super_phy->block_count))
		excepted_block = le32_to_cpu(super_phy->first_data_block);

	group_num = block_to_group(excepted_block, super, &offset);
	group = lext3_get_group_desc(super, group_num, &blkbuf_group);
	if (!group) {
		*perr = -EIO;
		goto fail;
	}

	/**
	 * 先在预期的块组里面分配
	 */
	free_blocks = le16_to_cpu(group->free_block_count);
	if (free_blocks > 0) {
		blkbuf_bitmap = lext3_read_bitmapblock(super, group_num);
		if (!blkbuf_bitmap) {
			*perr = -EIO;
			goto fail;
		}

		ret_block = __lext3_alloc_block(super, handle, group_num,
			blkbuf_bitmap, offset, &err);
		loosen_blkbuf(blkbuf_bitmap);

		if (err)
			goto fail;

		if (ret_block >= 0)
			goto got;
	}

	/**
	 * 预期的块组已经没有空间了，在其他块组里面找
	 * 从预期的组后面依次查找块组
	 */
	groups_count = super_to_lext3(super)->groups_count;
	for (i = 0; i < groups_count; i++) {
		group_num++;
		if (group_num >= groups_count)
			group_num = 0;

		group = lext3_get_group_desc(super, group_num, &blkbuf_group);
		if (!group) {
			*perr = -EIO;
			goto fail;
		}

		free_blocks = le16_to_cpu(group->free_block_count);
		if (free_blocks <= 0)
			continue;

		blkbuf_bitmap = lext3_read_bitmapblock(super, group_num);
		if (!blkbuf_bitmap) {
			*perr = -EIO;
			goto fail;
		}

		ret_block = __lext3_alloc_block(super, handle, group_num,
					blkbuf_bitmap, -1, &err);
		loosen_blkbuf(blkbuf_bitmap);

		if (err)
			goto fail;

		if (ret_block >= 0) 
			goto got;
	}

	*perr = -ENOSPC;

fail:
	if (err) {
		*perr = err;
		lext3_std_error(super, err);
	}

	return 0;

got:
	err = lext3_journal_get_write_access(handle, blkbuf_group);
	if (err)
		goto fail;

	ret_block = ret_block + group_num * LEXT3_BLOCKS_PER_GROUP(super)
					+ le32_to_cpu(super_phy->first_data_block);

	if (ret_block == le32_to_cpu(group->datablock_bitmap) ||
	    ret_block == le32_to_cpu(group->fnode_bitmap) ||
	    block_overlap(ret_block, le32_to_cpu(group->first_fnode_block),
		      super_to_lext3(super)->fnode_blocks_per_group))
		lext3_enconter_error(super,
			"Allocating block in system zone - block = %u", ret_block);

	if (ret_block >= le32_to_cpu(super_phy->block_count)) {
		lext3_enconter_error(super, "block(%d) >= blocks count(%d) - "
			"block_group = %d, super_phy == %p ", ret_block,
			le32_to_cpu(super_phy->block_count), group_num, super_phy);
		goto fail;
	}

	/*
	 * 对块组和文件系统中的可用块进行计数
	 */
	smp_lock(lext3_block_group_lock(lext3_super, group_num));
	group->free_block_count =
			cpu_to_le16(le16_to_cpu(group->free_block_count) - 1);
	smp_unlock(lext3_block_group_lock(lext3_super, group_num));
	approximate_counter_mod(&lext3_super->free_block_count, -1);

	ret = lext3_journal_dirty_metadata(handle, blkbuf_group);
	if (!err)
		err = ret;

	super->dirty = 1;
	if (err)
		goto fail;

	*perr = 0;

	return ret_block;
}

/**
 * 释放文件节点的特定块
 */
void
lext3_free_blocks(struct journal_handle *handle, struct file_node *fnode,
	unsigned long block, unsigned long count)
{
	struct blkbuf_desc *blkbuf_bitmap = NULL;
	struct lext3_superblock_phy *super_phy;
	struct lext3_superblock *lext3_super;
	struct blkbuf_desc *blkbuf_group;
	struct lext3_group_desc *group;
	unsigned long remain_count;
	unsigned long group_num;
	struct super_block *super;
	int offset;
	unsigned long i;
	int err = 0, ret;

	super = fnode->super;
	if (!super)
		return;

	lext3_super = super_to_lext3(super);
	super_phy = super_to_lext3(super)->phy_super;

	/**
	 * 验证逻辑块号是否正常
	 */
	if (verify_data_block(super_phy, block, count)) {
		lext3_enconter_error (super, "block = %lu, count = %lu", block, count);
		goto fail;
	}

remain:
	/**
	 * 计算本轮释放数量，及起始位置
	 */
	remain_count = 0;
	group_num = block_to_group(block, super, &offset);
	if (offset + count > LEXT3_BLOCKS_PER_GROUP(super)) {
		remain_count = offset + count - LEXT3_BLOCKS_PER_GROUP(super);
		count = LEXT3_BLOCKS_PER_GROUP(super) - offset;
	}

	blkbuf_bitmap = lext3_read_bitmapblock(super, group_num);
	if (!blkbuf_bitmap)
		goto fail;

	group = lext3_get_group_desc (super, group_num, &blkbuf_group);
	if (!group)
		goto fail;

	if (!verify_group_block(super, group, block, count))
		lext3_enconter_error (super, "Freeing blocks in system zones - "
			"Block = %lu, count = %lu", block, count);

	/**
	 * 对于位图块，需要获得undo权限
	 */
	err = lext3_journal_get_undo_access(handle, blkbuf_bitmap, NULL);
	if (err)
		goto fail;

	/*
	 * 获得块组的写权限
	 */
	err = lext3_journal_get_write_access(handle, blkbuf_group);
	if (err)
		goto fail;

	blkbuf_lock_state(blkbuf_bitmap);

	for (i = 0; i < count; i++) {
		struct blkbuf_journal_info *blkbuf_jinfo;

		blkbuf_jinfo = blkbuf_to_journal(blkbuf_bitmap);
		/**
		 * 对于位图块来说，一定在日志里面有两份数据
		 */
		ASSERT(blkbuf_jinfo && blkbuf_jinfo->undo_copy);
		/**
		 * 当前块正在释放，这之前是不可用的
		 * 因此日志中的位图需要标记为不可分配
		 */
		lext3_set_bit_atomic(offset + i, blkbuf_jinfo->undo_copy);
		/**
		 * 内存中的块清0，表示可用
		 * 待日志提交完毕后，块真正可用
		 */
		if (!lext3_clear_bit_atomic(offset + i, blkbuf_bitmap->block_data)) {
			blkbuf_unlock_state(blkbuf_bitmap);
			lext3_enconter_error(super, "bit already cleared for block %lu",
				block + i);
			blkbuf_lock_state(blkbuf_bitmap);
		}
	}
	blkbuf_unlock_state(blkbuf_bitmap);
	smp_lock(lext3_block_group_lock(lext3_super, group_num));
	group->free_block_count =
			cpu_to_le16(le16_to_cpu(group->free_block_count) + count);
	smp_unlock(lext3_block_group_lock(lext3_super, group_num));
	approximate_counter_mod(&lext3_super->free_block_count, count);

	err = lext3_journal_dirty_metadata(handle, blkbuf_bitmap);
	ret = lext3_journal_dirty_metadata(handle, blkbuf_group);
	if (!err)
		err = ret;

	/**
	 * 处理下一个块组的数据
	 */
	if (remain_count && !err) {
		block += count;
		count = remain_count;
		loosen_blkbuf(blkbuf_bitmap);

		goto remain;
	}
	super->dirty = 1;
fail:
	loosen_blkbuf(blkbuf_bitmap);
	lext3_std_error(super, err);
}

static inline int is_power(int group, int base)
{
	int num = base;

	while (group > num)
		num *= base;

	return num == group;
} 

/**
 * 在块组中，块组超级块占用的逻辑块数量
 * 仅仅可能是0或者1
 */
int lext3_group_has_super(struct super_block *super, int group)
{
	bool sparse_feature;
	bool backup = 0;

	sparse_feature = LEXT3_HAS_RO_COMPAT_FEATURE(super,
		LEXT3_FEATURE_RO_COMPAT_SPARSE_SUPER);
	if (sparse_feature) {
		/**
		 * 如果组号是0，1，以及3/5/7的乘方
		 * 则这些块需要备份超级块
		 */
		if (group <= 1)
			backup = 1;
		else
			backup = (is_power(group, 3) || is_power(group, 5) ||
				is_power(group, 7));

		if (!backup)
			return 0;
	}

 	return 1;
}

/**
 * 计算文件系统中空闲的数据块
 */
unsigned long lext3_count_free_blocks(struct super_block *super)
{
	struct lext3_group_desc *group;
	unsigned long ret;
	int i;

	ret = 0;
	for (i = 0; i < super_to_lext3(super)->groups_count; i++) {
		group = lext3_get_group_desc(super, i, NULL);
		if (!group)
			continue;

		ret += le16_to_cpu(group->free_block_count);
	}

	return ret;
}

/**
 * 当没有磁盘块可供分配时，是否重试分配
 */
int lext3_should_retry_alloc(struct super_block *super, int *retries)
{
	if (!has_free_blocks(super_to_lext3(super)) || (*retries)++ > 3)
		return 0;

	return journal_force_commit_nested(super_to_lext3(super)->journal);
}

/**
 * 每个组中，组描述符占用的块
 */
unsigned long lext3_blkcount_groupdesc(struct super_block *super, int group)
{
	/**
	 * 暂时返回一个不太准确的数据，不影响
	 */
	return 0;
}

/* TO-DO */
void lext3_discard_reservation(struct file_node *file_node)
{
}
