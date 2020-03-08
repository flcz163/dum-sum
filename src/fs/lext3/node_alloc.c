#include <dim-sum/block_buf.h>
#include <dim-sum/err.h>
#include <dim-sum/fs.h>
#include <dim-sum/lext3_fs.h>
#include <dim-sum/journal.h>

#include "internal.h"

#define FNODE_COST 64
#define BLOCK_COST 256

/*
 * 读取特定块组的文件节点位图
 */
static struct blkbuf_desc *
lext3_read_fnode_bitmap(struct super_block *super, unsigned long group)
{
	struct lext3_group_desc *desc;
	struct blkbuf_desc *blkbuf = NULL;

	desc = lext3_get_group_desc(super, group, NULL);
	if (!desc)
		goto fail;

	blkbuf = blkbuf_read_block(super, le32_to_cpu(desc->fnode_bitmap));
	if (!blkbuf)
		lext3_enconter_error(super, "Cannot read file node bitmap - "
			    "block_group = %lu, bitmap = %u",
			    group, le32_to_cpu(desc->fnode_bitmap));
fail:
	return blkbuf;
}

/**
 * 根据文件节点编号，从磁盘上将信息读入内存。
 */
struct file_node *
lext3_load_orphan_fnode(struct super_block *super, unsigned long fnode_num)
{
	unsigned long fnode_count;
	unsigned long block_group;
	struct blkbuf_desc *bitmap_blkbuf = NULL;
	struct file_node *fnode = NULL;
	int offset;

	/**
	 * 文件节点编号错误，可能是磁盘里面读取的值不正确
	 */
	fnode_count = le32_to_cpu(super_to_lext3(super)->phy_super->fnode_count);
	if (fnode_num > fnode_count)
		return NULL;

	block_group = fnode_num_to_group(fnode_num, super, &offset);
	bitmap_blkbuf = lext3_read_fnode_bitmap(super, block_group);
	if (!bitmap_blkbuf)
		return NULL;

	/**
	 * 判断文件节点的有效性
	 *	1、节点在位图中为空闲
	 *	2、读取节点失败
	 *	3、异常节点
	 *	4、下一个孤儿节点值不正常
	 */
	if (!lext3_test_bit(offset, bitmap_blkbuf->block_data) ||
	    !(fnode = fnode_read(super, fnode_num))
	    || is_bad_file_node(fnode)
	    || LEXT3_NEXT_ORPHAN(fnode) > fnode_count) {
		printk(KERN_NOTICE "file_node=%p\n", fnode);
		if (fnode) {
			printk(KERN_NOTICE "is_bad_file_node(file_node)=%d\n",
			       is_bad_file_node(fnode));
			printk(KERN_NOTICE "LEXT3_NEXT_ORPHAN(file_node)=%u\n",
			       LEXT3_NEXT_ORPHAN(fnode));
			printk(KERN_NOTICE "max node=%lu\n", fnode_count);
		}

		/**
		 * 是一个异常节点，内存中的信息不可靠
		 * 清空数据块数量，避免错误的释放数据块
		 */
		if (fnode && fnode->link_count == 0)
			fnode->block_count = 0;

		loosen_file_node(fnode);
		fnode = NULL;
	}

	loosen_blkbuf(bitmap_blkbuf);

	return fnode;
}

/*
 * 为目录 节点找到一个适合的块组
 * 旧的算法，不太好
 */
static int find_dir_group_old(struct super_block *super, struct file_node *dir)
{
	struct lext3_group_desc *desc, *best_desc = NULL;
	int free_fnode_count, avg_fnode_count;
	struct blkbuf_desc *blkbuf;
	int group, best_group = -1;
	int group_count;

	group_count = super_to_lext3(super)->groups_count;
	free_fnode_count = get_super_free_fnodes(super);
	avg_fnode_count = free_fnode_count / group_count;

	/**
	 * 冒泡排序，直接到一个空闲块最多的块组
	 */
	for (group = 0; group < group_count; group++) {
		desc = lext3_get_group_desc (super, group, &blkbuf);
		if (!desc || !desc->free_fnode_count)
			continue;

		if (le16_to_cpu(desc->free_fnode_count) < avg_fnode_count)
			continue;

		if (!best_desc || 
		    (le16_to_cpu(desc->free_block_count) >
		     le16_to_cpu(best_desc->free_block_count))) {
			best_group = group;
			best_desc = desc;
		}
	}

	return best_group;
}

/**
 * Orlov节点分配，为目录节点找到一个目标块组
 */
static int
find_dir_group_orlov(struct super_block *super, struct file_node *dir)
{
	int max_debt, max_dirs, min_blocks, min_fnodes;
	int free_fnode_count, avg_fnode_count;
	int free_block_count, avg_block_count;
	struct lext3_superblock_phy *super_phy;
	struct lext3_superblock *lext3_super;
	int blocks_per_dir, dir_count;
	struct lext3_group_desc *desc;
	struct blkbuf_desc *blkbuf;
	unsigned long group = -1;
	int nodes_in_groups;
	int group_count;
	int dir_group;
	int i;

	dir_group = fnode_to_lext3(dir)->block_group_num;
	lext3_super = super_to_lext3(super);
	super_phy = lext3_super->phy_super;
	group_count = lext3_super->groups_count;
	nodes_in_groups = LEXT3_FNODES_PER_GROUP(super);

	/**
	 * 统计值
	 *	1、空闲文件节点数
	 *	2、块组中平均空闲节点数
	 *	3、空闲块数
	 *	4、块组中平均空闲块数
	 */
	free_fnode_count =
		approximate_counter_read_positive(&lext3_super->free_fnode_count);
	avg_fnode_count = free_fnode_count / group_count;
	free_block_count =
		approximate_counter_read_positive(&lext3_super->free_block_count);
	avg_block_count = free_block_count / group_count;
	dir_count = approximate_counter_read_positive(&lext3_super->dir_count);

	/**
	 * 在顶级目录下创建子目录
	 * 或者子目录不与父目录共享块组
	 */
	if ((dir == super->root_fnode_cache->file_node) ||
	    (fnode_to_lext3(dir)->flags & LEXT3_TOPDIR_FL)) {
		int max_count = nodes_in_groups;
		int best_group = -1;

		/**
		 * 随便找一个起始块组
		 */
		group = jiffies;
		dir_group = (unsigned)group % group_count;
		for (i = 0; i < group_count; i++) {
			group = (dir_group + i) % group_count;
			desc = lext3_get_group_desc (super, group, &blkbuf);
			if (!desc || !desc->free_fnode_count)
				continue;

			/**
			 * 目录过多，不适合
			 */
			if (le16_to_cpu(desc->dir_count) >= max_count)
				continue;

			/**
			 * 空闲文件节点低于平均数
			 */
			if (le16_to_cpu(desc->free_fnode_count) < avg_fnode_count)
				continue;

			/**
			 * 空闲数据块低于平均数
			 */
			if (le16_to_cpu(desc->free_block_count) < avg_block_count)
				continue;

			/**
			 * 将当前组作为备选块组，是目前发现的最优组
			 */
			best_group = group;
			max_count = le16_to_cpu(desc->dir_count);
		}

		if (best_group >= 0)
			return best_group;

		/**
		 * 优选法不生效，随便找一个块组好了
		 */
		goto worst;
	}

	/**
	 * 平均每个目录中的块数
	 */
	blocks_per_dir = (le32_to_cpu(super_phy->block_count) - free_block_count)
					/ dir_count;
	max_dirs = dir_count / group_count + nodes_in_groups / 16;
	min_fnodes = avg_fnode_count - nodes_in_groups / 4;
	min_blocks = avg_block_count - LEXT3_BLOCKS_PER_GROUP(super) / 4;
	max_debt = LEXT3_BLOCKS_PER_GROUP(super)
				/ max(blocks_per_dir, BLOCK_COST);
	if (max_debt * FNODE_COST > nodes_in_groups)
		max_debt = nodes_in_groups / FNODE_COST;
	if (max_debt > 255)
		max_debt = 255;
	if (max_debt == 0)
		max_debt = 1;

	/**
	 * 从父目录所在块组开始，找一个满足要求的块组
	 */
	for (i = 0; i < group_count; i++) {
		group = (dir_group + i) % group_count;
		desc = lext3_get_group_desc (super, group, &blkbuf);

		if (!desc || !desc->free_fnode_count)
			continue;

		if (le16_to_cpu(desc->dir_count) >= max_dirs)
			continue;

		if (le16_to_cpu(desc->free_fnode_count) < min_fnodes)
			continue;

		if (le16_to_cpu(desc->free_block_count) < min_blocks)
			continue;

		return group;
	}

worst:
	for (i = 0; i < group_count; i++) {
		group = (dir_group + i) % group_count;
		desc = lext3_get_group_desc (super, group, &blkbuf);
		if (!desc || !desc->free_fnode_count)
			continue;

		if (le16_to_cpu(desc->free_fnode_count) >= avg_fnode_count)
			return group;
	}

	if (avg_fnode_count) {
		avg_fnode_count = 0;
		goto worst;
	}

	return -1;
}

/**
 * 为新文件(非目录)查找所在的块组
 */
static int find_file_group(struct super_block *super, struct file_node *dir)
{
	struct lext3_group_desc *group_desc;
	struct blkbuf_desc *blkbuf;
	int group_count;
	int dir_group;
	int group, i;

	dir_group = fnode_to_lext3(dir)->block_group_num;
	group_count = super_to_lext3(super)->groups_count;

	/**
	 * 尽量与目录处于同一块组
	 */
	group = dir_group;
	group_desc = lext3_get_group_desc (super, group, &blkbuf);
	/**
	 * 太好了，块组正好有空闲的节点编号和数据块
	 */
	if (group_desc
	    && le16_to_cpu(group_desc->free_fnode_count)
	    && le16_to_cpu(group_desc->free_block_count))
		return group;

	/**
	 * 没办法与父目录处于同一个块组
	 * 但是并不能直接用下一组
	 * 并且需要与父目录有点关系，这样同一目录下的文件才聚集
	 * 因此使用目录的节点号作为参照
	 */
	group = (group + dir->node_num) % group_count;

	for (i = 1; i < group_count; i <<= 1) {
		group += i;
		if (group >= group_count)
			group %= group_count;

		group_desc = lext3_get_group_desc (super, group, &blkbuf);
		if (group_desc
		    && le16_to_cpu(group_desc->free_fnode_count)
		    && le16_to_cpu(group_desc->free_block_count))
			return group;
	}

	group = dir_group;
	for (i = 0; i < group_count; i++) {
		group++;
		if (group >= group_count)
			group = 0;

		group_desc = lext3_get_group_desc (super, group, &blkbuf);
		/**
		 * 这里不再判断块了，否则容易失败
		 */
		if (group_desc && le16_to_cpu(group_desc->free_fnode_count))
			return group;
	}

	return -1;
}

/**
 * 分配一个文件节点
 */
struct file_node *lext3_alloc_fnode(struct journal_handle *handle,
	struct file_node *dir, int mode)
{
	struct lext3_group_desc *group_desc = NULL;
	struct blkbuf_desc *bitmap_blkbuf = NULL;
	struct lext3_superblock_phy *super_phy;
	struct lext3_superblock *lext3_super;
	struct lext3_file_node *lext3_fnode;
	unsigned long fnode_num = 0;
	struct super_block *super;
	struct blkbuf_desc *blkbuf;
	struct file_node * fnode;
	struct file_node *ret;
	int err = 0;
	int group;
	int i;

	/**
	 * 目录不存在或者已经彻底删除
	 */
	if (!dir || !dir->link_count)
		return ERR_PTR(-EPERM);

	super = dir->super;
	/**
	 * 为文件分配一个内存节点
	 */
	fnode = fnode_alloc(super);
	if (!fnode)
		return ERR_PTR(-ENOMEM);

	lext3_fnode = fnode_to_lext3(fnode);
	lext3_super = super_to_lext3(super);
	super_phy = lext3_super->phy_super;

	if (S_ISDIR(mode)) {
		/**
		 * 为目录查找其所属的块组
		 * 可以用老式的算法或者orlov算法
		 */
		if (lext3_test_opt (super, LEXT3_MOUNT_OLDALLOC))
			group = find_dir_group_old(super, dir);
		else
			group = find_dir_group_orlov(super, dir);
	} else
		group = find_file_group(super, dir);

	/**
	 * 文件系统完全没有空间了
	 */
	if (group == -1) {
		loosen_file_node(fnode);
		return ERR_PTR(-ENOSPC);
	}

	/**
	 * 从优先块组开始，遍历所有块组
	 */
	for (i = 0; i < lext3_super->groups_count; i++) {
		group_desc = lext3_get_group_desc(super, group, &blkbuf);

		err = -EIO;
		bitmap_blkbuf = lext3_read_fnode_bitmap(super, group);
		if (!bitmap_blkbuf)
			goto fail;

		fnode_num = 0;

try_again:
		/**
		 * 查找第一个未用的磁盘文件节点
		 */
		fnode_num = lext3_find_next_zero_bit(bitmap_blkbuf->block_data,
			LEXT3_FNODES_PER_GROUP(super), fnode_num);
		/**
		 * 查找到可用的文件节点编号
		 */
		if (fnode_num < LEXT3_FNODES_PER_GROUP(super)) {
			int credits = 0;

			err = ext3_journal_get_write_access_credits(handle,
							bitmap_blkbuf, &credits);
			if (err) {
				loosen_blkbuf(bitmap_blkbuf);
				bitmap_blkbuf = NULL;
				goto fail;
			}

			/**
			 * 原子的设置相应的块组位为1，表示已经占用此文件节点
			 */
			if (!lext3_set_bit_atomic(fnode_num, bitmap_blkbuf->block_data)) {
				/**
				 * 成功获得此文件节点，标记节点为脏
				 */
				err = lext3_journal_dirty_metadata(handle, bitmap_blkbuf);
				loosen_blkbuf(bitmap_blkbuf);
				bitmap_blkbuf = NULL;
				if (err)
					goto fail;

				goto got;
			}
			/**
			 * 冲突了，释放日志额度
			 */
			journal_putback_credits(handle, bitmap_blkbuf, credits);

			/**
			 * 再板一下，继续在块组的节点位图中查找
			 */
			fnode_num++;
			if (fnode_num < LEXT3_FNODES_PER_GROUP(super))
				goto try_again;
		}

		/**
		 * 为什么find**函数找到的块组里面竟然分配失败
		 * 再次find一下行吗?
		 * 而这里竟然从下一个块组里面查找，怪哉!
		 */
		group++;
		if (group == lext3_super->groups_count)
			group = 0;

		loosen_blkbuf(bitmap_blkbuf);
		bitmap_blkbuf = NULL;
	}

	loosen_file_node(fnode);
	return ERR_PTR(-ENOSPC);

got:
	/**
	 * 文件编号从1开始!
	 */
	fnode_num += group * LEXT3_FNODES_PER_GROUP(super) + 1;
	/**
	 * 这种情况会出现吗?
	 */
	if (fnode_num < LEXT3_FIRST_FILENO(super)
	    || fnode_num > le32_to_cpu(super_phy->fnode_count)) {
		lext3_enconter_error (super,
			"reserved file_node or file_node > inodes count - "
			"block_group = %d, file_node=%lu", group, fnode_num);
		err = -EIO;
		goto fail;
	}

	/**
	 * 获得块组描述符的写权限
	 */
	err = lext3_journal_get_write_access(handle, blkbuf);
	if (err)
		goto fail;

	/**
	 * 修改块组的空闲节点计数
	 */
	smp_lock(lext3_block_group_lock(lext3_super, group));
	group_desc->free_fnode_count =
		cpu_to_le16(le16_to_cpu(group_desc->free_fnode_count) - 1);
	if (S_ISDIR(mode)) {
		group_desc->dir_count =
			cpu_to_le16(le16_to_cpu(group_desc->dir_count) + 1);
	}
	smp_unlock(lext3_block_group_lock(lext3_super, group));
	/* 标记块组描述符为脏 */
	err = lext3_journal_dirty_metadata(handle, blkbuf);
	if (err)
		goto fail;

	/**
	 * 修改全局文件节点计数
	 */
	approximate_counter_dec(&lext3_super->free_fnode_count);
	if (S_ISDIR(mode))
		approximate_counter_inc(&lext3_super->dir_count);
	super->dirty = 1;

	fnode->uid = current->fsuid;
	if (lext3_test_opt (super, LEXT3_MOUNT_GRPID))
		fnode->gid = dir->gid;
	else if (dir->mode & S_ISGID) {
		fnode->gid = dir->gid;
		if (S_ISDIR(mode))
			mode |= S_ISGID;
	} else
		fnode->gid = current->fsgid;
	fnode->mode = mode;
	fnode->node_num = fnode_num;
	/**
	 * 这个字段是麻鬼的
	 */
	fnode->block_size = PAGE_SIZE;
	fnode->block_count = 0;
	fnode->data_modify_time = fnode->access_time
		= fnode->meta_modify_time = CURRENT_TIME_SEC;
	/**
	 * 清除垃圾数据块指针信息
	 */
	memset(lext3_fnode->data_blocks, 0, sizeof(lext3_fnode->data_blocks));
	lext3_fnode->last_logic_block = 0;
	lext3_fnode->last_phy_block = 0;
	lext3_fnode->lookup_start = 0;
	lext3_fnode->filesize_disk = 0;
	lext3_fnode->flags = fnode_to_lext3(dir)->flags & ~LEXT3_INDEX_FL;
	if (S_ISLNK(mode))
		lext3_fnode->flags &= ~(LEXT3_IMMUTABLE_FL | LEXT3_APPEND_FL);
	if (!S_ISDIR(mode))
		lext3_fnode->flags &= ~LEXT3_DIRSYNC_FL;
	lext3_fnode->file_acl = 0;
	lext3_fnode->dir_acl = 0;
	lext3_fnode->next_orphan = 0;
	lext3_fnode->block_group_num = group;

	/**
	 * 设置内存中的文件标志，并将内存节点对象放入哈希表
	 */
	lext3_set_fnode_flags(fnode);
	putin_file_node(fnode);

	if (IS_DIRSYNC(fnode))
		handle->sync = 1;

	smp_lock(&lext3_super->gen_lock);
	lext3_super->generation++;
	fnode->generation = lext3_super->generation;
	smp_unlock(&lext3_super->gen_lock);

	lext3_fnode->state = LEXT3_STATE_NEW;
	lext3_fnode->extra_fsize =
		(LEXT3_FNODE_SIZE(fnode->super) > LEXT3_OLD_FNODE_SIZE) ?
		sizeof(struct lext3_fnode_disk) - LEXT3_OLD_FNODE_SIZE : 0;

	ret = fnode;

	/* 获得inode的写权限，并将其置为脏 */
	err = lext3_mark_fnode_dirty(handle, fnode);
	if (err) {
		lext3_std_error(super, err);
		fnode->flags |= S_NOQUOTA;
		fnode->link_count = 0;
		/**
		 * 这里会将文件节点从哈希表中摘除
		 */
		loosen_file_node(fnode);
		/**
		 * 既然失败了，就要解除对位图块的引用
		 */
		loosen_blkbuf(bitmap_blkbuf);
		return ERR_PTR(err);
	}

	return ret;

fail:
	lext3_std_error(super, err);
	loosen_file_node(fnode);
	ret = ERR_PTR(err);

	return ret;
}

/**
 * 释放文件节点
 * 此时文件节点已经在不哈希表中，也从目录项中删除
 * 因此其他人不会再访问此节点
 */
void lext3_free_fnode(struct journal_handle *handle, struct file_node *fnode)
{
	struct blkbuf_desc *bitmap_blkbuf = NULL;
	struct lext3_superblock_phy *super_phy;
	struct lext3_group_desc *group_desc;
	struct lext3_superblock *lext3_super;
	struct blkbuf_desc *blkbuf;
	unsigned long block_group;
	struct super_block *super;
	unsigned long fnode_num;
	int fatal = 0, err;
	int offset;

	super = fnode->super;

	if (accurate_read(&fnode->ref_count) > 1) {
		printk ("lext3_free_fnode: file node has count=%d\n",
			accurate_read(&fnode->ref_count));
		return;
	}

	if (fnode->link_count) {
		printk ("lext3_free_fnode: file node has link_count=%d\n",
			fnode->link_count);
		return;
	}

	if (!super) {
		printk("lext3_free_fnode: file node on nonexistent device\n");
		return;
	}

	lext3_super = super_to_lext3(super);
	fnode_num = fnode->node_num;
	super_phy = super_to_lext3(super)->phy_super;

	/**
	 * 回写数据，完成其他清理工作
	 * 然后才能在位图中归还节点，以避免冲突
	 */
	fnode_clean (fnode);

	if (fnode_num < LEXT3_FIRST_FILENO(super)
	    || fnode_num > le32_to_cpu(super_phy->fnode_count)) {
		lext3_enconter_error (super, "reserved or nonexistent file node %lu",
			fnode_num);
		goto fail;
	}

	block_group = fnode_num_to_group(fnode_num, super, &offset);
	bitmap_blkbuf = lext3_read_fnode_bitmap(super, block_group);
	if (!bitmap_blkbuf)
		goto fail;

	fatal = lext3_journal_get_write_access(handle, bitmap_blkbuf);
	if (fatal)
		goto fail;

	/**
	 * 清除位标记，表示节点号空闲
	 */
	if (!lext3_clear_bit_atomic(offset, bitmap_blkbuf->block_data))
		lext3_enconter_error (super, "bit already cleared for file_node %lu",
			fnode_num);
	else {
		group_desc = lext3_get_group_desc(super, block_group, &blkbuf);

		/**
		 * 获得块组的日志写权限
		 */
		fatal = lext3_journal_get_write_access(handle, blkbuf);
		if (fatal)
			goto fail;

		if (group_desc) {
			/**
			 * 修改块组元数据中的统计计数
			 */
			smp_lock(lext3_block_group_lock(lext3_super, block_group));
			group_desc->free_fnode_count = cpu_to_le16(
				le16_to_cpu(group_desc->free_fnode_count) + 1);
			if (S_ISDIR(fnode->mode))
				group_desc->dir_count = 
					cpu_to_le16(le16_to_cpu(group_desc->dir_count) - 1);
			smp_unlock(lext3_block_group_lock(lext3_super, block_group));
			/**
			 * 修改整个文件系统的统计计数
			 */
			approximate_counter_inc(&lext3_super->free_fnode_count);
			if (S_ISDIR(fnode->mode))
				approximate_counter_dec(&lext3_super->dir_count);
		}

		err = lext3_journal_dirty_metadata(handle, blkbuf);
		if (!fatal)
			fatal = err;
	}

	err = lext3_journal_dirty_metadata(handle, bitmap_blkbuf);
	if (!fatal)
		fatal = err;

	super->dirty = 1;

fail:
	loosen_blkbuf(bitmap_blkbuf);
	lext3_std_error(super, fatal);
}

/**
 * 计算文件系统中可用的文件节点数量
 */
unsigned long lext3_count_free_fnodes(struct super_block *super)
{
	struct lext3_group_desc *group;
	unsigned long ret;
	int i;

	ret = 0;
	for (i = 0; i < super_to_lext3(super)->groups_count; i++) {
		group = lext3_get_group_desc (super, i, NULL);
		if (!group)
			continue;

		ret += le16_to_cpu(group->free_fnode_count);
	}

	return ret;
}

/**
 * 统计文件系统中的目录数量
 */
unsigned long lext3_count_dirs (struct super_block *super)
{
	unsigned long groups_count;
	unsigned long ret = 0;
	int i;

	groups_count = super_to_lext3(super)->groups_count;
	for (i = 0; i < groups_count; i++) {
		struct lext3_group_desc *group_desc;

		group_desc = lext3_get_group_desc (super, i, NULL);
		if (!group_desc)
			continue;

		ret += le16_to_cpu(group_desc->dir_count);
	}

	return ret;
}
