#include <dim-sum/aio.h>
#include <dim-sum/beehive.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/block_buf.h>
#include <dim-sum/err.h>
#include <dim-sum/fs.h>
#include <dim-sum/lext3_fs.h>
#include <dim-sum/highmem.h>
#include <dim-sum/journal.h>
#include <dim-sum/pagemap.h>
#include <dim-sum/rbtree.h>
#include <dim-sum/stacktrace.h>
#include <dim-sum/timer.h>
#include <dim-sum/uio.h>

#include "internal.h"

#define NAMEI_RA_CHUNKS  2
#define NAMEI_RA_BLOCKS  4
#define NAMEI_RA_SIZE        (NAMEI_RA_CHUNKS * NAMEI_RA_BLOCKS)
#define NAMEI_RA_INDEX(c,b)  (((c) * NAMEI_RA_BLOCKS) + (b))

static inline __le32 *parent_node_num(char *buffer)
{
	struct lext3_dir_item *parent, *self;
	int len;

	self = (struct lext3_dir_item *)buffer;
	len = self->rec_len;

	parent = (struct lext3_dir_item *)((char *)self + len);

	return &parent->file_node_num;
}

static int file_equal (int len, char *name, struct lext3_dir_item *dir_item)
{
	if (len != dir_item->name_len)
		return 0;

	if (!dir_item->file_node_num)
		return 0;

	return !memcmp(name, dir_item->name, len);
}

static bool dir_item_stale(struct lext3_dir_item *dir_item,
	struct file_node *fnode, struct filenode_cache *cache)
{
	int cmp_ret;

	if (le32_to_cpu(dir_item->file_node_num) != fnode->node_num)
		return true;

	if (dir_item->name_len != cache->file_name.len)
		return true;

	cmp_ret = strncmp(dir_item->name, cache->file_name.name,
		dir_item->name_len);

	return cmp_ret;
}

/**
 * 在目录块中，插入一个目录项
 */
static int add_to_blkbuf(struct journal_handle *handle,
	struct filenode_cache *fnode_cache, struct file_node *fnode,
	struct lext3_dir_item *dir_item, struct blkbuf_desc *blkbuf)
{
	unsigned long	offset = 0;
	struct file_node *dir;
	unsigned short req_len;
	int name_len;
	char	*name;
	char *end;
	int err;

	dir = fnode_cache->parent->file_node;
	name = fnode_cache->file_name.name;
	name_len = fnode_cache->file_name.len;
	req_len = lext3_dir_item_size(name_len);

	/**
	 * 调用者希望我们查找合适的空间
	 */
	if (!dir_item) {
		int valid;
		int free_len;

		dir_item = first_dir_item(blkbuf);
		end = blkbuf->block_data + dir->super->block_size - req_len;

		while ((char *)dir_item <= end) {
			valid = lext3_verify_dir_item("add_dir_item", dir, dir_item,
				blkbuf, offset);
			if (!valid) {
				loosen_blkbuf (blkbuf);
				return -EIO;
			}

			if (file_equal (name_len, name, dir_item)) {
				loosen_blkbuf (blkbuf);
				return -EEXIST;
			}

			if (dir_item->file_node_num)
				free_len = le16_to_cpu(dir_item->rec_len)
							- lext3_dir_item_size(dir_item->name_len);
			else
				free_len = le16_to_cpu(dir_item->rec_len);
			/**
			 * 节点空闲空间可以容纳新节点
			 */
			if (free_len >= req_len)
				break;

			/**
			 * 移动到下一个节点
			 */
			dir_item = next_dir_item(dir_item);
			offset += le16_to_cpu(dir_item->rec_len);
		}

		if ((char *)dir_item > end)
			return -ENOSPC;
	}

	/**
	 * 找到合适的地方来容纳新节点
	 * 首先获得缓冲块的日志写权限
	 */
	err = lext3_journal_get_write_access(handle, blkbuf);
	if (err) {
		lext3_std_error(dir->super, err);
		loosen_blkbuf(blkbuf);
		return err;
	}

	/**
	 * 当前节点有效，包含了有效的文件节点
	 */
	if (dir_item->file_node_num) {
		struct lext3_dir_item *new_item;
		int real_len, rec_len;

		/**
		 * 将当前节点一分为二
		 */
		real_len = lext3_dir_item_size(dir_item->name_len);
		rec_len = le16_to_cpu(dir_item->rec_len);
		dir_item->rec_len = cpu_to_le16(real_len);
		new_item = (struct lext3_dir_item *)((char *)dir_item + real_len);
		new_item->rec_len = cpu_to_le16(rec_len - real_len);
		dir_item = new_item;
	}

	dir_item->file_type = LEXT3_FT_UNKNOWN;
	if (fnode) {
		dir_item->file_node_num = cpu_to_le32(fnode->node_num);
		diritem_set_filetype(dir->super, dir_item, fnode->mode);
	} else
		dir_item->file_node_num = 0;
	dir_item->name_len = name_len;
	memcpy (dir_item->name, name, name_len);

	dir->data_modify_time = dir->meta_modify_time = CURRENT_TIME_SEC;
	set_fnode_dir_index(dir);
	dir->version++;

	lext3_mark_fnode_dirty(handle, dir);
	err = lext3_journal_dirty_metadata(handle, blkbuf);
	if (err)
		lext3_std_error(dir->super, err);

	loosen_blkbuf(blkbuf);

	return 0;
}

/**
 * 为目录创建一个元数据块
 */
static struct blkbuf_desc *create_metablock(struct journal_handle *handle,
	struct file_node *fnode, u32 *block, int *err)
{
	struct blkbuf_desc *blkbuf;

	*block = fnode->file_size >> fnode->super->block_size_order;

	blkbuf = lext3_read_metablock(handle, fnode, *block, 1, err);
	if (blkbuf) {
		fnode->file_size += fnode->super->block_size;
		fnode_to_lext3(fnode)->filesize_disk = fnode->file_size;
		/**
		 * 接下来要写元数据块，先获得其日志权限
		 */
		lext3_journal_get_write_access(handle,blkbuf);
	}

	return blkbuf;
}

/**
 * 在目录的数据块中，增加一个目录项
 */
static int add_dir_item (struct journal_handle *handle,
	struct filenode_cache *fnode_cache, struct file_node *fnode)
{
	struct lext3_dir_item *dir_item;
	struct blkbuf_desc *blkbuf;
	struct super_block *super;
	u32 block, block_count;
	struct file_node *dir;
	unsigned block_size;
	int ret;

	dir = fnode_cache->parent->file_node;
	super = dir->super;
	block_size = super->block_size;

	if (!fnode_cache->file_name.len)
		return -EINVAL;

	block_count = dir->file_size >> super->block_size_order;
	for (block = 0; block < block_count; block++) {
		blkbuf = lext3_read_metablock(handle, dir, block, 0, &ret);
		if(!blkbuf)
			return ret;

		/**
		 * 试图在当前块中插入一个目录项
		 */
		ret = add_to_blkbuf(handle, fnode_cache, fnode, NULL, blkbuf);
		if (ret != -ENOSPC)
			return ret;

		loosen_blkbuf(blkbuf);
	}

	/**
	 * 所有块都已经满了，创建一个新块
	 */
	blkbuf = create_metablock(handle, dir, &block, &ret);
	if (!blkbuf)
		return ret;

	dir_item = first_dir_item(blkbuf);
	dir_item->file_node_num = 0;
	dir_item->rec_len = cpu_to_le16(block_size);

	return add_to_blkbuf(handle, fnode_cache, fnode, dir_item, blkbuf);
}

/**
 * 将文件节点与其父目录绑定
 */
static int dir_item_stick(struct journal_handle *handle,
	struct filenode_cache *fnode_cache, struct file_node *fnode)
{
	int err;

	err = add_dir_item(handle, fnode_cache, fnode);
	if (!err) {
		lext3_mark_fnode_dirty(handle, fnode);
		fnode_cache_stick(fnode_cache, fnode);
		return 0;
	}

	fnode->link_count--;
	loosen_file_node(fnode);

	return err;
}

/**
 * 将目录项删除
 * 这里通过将目录项与前面的项合并完成的
 */
static int
dir_item_delete (struct journal_handle *handle, struct file_node *dir,
	struct lext3_dir_item *item_del, struct blkbuf_desc *blkbuf)
{
	struct lext3_dir_item *dir_item, *prev_item;
	int offset;
	int res;

	offset = 0;
	prev_item = NULL;
	dir_item = first_dir_item(blkbuf);
	while (offset < blkbuf->size) {
		res = lext3_verify_dir_item("dir_item_delete",
			dir, dir_item, blkbuf, offset);
		if (!res)
			return -EIO;

		/**
		 * 在块中找到待删除的目录项
		 */
		if (dir_item == item_del) {
			/**
			 * 获得逻辑块的日志写权限
			 */
			lext3_journal_get_write_access(handle, blkbuf);
			if (prev_item) {
				int item_len = le16_to_cpu(prev_item->rec_len) +
						    le16_to_cpu(dir_item->rec_len);

				/**
				 * 与前一个目录项合并
				 * 前一个目录项将后面一个目录吞并了
				 */
				prev_item->rec_len = cpu_to_le16(item_len);
			}
			else
				/**
				 * 要删除的目录项是第一个
				 * 直接将其文件节点编号设置为0即可
				 */
				dir_item->file_node_num = 0;
			dir->version++;
			lext3_journal_dirty_metadata(handle, blkbuf);

			return 0;
		}
		offset += le16_to_cpu(dir_item->rec_len);
		prev_item = dir_item;
		dir_item = next_dir_item(dir_item);
	}

	return -ENOENT;
}

/**
 * create回调，创建普通文件
 */
static int
lext3_create (struct file_node *dir, struct filenode_cache *fnode_cache,
	int mode, struct filenode_lookup_s *look)
{
	struct journal_handle *handle; 
	struct file_node * fnode;
	int ret, retries = 0;

retry:
	/**
	 * 获得原子操作描述符
	 */
	handle = lext3_start_journal(dir, LEXT3_DATA_TRANS_BLOCKS +
		LEXT3_INDEX_EXTRA_TRANS_BLOCKS + 3 + 2 * LEXT3_QUOTA_INIT_BLOCKS);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		handle->sync = 1;

	/**
	 * 创建一个空的文件节点
	 */
	fnode = lext3_alloc_fnode(handle, dir, mode);
	ret = PTR_ERR(fnode);
	if (!IS_ERR(fnode)) {
		/**
		 * 设置文件操作回调函数
		 */
		fnode->node_ops = &lext3_file_fnode_ops;
		fnode->file_ops = &lext3_file_ops;
		lext3_init_cachespace(fnode);
		/**
		 * 将目录项与文件节点关联起来
		 */
		ret = dir_item_stick(handle, fnode_cache, fnode);
	}
	lext3_stop_journal(handle);
	if (ret == -ENOSPC && lext3_should_retry_alloc(dir->super, &retries))
		goto retry;

	return ret;
}

/**
 * 在磁盘块缓冲区中，查找特定名称的文件名
 */
static int find_in_block(struct blkbuf_desc *blkbuf, struct file_node *dir,
	struct filenode_cache *fnode_cache, unsigned long offset,
	struct lext3_dir_item **res_dir)
{
	struct lext3_dir_item *dir_item;
	char *blkbuf_end;
	int name_len;
	int item_len;
	char *name;

	blkbuf_end = blkbuf->block_data + dir->super->block_size;
	dir_item = (struct lext3_dir_item *) blkbuf->block_data;
	name = fnode_cache->file_name.name;
	name_len = fnode_cache->file_name.len;

	/**
	 * 遍历块缓冲区，查找每一个目录项
	 */
	while ((char *)dir_item < blkbuf_end) {
		/**
		 * 防止磁盘坏数据引起死循环
		 */
		item_len = le16_to_cpu(dir_item->rec_len);
		if (item_len <= 0)
			return -1;

		if ((char *)next_dir_item(dir_item) <= blkbuf_end &&
		    file_equal (name_len, name, dir_item)) {
			/**
			 * 验证目录项是否正确
			 */
			if (!lext3_verify_dir_item("find_in_dir", dir, dir_item,
			    blkbuf, offset))
				return -1;

			/**
			 * 文件名称匹配，并且目录项有效
			 */
			*res_dir = dir_item;
			return 1;
		}

		offset += item_len;
		dir_item = next_dir_item(dir_item);
	}

	return 0;
}

/**
 * 在目录中搜索特定的文件
 */
static struct blkbuf_desc *find_in_dir(struct filenode_cache *fnode_cache,
	struct lext3_dir_item **res)
{
	struct blkbuf_desc *blkbuf_ra[NAMEI_RA_SIZE];
	struct blkbuf_desc * blkbuf, *ret = NULL;
	unsigned long lookup_start, block, ra_block;
	int block_count, found, err;
	struct super_block *super;
	struct file_node *dir;
	int ra_count = 0;
	int ra_idx = 0;
	int num = 0;

	*res = NULL;
	dir = fnode_cache->parent->file_node;
	super = dir->super;

	if (fnode_cache->file_name.len > LEXT3_NAME_LEN)
		return NULL;

	block_count = dir->file_size >> super->block_size_order;
	lookup_start = fnode_to_lext3(dir)->lookup_start;
	if (lookup_start >= block_count)
		lookup_start = 0;
	block = lookup_start;

restart:
	do {
		/**
		 * 需要预读一部分数据块
		 */
		if (ra_idx >= ra_count) {
			ra_idx = 0;
			ra_count = 0;
			ra_block = block;
			while (ra_block < block_count && ra_count < NAMEI_RA_SIZE) {
				if (num && block == lookup_start) {
					blkbuf_ra[ra_count] = NULL;
					break;
				}

				/**
				 * 读取逻辑块的内容
				 */
				blkbuf = lext3_get_metablock(NULL, dir, ra_block, 0, &err);
				blkbuf_ra[ra_count] = blkbuf;
				/**
				 * 逻辑块在磁盘上存在
				 * 提交预读请求
				 */
				if (blkbuf)
					submit_block_requests(READ, 1, &blkbuf);

				ra_block++;
				num++;
				ra_count++;
			}
		}

		if ((blkbuf = blkbuf_ra[ra_idx++]) == NULL)
			goto next_block;

		/**
		 * 等待当前块读取完毕
		 */
		blkbuf_wait_unlock(blkbuf);
		/**
		 * 然而读取过程中出错了
		 */
		if (!blkbuf_is_uptodate(blkbuf)) {
			lext3_enconter_error(super, "reading directory #%lu "
				"offset %lu", dir->node_num, block);
			loosen_blkbuf(blkbuf);
			goto next_block;
		}

		found = find_in_block(blkbuf, dir, fnode_cache,
			    block << super->block_size_order, res);
		/**
		 * 找到了
		 */
		if (found == 1) {
			/**
			 * 设置下次搜索的起始块，保持热度:)
			 */
			fnode_to_lext3(dir)->lookup_start = block;
			ret = blkbuf;
			goto out;
		} else {
			loosen_blkbuf(blkbuf);
			/**
			 * 遇到严重错误，不得不退
			 */
			if (found < 0)
				goto out;
		}

next_block:
		if (++block >= block_count)
			block = 0;
	} while (block != lookup_start);

	/**
	 * 目录大小变化，重新搜索
	 */
	if ((block_count << super->block_size_order) < dir->file_size) {
		lookup_start = 0;
		goto restart;
	}

out:
	/**
	 * 前面的块已经被引用或者释放掉了
	 */
	for (; ra_idx < ra_count; ra_idx++)
		loosen_blkbuf (blkbuf_ra[ra_idx]);

	return ret;
}

/**
 * lookup回调
 */
static struct filenode_cache *
lext3_lookup(struct file_node *dir, struct filenode_cache *fnode_cache,
	struct filenode_lookup_s *look)
{
	struct lext3_dir_item *dir_item;
	struct blkbuf_desc *blkbuf;
	struct file_node *fnode;

	/**
	 * 要查找的文件名称太长，在目录中一定不存在
	 */
	if (fnode_cache->file_name.len > LEXT3_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	blkbuf = find_in_dir(fnode_cache, &dir_item);
	fnode = NULL;
	if (blkbuf) {
		unsigned long fnode_num;

		fnode_num = le32_to_cpu(dir_item->file_node_num);
		loosen_blkbuf (blkbuf);

		/**
		 * 根据文件节点的编号，读取文件节点到内存中
		 */
		fnode = fnode_read(dir->super, fnode_num);
		if (!fnode)
			return ERR_PTR(-EACCES);
	}

	fnode_cache_stick(fnode_cache, fnode);
	putin_fnode_cache(fnode_cache);

	return NULL;
}

/**
 * link回调，硬链接时调用
 */
static int
lext3_link (struct filenode_cache *old_cache, struct file_node *dir,
	struct filenode_cache *fnode_cache)
{
	struct journal_handle *handle;
	struct file_node *fnode;
	int ret, retries = 0;

	fnode = old_cache->file_node;
	/**
	 * 链接数量太多
	 */
	if (fnode->link_count >= LEXT3_LINK_MAX)
		return -EMLINK;

retry:
	handle = lext3_start_journal(dir, LEXT3_DATA_TRANS_BLOCKS +
		LEXT3_INDEX_EXTRA_TRANS_BLOCKS);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		handle->sync = 1;

	fnode->meta_modify_time = CURRENT_TIME_SEC;
	fnode->link_count++;
	accurate_inc(&fnode->ref_count);

	ret = dir_item_stick(handle, fnode_cache, fnode);
	lext3_stop_journal(handle);
	if (ret == -ENOSPC && lext3_should_retry_alloc(dir->super, &retries))
		goto retry;

	return ret;
}

/**
 * unlink回调，删除文件
 */
static int
lext3_unlink(struct file_node *dir, struct filenode_cache *fnode_cache)
{
	struct lext3_dir_item *dir_item;
	struct journal_handle *handle;
	struct blkbuf_desc *blkbuf;
	struct file_node *fnode;
	int ret;

	handle = lext3_start_journal(dir, LEXT3_DELETE_TRANS_BLOCKS);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		handle->sync = 1;

	ret = -ENOENT;
	blkbuf = find_in_dir (fnode_cache, &dir_item);
	if (!blkbuf)
		goto fail;

	fnode = fnode_cache->file_node;
	ASSERT(fnode->link_count);

	ret = -EIO;
	if (le32_to_cpu(dir_item->file_node_num) != fnode->node_num)
		goto fail;

	ret = dir_item_delete(handle, dir, dir_item, blkbuf);
	if (ret)
		goto fail;

	dir->meta_modify_time = dir->data_modify_time = CURRENT_TIME_SEC;
	set_fnode_dir_index(dir);
	lext3_mark_fnode_dirty(handle, dir);
	fnode->link_count--;

	/**
	 * 彻底删除，为了避免异常崩溃，将其放入孤儿链表
	 */
	if (!fnode->link_count)
		lext3_add_orphan(handle, fnode);

	fnode->meta_modify_time = dir->meta_modify_time;
	lext3_mark_fnode_dirty(handle, fnode);
	ret = 0;

fail:
	lext3_stop_journal(handle);
	loosen_blkbuf (blkbuf);
	return ret;
}

/**
 * symlink回调
 */
static int
lext3_symlink (struct file_node *dir, struct filenode_cache *fnode_cache,
	const char *symname)
{
	struct journal_handle *handle;
	struct file_node *fnode;
	int len, err, retries = 0;

	len = strlen(symname) + 1;
	if (len > dir->super->block_size)
		return -ENAMETOOLONG;

retry:
	handle = lext3_start_journal(dir, LEXT3_DATA_TRANS_BLOCKS +
		LEXT3_INDEX_EXTRA_TRANS_BLOCKS + 5 + 2 * LEXT3_QUOTA_INIT_BLOCKS);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		handle->sync = 1;

	fnode = lext3_alloc_fnode (handle, dir, S_IFLNK | S_IRWXUGO);
	err = PTR_ERR(fnode);
	if (IS_ERR(fnode))
		goto out;

	/**
	 * 链接名太长，无法放到元数据中
	 */
	if (len > sizeof (fnode_to_lext3(fnode)->data_blocks)) {
		fnode->node_ops = &lext3_symlink_fnode_ops;
		lext3_init_cachespace(fnode);
		/*
		 * 向第一个块写入符号链接内容
		 */
		err = generic_symlink(fnode, symname, len);
		if (err) {
			fnode->link_count--;
			lext3_mark_fnode_dirty(handle, fnode);
			loosen_file_node (fnode);
			goto out;
		}
	} else {
		/**
		 * 可以直接写入到元数据中
		 */
		fnode->node_ops = &lext3_fast_symlink_fnode_ops;
		memcpy((char*)&fnode_to_lext3(fnode)->data_blocks, symname, len);
		fnode->file_size = len - 1;
	}

	fnode_to_lext3(fnode)->filesize_disk = fnode->file_size;
	err = dir_item_stick(handle, fnode_cache, fnode);

out:
	lext3_stop_journal(handle);
	if (err == -ENOSPC && lext3_should_retry_alloc(dir->super, &retries))
		goto retry;

	return err;
}

static int
lext3_mkdir(struct file_node *dir, struct filenode_cache *fnode_cache,
	int mode)
{
	struct lext3_file_node *lext3_fnode;
	struct lext3_dir_item *dir_item;
	struct blkbuf_desc *dir_block;
	struct journal_handle *handle;
	struct file_node *fnode;
	int err, retries = 0;

	/**
	 * 文件数量和链接数量过多
	 */
	if (dir->link_count >= LEXT3_LINK_MAX)
		return -EMLINK;

retry:
	handle = lext3_start_journal(dir, LEXT3_DATA_TRANS_BLOCKS +
		LEXT3_INDEX_EXTRA_TRANS_BLOCKS + 3 + 2 * LEXT3_QUOTA_INIT_BLOCKS);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		handle->sync = 1;

	/**
	 * 创建一个新的空白节点
	 */
	fnode = lext3_alloc_fnode (handle, dir, S_IFDIR | mode);
	err = PTR_ERR(fnode);
	if (IS_ERR(fnode))
		goto out;

	lext3_fnode = fnode_to_lext3(fnode);
	fnode->node_ops = &lext3_dir_fnode_ops;
	fnode->file_ops = &lext3_dir_fileops;
	fnode->file_size = lext3_fnode->filesize_disk = fnode->super->block_size;
	dir_block = lext3_read_metablock (handle, fnode, 0, 1, &err);
	/**
	 * 失败了，首先递减文件节点计数
	 */
	if (!dir_block) {
		fnode->link_count--;
		lext3_mark_fnode_dirty(handle, fnode);
		loosen_file_node (fnode);
		goto out;
	}

	lext3_journal_get_write_access(handle, dir_block);
	/**
	 * 首先创建"."目录项
	 */
	dir_item = first_dir_item(dir_block);
	dir_item->file_node_num = cpu_to_le32(fnode->node_num);
	dir_item->name_len = 1;
	dir_item->rec_len = cpu_to_le16(lext3_dir_item_size(dir_item->name_len));
	strcpy (dir_item->name, ".");
	diritem_set_filetype(dir->super, dir_item, S_IFDIR);

	/**
	 * 创建".."目录项
	 */
	dir_item = next_dir_item(dir_item);
	dir_item->file_node_num = cpu_to_le32(dir->node_num);
	dir_item->rec_len = cpu_to_le16(fnode->super->block_size
						- lext3_dir_item_size(1));
	dir_item->name_len = 2;
	strcpy (dir_item->name, "..");
	diritem_set_filetype(dir->super, dir_item, S_IFDIR);
	/**
	 * "." and ".."
	 */
	fnode->link_count = 2;

	lext3_journal_dirty_metadata(handle, dir_block);
	loosen_blkbuf (dir_block);
	lext3_mark_fnode_dirty(handle, fnode);
	/**
	 * 将节点添加到父目录的目录项表中
	 */
	err = add_dir_item (handle, fnode_cache, fnode);
	if (err) {
		fnode->link_count = 0;
		lext3_mark_fnode_dirty(handle, fnode);
		loosen_file_node (fnode);
		goto out;
	}

	dir->link_count++;
	set_fnode_dir_index(dir);
	lext3_mark_fnode_dirty(handle, dir);
	fnode_cache_stick(fnode_cache, fnode);
out:
	lext3_stop_journal(handle);
	if (err == -ENOSPC && lext3_should_retry_alloc(dir->super, &retries))
		goto retry;

	return err;
}

/**
 * fmdir回调
 */
static int
lext3_rmdir(struct file_node *dir, struct filenode_cache *fnode_cache)
{
	struct lext3_dir_item *dir_item;
	struct journal_handle *handle;
	struct blkbuf_desc *blkbuf;
	struct file_node *fnode;
	struct timespec now;
	int ret;

	handle = lext3_start_journal(dir, LEXT3_DELETE_TRANS_BLOCKS);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	ret = -ENOENT;
	blkbuf = find_in_dir(fnode_cache, &dir_item);
	if (!blkbuf)
		goto fail;

	if (IS_DIRSYNC(dir))
		handle->sync = 1;

	fnode = fnode_cache->file_node;

	/**
	 * 文件节点编号变化，可能是文件更名了
	 */
	ret = -EIO;
	if (le32_to_cpu(dir_item->file_node_num) != fnode->node_num)
		goto fail;

	/**
	 * 目录不为空，不能删除
	 */
	ret = -ENOTEMPTY;
	if (!lext3_dir_is_empty(fnode))
		goto fail;

	/**
	 * 从目录项中删除
	 */
	ret = dir_item_delete(handle, dir, dir_item, blkbuf);
	if (ret)
		goto fail;

	fnode->version++;
	fnode->link_count = 0;
	fnode->file_size = 0;
	/**
	 * 先进孤儿链表
	 */
	lext3_add_orphan(handle, fnode);

	now = CURRENT_TIME_SEC;
	fnode->meta_modify_time = now;
	dir->meta_modify_time = now;
	dir->data_modify_time = now;
	lext3_mark_fnode_dirty(handle, fnode);

	dir->link_count--;
	set_fnode_dir_index(dir);
	lext3_mark_fnode_dirty(handle, dir);

fail:
	lext3_stop_journal(handle);
	loosen_blkbuf (blkbuf);

	return ret;
}

/**
 * mknod回调
 */
static int
lext3_mknod (struct file_node *dir, struct filenode_cache *fnode_cache,
	int mode, devno_t dev_num)
{
	struct journal_handle *handle;
	struct file_node *fnode;
	int err, retries = 0;

retry:
	handle = lext3_start_journal(dir, LEXT3_DATA_TRANS_BLOCKS +
		LEXT3_INDEX_EXTRA_TRANS_BLOCKS + 3 + 2 * LEXT3_QUOTA_INIT_BLOCKS);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		handle->sync = 1;

	/**
	 * 创建一个空白文件节点
	 */
	fnode = lext3_alloc_fnode (handle, dir, mode);
	err = PTR_ERR(fnode);
	if (!IS_ERR(fnode)) {
		/**
		 * 初始化文件节点后，将其与父目录绑定起来
		 */
		init_special_filenode(fnode, fnode->mode, dev_num);
		err = dir_item_stick(handle, fnode_cache, fnode);
	}
	lext3_stop_journal(handle);

	if (err == -ENOSPC && lext3_should_retry_alloc(dir->super, &retries))
		goto retry;

	return err;
}

static void
dir_item_find_del(struct journal_handle *handle, struct file_node *dir,
	struct filenode_cache *cache)
{
	struct blkbuf_desc *blkbuf;
	struct lext3_dir_item *dir_item;

	blkbuf = find_in_dir(cache, &dir_item);
	if (blkbuf) {
		dir_item_delete(handle, dir, dir_item, blkbuf);
		loosen_blkbuf(blkbuf);
	}
}

/**
 * 重命名文件，但是这里不检查权限
 * 由上层调用者负责检查
 */
static int
lext3_rename (struct file_node *old_dir, struct filenode_cache *old_cache,
	struct file_node *new_dir, struct filenode_cache *new_cache)
{
	struct blkbuf_desc *old_blkbuf, *new_blkbuf, *dir_blkbuf;
	struct lext3_dir_item *old_dir_entry, *new_dir_entry;
	struct file_node *old_fnode, *new_fnode;
	struct journal_handle *handle;
	int ret, journal_blocks;

	old_blkbuf = new_blkbuf = dir_blkbuf = NULL;

	journal_blocks= 2 * LEXT3_DATA_TRANS_BLOCKS
		+ LEXT3_INDEX_EXTRA_TRANS_BLOCKS + 2;
	handle = lext3_start_journal(old_dir, journal_blocks);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(old_dir) || IS_DIRSYNC(new_dir))
		handle->sync = 1;

	/**
	 * 在旧目录中查找旧文件节点
	 */
	old_blkbuf = find_in_dir(old_cache, &old_dir_entry);
	old_fnode = old_cache->file_node;

	/**
	 * 旧文件不存在，或者文件节点编号发生了变化
	 */
	ret = -ENOENT;
	if (!old_blkbuf)
		goto out;
	if (le32_to_cpu(old_dir_entry->file_node_num) != old_fnode->node_num)
		goto out;

	/**
	 * 在新目录中查找新文件节点
	 */
	new_fnode = new_cache->file_node;
	new_blkbuf = find_in_dir(new_cache, &new_dir_entry);
	if (new_blkbuf) {
		if (!new_fnode) {
			loosen_blkbuf (new_blkbuf);
			new_blkbuf = NULL;
		}
	}

	if (S_ISDIR(old_fnode->mode)) {
		if (new_fnode) {
			ret = -ENOTEMPTY;
			/**
			 * 目标目录不为空
			 */
			if (!lext3_dir_is_empty (new_fnode))
				goto out;
		}

		/**
		 * 读入旧目录的第一个块
		 * 该块包含".."目录项
		 */
		ret = -EIO;
		dir_blkbuf = lext3_read_metablock (handle, old_fnode, 0, 0, &ret);
		if (!dir_blkbuf)
			goto out;

		/**
		 * 找到".."目录项中的文件节点编号
		 * 即父目录节点编号，如果与父目录节点编号不一致则退出
		 */
		if (le32_to_cpu(*parent_node_num(dir_blkbuf->block_data)) != old_dir->node_num)
			goto out;

		ret = -EMLINK;
		/**
		 * 新节点不存在，并且旧节点与新节点不在同一个目录
		 * 这需要新创建节点，如果新目录下的文件过多则退出
		 */
		if (!new_fnode && new_dir!=old_dir
		    && new_dir->link_count >= LEXT3_LINK_MAX)
			goto out;
	}

	/**
	 * 新目录项还不存在
	 */
	if (!new_blkbuf) {
		ret = add_dir_item (handle, new_cache, old_fnode);
		if (ret)
			goto out;
	} else {
		/**
		 * 新目录 项已经存在
		 * 获得块的日志权限后修改其文件节点编号既可
		 */
		lext3_journal_get_write_access(handle, new_blkbuf);
		new_dir_entry->file_node_num = cpu_to_le32(old_fnode->node_num);
		if (LEXT3_HAS_INCOMPAT_FEATURE(new_dir->super,
		    LEXT3_FEATURE_INCOMPAT_FILETYPE))
			new_dir_entry->file_type = old_dir_entry->file_type;
		new_dir->version++;
		lext3_journal_dirty_metadata(handle, new_blkbuf);
		loosen_blkbuf(new_blkbuf);
		new_blkbuf = NULL;
	}

	/**
	 * 修改节点的访问时间并将节点置为脏
	 */
	old_fnode->meta_modify_time = CURRENT_TIME_SEC;
	lext3_mark_fnode_dirty(handle, old_fnode);

	/**
	 * 删除旧的目录项
	 * 首先判断节点与目录项是否一致
	 * 防止目录项发生了改变
	 */
	if (dir_item_stale(old_dir_entry, old_fnode, old_cache))
		dir_item_find_del(handle, old_dir, old_cache);
	else { 
		/**
		 * 如果删除节点不成功，则重新查找并删除
		 */
		ret = dir_item_delete(handle, old_dir, old_dir_entry, old_blkbuf);
		if (ret == -ENOENT)
			dir_item_find_del(handle, old_dir, old_cache);
	}

	/**
	 * 文件节点存在
	 * 被覆盖后，其链接计数应当减少
	 */
	if (new_fnode) {
		new_fnode->link_count--;
		new_fnode->meta_modify_time = CURRENT_TIME_SEC;
	}
	old_dir->meta_modify_time = old_dir->data_modify_time = CURRENT_TIME_SEC;
	set_fnode_dir_index(old_dir);

	/**
	 * 要移动的是一个目录
	 */
	if (dir_blkbuf) {
		lext3_journal_get_write_access(handle, dir_blkbuf);
		/**
		 * 修改".."目录的父节点，使其指向新父目录
		 */
		*parent_node_num(dir_blkbuf->block_data) =
			cpu_to_le32(new_dir->node_num);
		lext3_journal_dirty_metadata(handle, dir_blkbuf);
		/**
		 * 被移除的目录节点不再引用所在目录
		 * 减少父目录的链接计数
		 */
		old_dir->link_count--;
		/**
		 * 目录被覆盖，目录中的"."引用也不存在了
		 * 因此需要减少链接计数
		 */
		if (new_fnode)
			new_fnode->link_count--;
		else {
			/**
			 * 不是覆盖，因此是新创建目录项
			 * 新的父目录被目录的".."项所引用
			 * 因此链接计数要加1
			 */
			new_dir->link_count++;
			set_fnode_dir_index(new_dir);
			lext3_mark_fnode_dirty(handle, new_dir);
		}
	}

	lext3_mark_fnode_dirty(handle, old_dir);
	if (new_fnode) {
		lext3_mark_fnode_dirty(handle, new_fnode);
		/**
		 * 没有人引用旧节点了，将它挂到孤儿链表中
		 * 后面loosen_blkbuf调用会真正删除节点
		 */
		if (!new_fnode->link_count)
			lext3_add_orphan(handle, new_fnode);
	}
	ret = 0;

out:
	loosen_blkbuf (dir_blkbuf);
	loosen_blkbuf (old_blkbuf);
	loosen_blkbuf (new_blkbuf);
	lext3_stop_journal(handle);

	return ret;
}

/**
 * setattr回调
 * 在截断文件、设置文件属性时均会调用
 */
int lext3_setattr(struct filenode_cache *fnode_cache, struct fnode_attr *attr)
{
	const unsigned int valid = attr->valid;
	struct file_node *fnode;
	int error, rc = 0;

	fnode = fnode_cache->file_node;

	/**
	 * 检查是否有权限修改属性
	 */
	error = fnode_check_attr(fnode, attr);
	if (error)
		return error;

	if ((valid & ATTR_UID && attr->uid != fnode->uid) ||
		(valid & ATTR_GID && attr->gid != fnode->gid)) {
		struct journal_handle *handle;

		handle = lext3_start_journal(fnode, 4 * LEXT3_QUOTA_INIT_BLOCKS + 3);
		if (IS_ERR(handle)) {
			error = PTR_ERR(handle);
			goto fail;
		}

		/**
		 * 修改节点用户ID后将页面置脏
		 * 并更新块缓冲区
		 */
		if (attr->valid & ATTR_UID)
			fnode->uid = attr->uid;
		if (attr->valid & ATTR_GID)
			fnode->gid = attr->gid;
		error = lext3_mark_fnode_dirty(handle, fnode);
		/**
		 * 独立的属性，作为一个事务
		 */
		lext3_stop_journal(handle);
	}

	/**
	 * 截断文件操作
	 */
	if (S_ISREG(fnode->mode) &&
	    attr->valid & ATTR_SIZE && attr->size < fnode->file_size) {
		struct journal_handle *handle;

		handle = lext3_start_journal(fnode, 3);
		if (IS_ERR(handle)) {
			error = PTR_ERR(handle);
			goto fail;
		}

		/**
		 * 截断文件时耗时的操作，这里将其放入孤儿节点链表
		 * 即使失败了，也能在重启时由恢复过程继续截断文件
		 */
		error = lext3_add_orphan(handle, fnode);
		/**
		 * 修改文件长度并同步到节点元数据缓冲区
 		 */
		fnode_to_lext3(fnode)->filesize_disk = attr->size;
		rc = lext3_mark_fnode_dirty(handle, fnode);
		if (!error)
			error = rc;

		lext3_stop_journal(handle);
	}

	/**
	 * 设置通用属性
	 */
	rc = fnode_setattr(fnode, attr);

	/**
	 * 不管是否执行了文件截断操作
	 * 都强制从孤儿链表中摘除文件节点
	 */
	if (fnode->link_count)
		lext3_orphan_del(NULL, fnode);

	/**
	 * 暂时不支持ACL访问控制
	 */
	if (!rc && (valid & ATTR_MODE))
		rc = ext3_acl_chmod(fnode);

fail:
	lext3_std_error(fnode->super, error);

	if (!error)
		error = rc;

	return error;
}

struct file_node_ops lext3_file_fnode_ops = {
	.truncate	= lext3_truncate,
	.setattr	= lext3_setattr,
	.permission	= lext3_permission,
};

struct file_node_ops lext3_dir_fnode_ops = {
	.create = lext3_create,
	.lookup = lext3_lookup,
	.link = lext3_link,
	.unlink = lext3_unlink,
	.symlink = lext3_symlink,
	.mkdir = lext3_mkdir,
	.rmdir = lext3_rmdir,
	.mknod = lext3_mknod,
	.rename = lext3_rename,
	.setattr = lext3_setattr,
	.permission = lext3_permission,
};

struct file_node_ops lext3_special_fnode_ops = {
	.setattr = lext3_setattr,
	.permission = lext3_permission,
};

/**
 * 检查目录是否为空
 */
int lext3_dir_is_empty(struct file_node *fnode)
{
	struct lext3_dir_item *self_item, *parent_item, *dir_item;
	struct blkbuf_desc *blkbuf;
	struct super_block *super;
	unsigned long offset;
	int empty_dir_len;
	int err = 0;

	super = fnode->super;
	/**
	 * "." and ".."
	 */
	empty_dir_len = lext3_dir_item_size(1) + lext3_dir_item_size(2);
	/**
	 * 读取第一个逻辑块
	 */
	blkbuf = lext3_read_metablock (NULL, fnode, 0, 0, &err);
	if (fnode->file_size < empty_dir_len || (!blkbuf)) {
		if (err)
			lext3_enconter_error(fnode->super,
				"error %d reading directory #%lu offset 0",
				err, fnode->node_num);
		return 1;
	}

	/**
	 * 判断'.'和'..'目录的有效性
	 */
	self_item = (struct lext3_dir_item *)blkbuf->block_data;
	parent_item = next_dir_item(self_item);
	if (le32_to_cpu(self_item->file_node_num) != fnode->node_num
	    || !le32_to_cpu(parent_item->file_node_num)
	    || strcmp (".", self_item->name)
	    || strcmp ("..", parent_item->name)) {
		loosen_blkbuf (blkbuf);
		return 1;
	}

	offset = le16_to_cpu(self_item->rec_len) + le16_to_cpu(parent_item->rec_len);
	dir_item = next_dir_item(parent_item);
	while (offset < fnode->file_size ) {
		int valid;

		/**
		 * 需要读入新的数据块
		 */
		if (!blkbuf || !dir_item_in_blkbuf(dir_item, blkbuf, super)) {
			if (blkbuf)
				loosen_blkbuf (blkbuf);

			err = 0;
			blkbuf = lext3_read_metablock (NULL, fnode,
				offset >> super->block_size_order, 0, &err);
			if (!blkbuf) {
				if (err)
					lext3_enconter_error(super, 
						"error %d reading directory #%lu offset %lu",
						err, fnode->node_num, offset);
				offset += super->block_size;
				continue;
			}

			dir_item = (struct lext3_dir_item *)blkbuf->block_data;
		}

		valid = lext3_verify_dir_item("lext3_dir_is_empty",
			fnode, dir_item, blkbuf, offset);
		/**
		 * 当前目录项无效，可能是磁盘有问题
		 * 读下一块
		 */
		if (!valid) {
			dir_item = (struct lext3_dir_item *)(blkbuf->block_data +
							 super->block_size);
			offset = (offset & ~(super->block_size - 1)) + super->block_size;
			continue;
		}

		/**
		 * 节点是有效节点，非空
		 */
		if (le32_to_cpu(dir_item->file_node_num)) {
			loosen_blkbuf (blkbuf);
			return 0;
		}

		offset += le16_to_cpu(dir_item->rec_len);
		dir_item = next_dir_item(dir_item);
	}

	loosen_blkbuf (blkbuf);

	return 1;
}

/**
 * delete_fnode回调
 * 当链接计数为0时，调用此函数删除磁盘上的节点
 */
void lext3_delete_fnode(struct file_node *fnode)
{
	struct journal_handle *handle;

	if (is_bad_file_node(fnode)) {
		/**
		 * 等待文件节点回写完毕
		 * 并将文件节点从内存中清除
		 */
		fnode_clean(fnode);
		return;
	}

	handle = lext3_start_trunc_journal(fnode);
	if (IS_ERR(handle)) {
		/**
		 * 从孤儿链表中摘除文件节点
		 */
		lext3_orphan_del(NULL, fnode);
		fnode_clean(fnode);
		return;
	}

	if (IS_SYNC(fnode))
		handle->sync = 1;
	/**
	 * 截断文件内容
	 */
	fnode->file_size = 0;
	if (fnode->block_count)
		lext3_truncate(fnode);

	/**
	 * 截断操作可能将节点加入到孤儿链表中
	 * 这里将其摘除
	 */
	lext3_orphan_del(handle, fnode);
	fnode_to_lext3(fnode)->del_time	= get_seconds();

	if (lext3_mark_fnode_dirty(handle, fnode))
		/**
		 * 仅仅清除内存信息
		 */
		fnode_clean(fnode);
	else
		/**
		 * 从磁盘上释放节点资源
		 */
		lext3_free_fnode(handle, fnode);

	lext3_stop_journal(handle);

	return;	
}

/**
 * dirty_fnode回调
 */
void lext3_dirty_fnode(struct file_node *fnode)
{
	struct journal_handle *handle;

	handle = lext3_start_journal(fnode, 2);
	if (IS_ERR(handle))
		goto out;

	/**
	 * 将内存中的日志信息复制到块缓冲区
	 * 并在日志中标记为脏
	 */
	lext3_mark_fnode_dirty(handle, fnode);

	lext3_stop_journal(handle);
out:
	return;
}

/**
 * 文件系统的read_fnode回调
 * 在加载根文件系统、打开文件时都会调用
 */
void lext3_read_fnode(struct file_node *fnode)
{
	struct lext3_fnode_disk *fnode_disk;
	struct lext3_file_node *lext3_fnode;
	struct lext3_fnode_loc fnode_loc;
	struct blkbuf_desc *blkbuf;
	int block;

	lext3_fnode = fnode_to_lext3(fnode);
	/**
	 * 从磁盘上加载文件节点
	 */
	if (lext3_load_fnode(fnode, &fnode_loc, 0))
		goto bad_node;

	blkbuf = fnode_loc.blkbuf;
	fnode_disk = lext3_get_fnode_disk(&fnode_loc);
	fnode->mode = le16_to_cpu(fnode_disk->mode);
	fnode->uid = (uid_t)le16_to_cpu(fnode_disk->uid_low);
	fnode->gid = (gid_t)le16_to_cpu(fnode_disk->gid_low);
	if(!(lext3_test_opt (fnode->super, LEXT3_MOUNT_NO_UID32))) {
		fnode->uid |= le16_to_cpu(fnode_disk->uid_high) << 16;
		fnode->gid |= le16_to_cpu(fnode_disk->gid_high) << 16;
	}
	fnode->link_count = le16_to_cpu(fnode_disk->links_count);
	fnode->file_size = le32_to_cpu(fnode_disk->file_size);
	fnode->access_time.tv_sec = le32_to_cpu(fnode_disk->access_time);
	fnode->meta_modify_time.tv_sec = le32_to_cpu(fnode_disk->meta_modify_time);
	fnode->data_modify_time.tv_sec = le32_to_cpu(fnode_disk->data_modify_time);
	fnode->access_time.tv_nsec = fnode->meta_modify_time.tv_nsec = fnode->data_modify_time.tv_nsec = 0;

	lext3_fnode->state = 0;
	lext3_fnode->last_logic_block = 0;
	lext3_fnode->last_phy_block = 0;
	lext3_fnode->lookup_start = 0;
	lext3_fnode->del_time = le32_to_cpu(fnode_disk->del_time);

	if (fnode->link_count == 0) {
		if (!(super_to_lext3(fnode->super)->mount_state & LEXT3_ORPHAN_FS)
		   || fnode->mode == 0) {
			/**
			 * 文件被删除了
			 */
			loosen_blkbuf (blkbuf);
			goto bad_node;
		}
	}

	/**
	 * 这个字段实际没有什么用
	 */
	fnode->block_size = PAGE_SIZE;
	fnode->block_count = le32_to_cpu(fnode_disk->block_count);
	lext3_fnode->flags = le32_to_cpu(fnode_disk->flags);
	lext3_fnode->file_acl = le32_to_cpu(fnode_disk->file_acl);
	if (!S_ISREG(fnode->mode)) {
		lext3_fnode->dir_acl = le32_to_cpu(fnode_disk->dir_acl);
	} else {
		fnode->file_size |=
			((__u64)le32_to_cpu(fnode_disk->size_high)) << 32;
	}
	lext3_fnode->filesize_disk = fnode->file_size;
	fnode->generation = le32_to_cpu(fnode_disk->generation);
	lext3_fnode->block_group_num = fnode_loc.block_group;

	for (block = 0; block < LEXT3_BLOCK_INDEX_COUNT; block++)
		lext3_fnode->data_blocks[block] = fnode_disk->blocks[block];
	list_init(&lext3_fnode->orphan);

	/**
	 * 文件节点元数据超过128字节，新版本的ext3
	 */
	if (fnode->node_num >= LEXT3_FIRST_FILENO(fnode->super) + 1 &&
	    LEXT3_FNODE_SIZE(fnode->super) > LEXT3_OLD_FNODE_SIZE) {
		lext3_fnode->extra_fsize = le16_to_cpu(fnode_disk->extra_fsize);
		if (LEXT3_OLD_FNODE_SIZE + lext3_fnode->extra_fsize >
		    LEXT3_FNODE_SIZE(fnode->super))
			goto bad_node;
		if (lext3_fnode->extra_fsize == 0) {
			lext3_fnode->extra_fsize = sizeof(struct lext3_fnode_disk) -
					    LEXT3_OLD_FNODE_SIZE;
		} else {
			__le32 *magic = (void *)fnode_disk +
					LEXT3_OLD_FNODE_SIZE +
					lext3_fnode->extra_fsize;
			if (*magic == cpu_to_le32(LEXT3_XATTR_MAGIC))
				 lext3_fnode->state |= LEXT3_STATE_XATTR;
		}
	} else
		lext3_fnode->extra_fsize = 0;

	/**
	 * 根据节点类型的差异，设置回调函数
	 */
	if (S_ISREG(fnode->mode)) {
		fnode->node_ops = &lext3_file_fnode_ops;
		fnode->file_ops = &lext3_file_ops;
		lext3_init_cachespace(fnode);
	} else if (S_ISDIR(fnode->mode)) {
		fnode->node_ops = &lext3_dir_fnode_ops;
		fnode->file_ops = &lext3_dir_fileops;
	} else if (S_ISLNK(fnode->mode)) {
		if (lext3_fnode_is_fast_symlink(fnode))
			fnode->node_ops = &lext3_fast_symlink_fnode_ops;
		else {
			fnode->node_ops = &lext3_symlink_fnode_ops;
			lext3_init_cachespace(fnode);
		}
	} else {
		fnode->node_ops = &lext3_special_fnode_ops;
		if (fnode_disk->blocks[0])
			init_special_filenode(fnode, fnode->mode,
			   uint_to_devno(le32_to_cpu(fnode_disk->blocks[0])));
		else 
			init_special_filenode(fnode, fnode->mode,
			   uint_to_devno(le32_to_cpu(fnode_disk->blocks[1])));
	}
	loosen_blkbuf (fnode_loc.blkbuf);
	lext3_set_fnode_flags(fnode);

	return;

bad_node:
	build_bad_file_node(fnode);
	return;
}

/**
 * 文件系统的write_fnode回调
 * 可能有几种情况进入:
 *	1、回收内存
 *	2、sync调用
 *	3、带O_SYNC的文件写
 */
int lext3_write_fnode(struct file_node *fnode, int wait)
{
	/**
	 * 内存回收任务，这里油水不多，回写也慢
	 * 所以直接退出为好
	 */
	if (current->flags & TASKFLAG_RECLAIM)
		return 0;

	/**
	 * 不论是同步回写，还是回收内存，都不应当获得日志句柄
	 */
	if (lext3_get_journal_handle()) {
		dump_stack();
		return -EIO;
	}

	/**
	 * 如果是sync调用，并且不等待，也退出
	 */
	if (!wait)
		return 0;

	/**
	 * 提交日志并等待日志结束
	 */
	return lext3_commit_journal(fnode->super);
}
