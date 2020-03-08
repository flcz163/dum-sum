#include <dim-sum/aio.h>
#include <dim-sum/beehive.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/block_buf.h>
#include <dim-sum/err.h>
#include <dim-sum/fs.h>
#include <dim-sum/lext3_fs.h>
#include <dim-sum/highmem.h>
#include <dim-sum/journal.h>
#include <dim-sum/rbtree.h>
#include <dim-sum/uio.h>
#include <dim-sum/pagemap.h>
#include <dim-sum/timer.h>
#include <dim-sum/stacktrace.h>

#include "internal.h"

/**
 * 将删除的文件或者截断的文件添加到磁盘和内存链表中。
 * 如果没有彻底完成删除或者截断操作，则在恢复时继续
 */
int lext3_add_orphan(struct journal_handle *handle, struct file_node *fnode)
{
	struct super_block *super = fnode->super;
	struct lext3_fnode_loc file_loc;
	int err = 0, rc;

	/**
	 * 操作超级块之前，先获得其锁
	 */
	down(&super->sem);
	/**
	 * 已经将它加入到链表中了。
	 */
	if (!list_is_empty(&fnode_to_lext3(fnode)->orphan))
		goto unlock;

	ASSERT ((S_ISREG(fnode->mode) || S_ISDIR(fnode->mode) ||
		S_ISLNK(fnode->mode)) || fnode->link_count == 0);

	/**
	 * 获得超级块的日志权限
	 */
	err = lext3_journal_get_write_access(handle,
		super_to_lext3(super)->blkbuf_super);
	if (err)
		goto unlock;

	/** 
	 * 读取文件节点的信息到内存中
	 * 并准备写节点内容
	 */
	err = lext3_fnode_prepare_write(handle, fnode, &file_loc);
	if (err)
		goto unlock;

	/**
	 * 将文件节点插入到全局孤儿链表的前面
	 */
	LEXT3_NEXT_ORPHAN(fnode) = le32_to_cpu(super_to_lext3(super)->phy_super->last_orphan);
	super_to_lext3(super)->phy_super->last_orphan = cpu_to_le32(fnode->node_num);
	/**
	 * 将超级块和文件节点块置脏
	 */
	err = lext3_journal_dirty_metadata(handle, super_to_lext3(super)->blkbuf_super);
	rc = lext3_mark_fnode_loc_dirty(handle, fnode, &file_loc);
	if (!err)
		err = rc;

	/**
	 * 将orphan节点链接到内存链表中。
	 */
	if (!err)
		list_insert_front(&fnode_to_lext3(fnode)->orphan, &super_to_lext3(super)->orphans);

unlock:
	up(&super->sem);
	lext3_std_error(fnode->super, err);

	return err;
}

/**
 * 当删除和截断操作真正完成以后
 * 在磁盘和内存中删除孤儿节点。
 */
int lext3_orphan_del(struct journal_handle *handle, struct file_node *fnode)
{
	struct lext3_superblock *lext3_super;
	struct lext3_file_node *lext3_fnode;
	struct lext3_fnode_loc file_loc;
	struct double_list *prev;
	unsigned long next;
	int err = 0;

	lext3_fnode = fnode_to_lext3(fnode);
	lext3_super = super_to_lext3(fnode->super);

	down(&fnode->super->sem);
	/**
	 * 不在孤儿链表中，直接退出
	 */
	if (list_is_empty(&lext3_fnode->orphan)) {
		up(&fnode->super->sem);
		return 0;
	}

	/**
	 *  取磁盘中保存的下一个节点。
	 */
	next = LEXT3_NEXT_ORPHAN(fnode);
	/**
	 * 取内存中的上一个节点。
	 */
	prev = lext3_fnode->orphan.prev;

	/**
	 * 从内存中摘除
	 */
	list_del_init(&lext3_fnode->orphan);

	if (!handle)
		goto out;

	err = lext3_fnode_prepare_write(handle, fnode, &file_loc);
	if (err)
		goto fail;

	/* 第一个孤儿 节点 */
	if (prev == &lext3_super->orphans) {
		err = lext3_journal_get_write_access(handle, lext3_super->blkbuf_super);
		if (err) {
			loosen_blkbuf(file_loc.blkbuf);
			goto fail;
		}

		/* 修改头节点，删除当前节点*/
		lext3_super->phy_super->last_orphan = cpu_to_le32(next);
		err = lext3_journal_dirty_metadata(handle, lext3_super->blkbuf_super);
	} else {
		struct lext3_fnode_loc loc_prev;
		struct lext3_file_node *fnode_prev;

		fnode_prev = list_container(prev, struct lext3_file_node, orphan);
		err = lext3_fnode_prepare_write(handle, &fnode_prev->vfs_fnode,
			&loc_prev);
		if (err) {
			loosen_blkbuf(file_loc.blkbuf);
			goto fail;
		}
		/**
		 * 修改前一节点的指针，使其指向下一个节点
		 */
		fnode_prev->next_orphan = next;
		err = lext3_mark_fnode_loc_dirty(handle, &fnode_prev->vfs_fnode,
			&loc_prev);
	}

	if (err) {
		loosen_blkbuf(file_loc.blkbuf);
		goto fail;
	}

	LEXT3_NEXT_ORPHAN(fnode) = 0;
	err = lext3_mark_fnode_loc_dirty(handle, fnode, &file_loc);

fail:
	lext3_std_error(fnode->super, err);
out:
	up(&fnode->super->sem);
	return err;
}

/*
 * 将内存中的文件节点数据同步到磁盘缓冲区
 * 并将缓冲区设置为脏
 */
static int lext3_update_fnode(struct journal_handle *handle, 
	struct file_node *fnode, struct lext3_fnode_loc *fnode_loc)
{
	struct lext3_fnode_disk *disk_node = lext3_get_fnode_disk(fnode_loc);
	struct lext3_file_node *lext3_fnode = fnode_to_lext3(fnode);
	struct blkbuf_desc *blkbuf = fnode_loc->blkbuf;
	int err = 0, rc, block;

	/** 
	 * 新节点，初始化为0
	 */
	if (lext3_fnode->state & LEXT3_STATE_NEW)
		memset(disk_node, 0, super_to_lext3(fnode->super)->fnode_size);

	disk_node->mode = cpu_to_le16(fnode->mode);
	if(!(lext3_test_opt(fnode->super, LEXT3_MOUNT_NO_UID32))) {
		disk_node->uid_low = cpu_to_le16(low_16_bits(fnode->uid));
		disk_node->gid_low = cpu_to_le16(low_16_bits(fnode->gid));
		if(!lext3_fnode->del_time) {
			disk_node->uid_high = cpu_to_le16(high_16_bits(fnode->uid));
			disk_node->gid_high = cpu_to_le16(high_16_bits(fnode->gid));
		} else {
			disk_node->uid_high = 0;
			disk_node->gid_high = 0;
		}
	} else {
		disk_node->uid_low = cpu_to_le16(fs_high2lowuid(fnode->uid));
		disk_node->gid_low = cpu_to_le16(fs_high2lowgid(fnode->gid));
		disk_node->uid_high = 0;
		disk_node->gid_high = 0;
	}
	disk_node->links_count = cpu_to_le16(fnode->link_count);
	disk_node->file_size = cpu_to_le32(lext3_fnode->filesize_disk);
	disk_node->access_time = cpu_to_le32(fnode->access_time.tv_sec);
	disk_node->meta_modify_time = cpu_to_le32(fnode->meta_modify_time.tv_sec);
	disk_node->data_modify_time = cpu_to_le32(fnode->data_modify_time.tv_sec);
	disk_node->block_count = cpu_to_le32(fnode->block_count);
	disk_node->del_time = cpu_to_le32(lext3_fnode->del_time);
	disk_node->flags = cpu_to_le32(lext3_fnode->flags);
	disk_node->file_acl = cpu_to_le32(lext3_fnode->file_acl);
	if (!S_ISREG(fnode->mode)) {
		disk_node->dir_acl = cpu_to_le32(lext3_fnode->dir_acl);
	} else {
		disk_node->size_high =
			cpu_to_le32(lext3_fnode->filesize_disk >> 32);
		if (lext3_fnode->filesize_disk > 0x7fffffffULL) {
			struct super_block *super = fnode->super;

			if (!LEXT3_HAS_RO_COMPAT_FEATURE(super,
					LEXT3_FEATURE_RO_COMPAT_LARGE_FILE) ||
			    super_to_lext3(super)->phy_super->revision ==
					cpu_to_le32(LEXT3_GOOD_OLD_REV)) {
				err = lext3_journal_get_write_access(handle,
						super_to_lext3(super)->blkbuf_super);
				if (err)
					goto out_loosen;

				lext3_update_revision(super);
				LEXT3_SET_RO_COMPAT_FEATURE(super,
					LEXT3_FEATURE_RO_COMPAT_LARGE_FILE);
				super->dirty = 1;
				handle->sync = 1;
				err = lext3_journal_dirty_metadata(handle,
						super_to_lext3(super)->blkbuf_super);
			}
		}
	}
	disk_node->generation = cpu_to_le32(fnode->generation);
	if (S_ISCHR(fnode->mode) || S_ISBLK(fnode->mode)) {
		disk_node->blocks[0] =
			cpu_to_le32(devno_to_uint(fnode->devno));
		disk_node->blocks[1] = 0;
	} else {
		for (block = 0; block < LEXT3_BLOCK_INDEX_COUNT; block++)
			disk_node->blocks[block] = lext3_fnode->data_blocks[block];
	}

	if (LEXT3_FNODE_SIZE(fnode->super) > LEXT3_OLD_FNODE_SIZE)
		disk_node->extra_fsize = cpu_to_le16(lext3_fnode->extra_fsize);

	rc = lext3_journal_dirty_metadata(handle, blkbuf);
	if (!err)
		err = rc;

	lext3_fnode->state &= ~LEXT3_STATE_NEW;

out_loosen:
	loosen_blkbuf (blkbuf);
	lext3_std_error(fnode->super, err);

	return err;
}

int lext3_mark_fnode_loc_dirty(struct journal_handle *handle,
		struct file_node *fnode, struct lext3_fnode_loc *fnode_loc)
{
	int err = 0;

	hold_blkbuf(fnode_loc->blkbuf);

	/**
	 * 将文件节点中的数据同步到缓冲区
	 * 并置缓冲区为脏
	 */
	err = lext3_update_fnode(handle, fnode, fnode_loc);
	loosen_blkbuf(fnode_loc->blkbuf);
	return err;
}

int lext3_mark_fnode_dirty(struct journal_handle *handle,
					struct file_node *fnode)
{
	struct lext3_fnode_loc fnode_loc;
	int err;

	might_sleep();

	err = lext3_fnode_prepare_write(handle, fnode, &fnode_loc);
	if (!err)
		err = lext3_mark_fnode_loc_dirty(handle, fnode, &fnode_loc);

	return err;
}

/**
 * 判断是否为符号链接，并且链接数据在文件元数据中
 */
int lext3_fnode_is_fast_symlink(struct file_node *fnode)
{
	int acl_blocks = fnode_to_lext3(fnode)->file_acl ?
		(fnode->super->block_size >> 9) : 0;

	return (S_ISLNK(fnode->mode) && fnode->block_count - acl_blocks == 0);
}

/**
 * 计算截断日志所需要的日志空间
 */
unsigned long lext3_journal_blocks_truncate(struct file_node *fnode)
{
	unsigned long needed;

	needed = fnode->block_count >> (fnode->super->block_size_order - 9);

	if (needed < 2)
		needed = 2;

	if (needed > LEXT3_MAX_TRANS_DATA) 
		needed = LEXT3_MAX_TRANS_DATA;

	return LEXT3_DATA_TRANS_BLOCKS + needed;
}

/**
 * 找到文件节点元数据块的编号
 */
static unsigned long get_fnode_metablock(struct super_block *super,
		unsigned long fnode_num, struct lext3_fnode_loc *fnode_loc)
{
	unsigned long desc, group_desc, block_group;
	unsigned long offset, block;
	struct lext3_group_desc *blkgroup;
	struct blkbuf_desc *blkbuf;
	unsigned long fnode_count;

	fnode_count = le32_to_cpu(super_to_lext3(super)->phy_super->fnode_count);
	if ((fnode_num != LEXT3_ROOT_FILENO && fnode_num != LEXT3_JOURNAL_FILENO &&
	    fnode_num != LEXT3_RESIZE_FILENO && fnode_num < LEXT3_FIRST_FILENO(super))
	    || fnode_num > fnode_count) {
		lext3_enconter_error (super, "bad file node number: %lu", fnode_num);
		return 0;
	}

	block_group = (fnode_num - 1) / LEXT3_FNODES_PER_GROUP(super);
	if (block_group >= super_to_lext3(super)->groups_count) {
		lext3_enconter_error(super, "bad group num: %lu, max groups count: %lu",
			block_group, super_to_lext3(super)->groups_count);
		return 0;
	}

	smp_rmb();
	group_desc = block_group >> EXT3_DESC_PER_BLOCK_BITS(super);
	desc = block_group & (LEXT3_DESC_PER_BLOCK(super) - 1);
	blkbuf = super_to_lext3(super)->blkbuf_grpdesc[group_desc];
	if (!blkbuf) {
		lext3_enconter_error (super, "Descriptor not loaded");
		return 0;
	}

	blkgroup = (struct lext3_group_desc *)blkbuf->block_data;
	offset = ((fnode_num - 1) % LEXT3_FNODES_PER_GROUP(super)) *
		LEXT3_FNODE_SIZE(super);
	block = le32_to_cpu(blkgroup[desc].first_fnode_block) +
		(offset >> super->block_size_order);

	fnode_loc->block_group = block_group;
	fnode_loc->offset = offset & (LEXT3_BLOCK_SIZE(super) - 1);

	return block;
}

/**
 * 获得文件节点的位置信息
 */
int lext3_load_fnode(struct file_node *fnode,
	struct lext3_fnode_loc *fnode_loc, int in_mem)
{
	struct blkbuf_desc *blkbuf;
	unsigned long block;

	/**
	 * 得到文件节点元数据块的编号
	 */
	block = get_fnode_metablock(fnode->super, fnode->node_num, fnode_loc);
	if (!block)
		return -EIO;

	/**
	 * 在内存中查找块是否存在，如果不存在就分配一个
	 */
	blkbuf = blkbuf_find_alloc(fnode->super, block);
	if (!blkbuf) {
		lext3_enconter_error (fnode->super, "unable to read file_node block - "
			"file_node=%lu, block=%lu", fnode->node_num, block);
		return -EIO;
	}

	/**
	 * 如果还没有从磁盘上读取内容，则读取
	 */
	if (!blkbuf_is_uptodate(blkbuf)) {
		blkbuf_lock(blkbuf);
		if (blkbuf_is_uptodate(blkbuf)) {
			/**
			 * 幸运，其他人正好读了数据
			 */
			blkbuf_unlock(blkbuf);
			goto has_buffer;
		}

		if (in_mem) {
			struct blkbuf_desc *bitmap_bh;
			struct lext3_group_desc *desc;
			int inodes_per_buffer;
			int inode_offset, i;
			int block_group;
			int start;

			block_group = (fnode->node_num - 1) /
					LEXT3_FNODES_PER_GROUP(fnode->super);
			inodes_per_buffer = blkbuf->size /
				LEXT3_FNODE_SIZE(fnode->super);
			inode_offset = ((fnode->node_num - 1) %
					LEXT3_FNODES_PER_GROUP(fnode->super));
			start = inode_offset & ~(inodes_per_buffer - 1);

			desc = lext3_get_group_desc(fnode->super,
						block_group, NULL);
			if (!desc)
				goto make_io;

			bitmap_bh = blkbuf_find_alloc(fnode->super,
					le32_to_cpu(desc->fnode_bitmap));
			if (!bitmap_bh)
				goto make_io;
			if (!blkbuf_is_uptodate(bitmap_bh)) {
				loosen_blkbuf(bitmap_bh);
				goto make_io;
			}

			for (i = start; i < start + inodes_per_buffer; i++) {
				if (i == inode_offset)
					continue;
				if (lext3_test_bit(i, bitmap_bh->block_data))
					break;
			}
			loosen_blkbuf(bitmap_bh);

			/**
			 * 在同一个磁盘块上的文件节点都是空闲的
			 */
			if (i == start + inodes_per_buffer) { 
				memset(blkbuf->block_data, 0, blkbuf->size);
				blkbuf_set_uptodate(blkbuf);
				blkbuf_unlock(blkbuf);
				goto has_buffer;
			}
		}

make_io:
		/**
		 * 读文件节点所在的元数据块
		 */
		hold_blkbuf(blkbuf);
		blkbuf->finish_io = blkbuf_finish_read_block;
		submit_block_request(READ, blkbuf);
		blkbuf_wait_unlock(blkbuf);
		/**
		 * 遇到错误了
		 */
		if (!blkbuf_is_uptodate(blkbuf)) {
			lext3_enconter_error(fnode->super,
				"unable to read file_node block - "
				"file_node=%lu, block=%lu",
				fnode->node_num, block);
			loosen_blkbuf(blkbuf);
			return -EIO;
		}
	}

has_buffer:
	fnode_loc->blkbuf = blkbuf;

	return 0;
}

static int get_fnode_location(struct file_node *fnode,
	struct lext3_fnode_loc *fnode_loc)
{
	return lext3_load_fnode(fnode, fnode_loc,
		!(fnode_to_lext3(fnode)->state & LEXT3_STATE_XATTR));
}

/**
 * 准备开始写文件节点元数据
 */
int lext3_fnode_prepare_write(struct journal_handle *handle,
	struct file_node *fnode, struct lext3_fnode_loc *fnode_loc)
{
	int err = 0;

	if (handle) {
		err = get_fnode_location(fnode, fnode_loc);
		if (!err) {
			err = lext3_journal_get_write_access(handle, fnode_loc->blkbuf);
			if (err) {
				loosen_blkbuf(fnode_loc->blkbuf);
				fnode_loc->blkbuf = NULL;
			}
		}
	}

	lext3_std_error(fnode->super, err);

	return err;
}

static inline void set_indblock_desc(struct indirect_block_desc *ind_desc,
	struct blkbuf_desc *blkbuf, __le32 *v)
{
	ind_desc->child_pos = v;
	ind_desc->child_block_num = *v;
	ind_desc->blkbuf = blkbuf;
}

static inline int check_indblock_desc(struct indirect_block_desc *from,
	struct indirect_block_desc *to)
{
	while (from <= to && from->child_block_num == *from->child_pos)
		from++;
	return (from > to);
}

/**
 * 读取间接块链
 */
struct indirect_block_desc *
lext3_read_branch(struct file_node *fnode, int depth, int *offsets,
	struct indirect_block_desc chain[4], int *err)
{
	struct super_block *super = fnode->super;
	struct indirect_block_desc *ind_desc = chain;
	struct blkbuf_desc *blkbuf;

	*err = 0;
	/**
	 * 第一级是文件节点!
	 */
	set_indblock_desc(chain, NULL,
		fnode_to_lext3(fnode)->data_blocks + *offsets);
	/**
	 * 逻辑块号不正常，没有下级块号，只能退出了
	 */
	if (!ind_desc->child_block_num)
		goto no_block;

	/**
	 * 遍历每一层的间接块编号
	 */
	while (--depth) {
		/**
		 * 读取间接块内容到内存
		 */
		blkbuf = blkbuf_read_block(super,
			le32_to_cpu(ind_desc->child_block_num));
		if (!blkbuf)
			goto failure;

		/**
		 * 检查是否间接块发生了变化
		 */
		if (!check_indblock_desc(chain, ind_desc))
			goto changed;
		set_indblock_desc(++ind_desc, blkbuf, (__le32*)blkbuf->block_data + *++offsets);

		/**
		 * 没有下层了
		 */
		if (!ind_desc->child_block_num)
			goto no_block;
	}

	return NULL;

changed:
	loosen_blkbuf(blkbuf);
	*err = -EAGAIN;
	goto no_block;

failure:
	*err = -EIO;

no_block:
	return ind_desc;
}

/**
 * 为文件节点块找到一个邻近的磁盘块
 * 尽量从此块开始为节点分配空间
 */
static int find_near_block(struct file_node *fnode, long block, struct indirect_block_desc chain[4],
			  struct indirect_block_desc *partial, unsigned long *goal)
{
	struct lext3_file_node *lext3_fnode = fnode_to_lext3(fnode);

	if ((block == lext3_fnode->last_logic_block + 1)
	    && lext3_fnode->last_phy_block) {
		lext3_fnode->last_logic_block++;
		lext3_fnode->last_phy_block++;
	}

	if (check_indblock_desc(chain, partial)) {
		/*
		 * 尽量从上一次分配的后面开始分配
		 */
		if (block == lext3_fnode->last_logic_block)
			*goal = lext3_fnode->last_phy_block;

		if (!*goal) {
			__le32 *start;
			__le32 *p;

			if (partial->blkbuf)
				start =  (__le32*) partial->blkbuf->block_data;
			else
				start = lext3_fnode->data_blocks;

			/**
			 * 在间接块或者元数据中查找相邻块的位置
			 * 尽量与这些块相邻
			 */
			for (p = partial->child_pos - 1; p >= start; p--)
				if (*p) {
					*goal = le32_to_cpu(*p);
					return 0;
				}

			/**
			 * 如果有间接块，则尽量与间接块相邻
			 */
			if (!*goal && partial->blkbuf)
				*goal = partial->blkbuf->block_num_dev;
			else {
				struct lext3_superblock_phy *phy_super;
				unsigned long group_start, data_start;
				unsigned long colour;
				/**
				 * 没有间接块，则尽量与文件节点相邻
				 */
				group_start = lext3_fnode->block_group_num
					* LEXT3_BLOCKS_PER_GROUP(fnode->super);
				phy_super = super_to_lext3(fnode->super)->phy_super;
				data_start = le32_to_cpu(phy_super->first_data_block)
					+ group_start;
				colour = (current->pid % 16) *
						(LEXT3_BLOCKS_PER_GROUP(fnode->super) / 16);
				*goal = data_start + colour;
			}
		}

		return 0;
	}

	return -EAGAIN;
}

static int
alloc_branch(struct journal_handle *handle, struct file_node *fnode,
	int num, unsigned long goal, int *offsets,
	struct indirect_block_desc *branch)
{
	int level = 0, keys = 0;
	int blocksize;
	int err = 0;
	int parent;
	int i;

	blocksize = fnode->super->block_size;
	parent = lext3_alloc_block(handle, fnode, goal, &err);
	branch[0].child_block_num = cpu_to_le32(parent);

	if (parent) {
		/**
		 * 依次处理各层
		 */
		for (level = 1; level < num; level++) {
			struct blkbuf_desc *blkbuf;
			int block;
	
			/**
			 * 为每一层分配磁盘块
			 */
			block = lext3_alloc_block(handle, fnode, parent, &err);
			if (!block)
				break;

			branch[level].child_block_num= cpu_to_le32(block);
			keys = level + 1;

			blkbuf = blkbuf_find_alloc(fnode->super, parent);
			branch[level].blkbuf = blkbuf;
			blkbuf_lock(blkbuf);
			err = lext3_journal_get_create_access(handle, blkbuf);
			if (err) {
				blkbuf_unlock(blkbuf);
				loosen_blkbuf(blkbuf);
				break;
			}

			/**
			 * 将新分配的上层块清0
			 * 并指向刚分配的块
			 */
			memset(blkbuf->block_data, 0, blocksize);
			branch[level].child_pos= (__le32*) blkbuf->block_data + offsets[level];
			*branch[level].child_pos = branch[level].child_block_num;
			blkbuf_set_uptodate(blkbuf);
			blkbuf_unlock(blkbuf);

			/**
			 * 上层块当然是元数据了，置脏
			 */
			err = lext3_journal_dirty_metadata(handle, blkbuf);
			if (err)
				break;

			parent = block;
		}
	}

	if (level == num)
		return 0;

	/**
	 * 分配失败
	 * 通知日志系统，将刚分配的块作废
	 */
	for (i = 1; i < keys; i++) {
		lext3_journal_forget(handle, branch[i].blkbuf);
	}

	for (i = 0; i < keys; i++)
		lext3_free_blocks(handle, fnode,
			le32_to_cpu(branch[i].child_block_num), 1);

	return err;
}

/**
 * 将新分配的间接块与文件节点绑定起来
 */
static int
stick_branch(struct journal_handle *handle, struct file_node *fnode,
	long block, struct indirect_block_desc chain[4],
	struct indirect_block_desc *where, int num)
{
	struct lext3_file_node *lext3_fnode;
	int err = 0;
	int i;

	lext3_fnode = fnode_to_lext3(fnode);
	/**
	 * 如果就关联到间接块，就要获得间接块的日志权限
	 */
	if (where->blkbuf) {
		err = lext3_journal_get_write_access(handle, where->blkbuf);
		if (err)
			goto err_out;
	}

	/**
	 * 在truncate锁的保护下，验证间接块链仍然有效
	 */
	if (!check_indblock_desc(chain, where-1) || *where->child_pos)
		goto changed;

	*where->child_pos = where->child_block_num;
	lext3_fnode->last_logic_block = block;
	lext3_fnode->last_phy_block = le32_to_cpu(where[num-1].child_block_num);

	fnode->meta_modify_time = CURRENT_TIME_SEC;
	lext3_mark_fnode_dirty(handle, fnode);

	/**
	 * 关联到间接接，需要置间接块脏标记
	 */
	if (where->blkbuf) {
		err = lext3_journal_dirty_metadata(handle, where->blkbuf);
		if (err) 
			goto err_out;
	}

	return err;

changed:
	err = -EAGAIN;

err_out:
	for (i = 1; i < num; i++)
		lext3_journal_forget(handle, where[i].blkbuf);

	if (err == -EAGAIN)
		for (i = 0; i < num; i++)
			lext3_free_blocks(handle, fnode, 
					 le32_to_cpu(where[i].child_block_num), 1);

	return err;
}

/**
 * 查找或者创建文件节点的逻辑块
 */
int lext3_find_block(struct journal_handle *handle, struct file_node *fnode,
	sector_t iblock, struct blkbuf_desc *blkbuf, int create, int extend)
{
	struct lext3_file_node *lext3_fnode;
	unsigned long goal;
	int boundary = 0;
	struct indirect_block_desc chain[4];
	struct indirect_block_desc *partial;
	int err = -EIO;
	int offsets[4];
	int depth;
	int left;

	ASSERT(handle || !create);

	depth = lext3_block_to_path(fnode, iblock, offsets, &boundary);
	lext3_fnode = fnode_to_lext3(fnode);

	/**
	 * 参数不正确
	 */
	if (depth == 0)
		goto out;

reread:
	partial = lext3_read_branch(fnode, depth, offsets, chain, &err);

	/**
	 * 逻辑块存在，直接返回
	 */
	if (!partial) {
		blkbuf_clear_new(blkbuf);
		goto got_it;
	}

	/**
	 * 仅仅是搜索，或者有错误
	 */
	if (!create || err == -EIO)
		goto cleanup;

	/**
	 * 没有truncate_sem的保护，间接块信息产生了变化，重试
	 */
	if (err == -EAGAIN)
		goto changed;

	goal = 0;
	down(&lext3_fnode->truncate_sem);
	/**
	 * 节点间接块发生变化，重试
	 */
	if (find_near_block(fnode, iblock, chain, partial, &goal) < 0) {
		up(&lext3_fnode->truncate_sem);
		goto changed;
	}

	left = (chain + depth) - partial;
	/**
	 * 为逻辑块创建中间间接块
	 */
	err = alloc_branch(handle, fnode, left, goal,
		offsets + (partial - chain), partial);

	/**
	 * 将已经分配的间接块与文件节点关联起来 
	 */
	if (!err)
		err = stick_branch(handle, fnode, iblock, chain, partial, left);

	if (!err && extend && fnode->file_size > lext3_fnode->filesize_disk)
		lext3_fnode->filesize_disk = fnode->file_size;

	up(&lext3_fnode->truncate_sem);

	if (err == -EAGAIN)
		goto changed;

	if (err)
		goto cleanup;

	/**
	 * 新分配的块
	 */
	blkbuf_set_new(blkbuf);

	goto got_it;

changed:
	while (partial > chain) {
		loosen_blkbuf(partial->blkbuf);
		partial--;
	}
	goto reread;

got_it:
	blkbuf_set_map_data(blkbuf, fnode->super,
		le32_to_cpu(chain[depth - 1].child_block_num));
	if (boundary)
		blkbuf_set_boundary(blkbuf);

	partial = chain + depth - 1;

cleanup:
	while (partial > chain) {
		loosen_blkbuf(partial->blkbuf);
		partial--;
	}
out:
	return err;
}

/**
 * 为普通文件分配数据块
 */
int lext3_get_datablock(struct file_node *fnode, sector_t iblock,
	struct blkbuf_desc *blkbuf, int create)
{
	struct journal_handle *handle = NULL;
	int ret;

	if (create) {
		handle = lext3_get_journal_handle();
		ASSERT(handle);
	}

	ret = lext3_find_block(handle, fnode, iblock, blkbuf, create, 1);

	return ret;
}

int lext3_get_datablock_direct(struct file_node *fnode, sector_t iblock,
	unsigned long max_blocks, struct blkbuf_desc *bh_result, int create)
{
	struct journal_handle *handle;
	int ret = 0;

	handle = journal_current_handle();
	if (!handle)
		goto get_block;

	if (handle->transaction->state == TRANS_PREPARE_COMMIT) {
		lext3_stop_journal(handle);
		handle = lext3_start_journal(fnode, DIO_CREDITS);
		if (IS_ERR(handle))
			ret = PTR_ERR(handle);
		goto get_block;
	}

	if (handle->block_credits <= EXT3_RESERVE_TRANS_BLOCKS) {
		ret = journal_extend(handle, DIO_CREDITS);
		if (ret > 0) {
			ret = lext3_restart_journal(handle, DIO_CREDITS);
		}
	}

get_block:
	if (ret == 0)
		ret = lext3_find_block(handle, fnode, iblock,
					bh_result, create, 0);
	bh_result->size = (1 << fnode->block_size_order);

	return ret;
}

/**
 * 获得文件元数据块的位置，如目录节点块
 * 可能会分配新块
 */
struct blkbuf_desc *
lext3_get_metablock(struct journal_handle *handle, struct file_node *fnode,
	long block, int create, int *errp)
{
	struct blkbuf_desc dummy;
	int fatal = 0, err;

	ASSERT(handle != NULL || create == 0);

	dummy.state = 0;
	dummy.block_num_dev = 0;
	/**
	 * 在磁盘上查找或者创建节点
	 */
	*errp = lext3_find_block(handle, fnode, block, &dummy, create, 1);
	if (!*errp && blkbuf_is_mapped(&dummy)) {
		struct blkbuf_desc *blkbuf;

		/**
		 * 查找逻辑块缓冲区
		 */
		blkbuf = blkbuf_find_alloc(fnode->super, dummy.block_num_dev);
		/**
		 *  新分配的块
		 */
		if (blkbuf_is_new(&dummy)) {
			ASSERT(create != 0);
			ASSERT(handle != 0);

			blkbuf_lock(blkbuf);
			/**
			 * 新的元数据块，获得其日志权限
			 */
			fatal = lext3_journal_get_create_access(handle, blkbuf);
			if (!fatal && !blkbuf_is_uptodate(blkbuf)) {
				memset(blkbuf->block_data, 0, fnode->super->block_size);
				blkbuf_set_uptodate(blkbuf);
			}
			blkbuf_unlock(blkbuf);
			err = lext3_journal_dirty_metadata(handle, blkbuf);
			if (!fatal)
				fatal = err;
		}

		if (fatal) {
			*errp = fatal;
			loosen_blkbuf(blkbuf);
			blkbuf = NULL;
		}

		return blkbuf;
	}

	return NULL;
}

/**
 * 读取逻辑块(元数据，如目录)的内容
 */
struct blkbuf_desc *lext3_read_metablock(struct journal_handle *handle,
	struct file_node *fnode, int block, int create, int *err)
{
	struct blkbuf_desc * blkbuf;

	blkbuf = lext3_get_metablock(handle, fnode, block, create, err);
	if (!blkbuf)
		return blkbuf;

	/**
	 * 块内容已经是最新的了
	 */
	if (blkbuf_is_uptodate(blkbuf))
		return blkbuf;

	/**
	 * 提交读请求并等待IO结束
	 */
	submit_block_requests(READ, 1, &blkbuf);
	blkbuf_wait_unlock(blkbuf);
	if (blkbuf_is_uptodate(blkbuf))
		return blkbuf;

	/**
	 * 读失败
	 */
	loosen_blkbuf(blkbuf);
	*err = -EIO;

	return NULL;
}

void lext3_set_fnode_flags(struct file_node *fnode)
{
	unsigned int flags = fnode_to_lext3(fnode)->flags;

	fnode->flags &= ~(S_SYNC|S_APPEND|S_IMMUTABLE|S_NOATIME|S_DIRSYNC);
	if (flags & LEXT3_SYNC_FL)
		fnode->flags |= S_SYNC;
	if (flags & LEXT3_APPEND_FL)
		fnode->flags |= S_APPEND;
	if (flags & LEXT3_IMMUTABLE_FL)
		fnode->flags |= S_IMMUTABLE;
	if (flags & LEXT3_NOATIME_FL)
		fnode->flags |= S_NOATIME;
	if (flags & LEXT3_DIRSYNC_FL)
		fnode->flags |= S_DIRSYNC;
}
