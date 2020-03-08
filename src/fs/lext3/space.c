#include <dim-sum/aio.h>
#include <dim-sum/beehive.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/block_buf.h>
#include <dim-sum/err.h>
#include <dim-sum/fs.h>
#include <dim-sum/highmem.h>
#include <dim-sum/journal.h>
#include <dim-sum/lext3_fs.h>
#include <dim-sum/rbtree.h>
#include <dim-sum/uio.h>
#include <dim-sum/pagemap.h>
#include <dim-sum/timer.h>
#include <dim-sum/stacktrace.h>

#include <asm/cacheflush.h>

#include "internal.h"

int fs_overflowuid = DEFAULT_FS_OVERFLOWUID;
int fs_overflowgid = DEFAULT_FS_OVERFLOWUID;

typedef int (*handle_blkbuf_func)(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf);
						
/**
 * 遍历页面中每一个块缓冲
 */
static int
for_each_blkbuf(struct journal_handle *handle, struct blkbuf_desc *head,
	unsigned from, unsigned to, int *partial, handle_blkbuf_func func)
{
	unsigned block_start, block_end;
	unsigned blocksize = head->size;
	struct blkbuf_desc *blkbuf;
	struct blkbuf_desc *next;
	int err, ret = 0;

	blkbuf = head;
	block_start = 0;

	do {
	    	next = blkbuf->next_in_page;
		block_end = block_start + blocksize;

		if (block_end <= from || block_start >= to) {
			if (partial && !blkbuf_is_uptodate(blkbuf))
				*partial = 1;

			continue;
		}

		err = func(handle, blkbuf);
		if (!ret)
			ret = err;

		block_start = block_end;
		blkbuf = next;
	} while ((ret == 0) && (blkbuf != head));

	return ret;
}

/**
 * 在截断文件时，自末尾处将最后一页的数据清0
 * 避免磁盘中保留残余的数据
 */
static int trucate_last_block(struct journal_handle *handle, struct page_frame *page,
		struct file_cache_space *space, loff_t from)
{
	unsigned long blocksize, block_index, length, pos;
	struct blkbuf_desc *blkbuf;
	struct file_node *fnode;
	unsigned long page_index;
	unsigned long offset;
	void *kaddr;
	int err;

	fnode = space->fnode;
	page_index = from >> PAGE_CACHE_SHIFT;
	offset = from & (PAGE_CACHE_SIZE - 1);
	blocksize = fnode->super->block_size;
	length = blocksize - (offset & (blocksize - 1));
	block_index = page_index << (PAGE_CACHE_SHIFT - fnode->super->block_size_order);

	if (!page_has_blocks(page))
		blkbuf_create_desc_page(page, blocksize, 0);

	/**
	 * 查找文件末尾所在的块
	 */
	blkbuf = page_first_block(page);
	pos = blocksize;
	while (offset >= pos) {
		blkbuf = blkbuf->next_in_page;
		block_index++;
		pos += blocksize;
	}

	err = 0;
	if (blkbuf_is_freed(blkbuf)) {
		goto unlock;
	}

	/**
	 * 没有映射到磁盘，需要从磁盘中读取
	 */
	if (!blkbuf_is_mapped(blkbuf)) {
		lext3_get_datablock(fnode, block_index, blkbuf, 0);
		/**
		 * 仍然没有映射到磁盘，说明是空洞
		 * 什么都不需要做
		 */
		if (!blkbuf_is_mapped(blkbuf)) {
			goto unlock;
		}
	}

	if (pgflag_uptodate(page))
		blkbuf_set_uptodate(blkbuf);

	/**
	 * 数据块内容不是最新的
	 */
	if (!blkbuf_is_uptodate(blkbuf)) {
		err = -EIO;
		/**
		 * 读取数据块并等待
		 */
		submit_block_requests(READ, 1, &blkbuf);
		blkbuf_wait_unlock(blkbuf);
		/**
		 * 读取失败
		 */
		if (!blkbuf_is_uptodate(blkbuf))
			goto unlock;
	}

	/**
	 * 需要修改块的内容，先获取块的日志权限
	 */
	if (lext3_get_journal_type(fnode) == JOURNAL_TYPE_FULL) {
		err = lext3_journal_get_write_access(handle, blkbuf);
		if (err)
			goto unlock;
	}

	/**
	 * 将页面中超出文件大小的页面内容清0
	 */
	kaddr = kmap_atomic(page, KM_USER0);
	memset(kaddr + offset, 0, length);
	flush_dcache_page(page);
	kunmap_atomic(kaddr, KM_USER0);

	/**
	 * 根据日志类型，标记日志缓冲区
	 */
	err = 0;
	if (lext3_get_journal_type(fnode) == JOURNAL_TYPE_FULL)
		err = lext3_journal_dirty_metadata(handle, blkbuf);
	else {
		if (lext3_get_journal_type(fnode) == JOURNAL_TYPE_ORDERD)
			err = lext3_journal_dirty_data(handle, blkbuf);
		blkbuf_mark_dirty(blkbuf);
	}

unlock:
	unlock_page(page);
	loosen_page_cache(page);
	return err;
}

/**
 * 将文件逻辑块号转换为树路径
 *	fnode:	......
 *	block:	逻辑块号
 *	offset:	保存树中每一级间接块节点的偏移
 */
int lext3_block_to_path(struct file_node *fnode,
	long block, int offsets[4], int *boundary)
{
	unsigned long direct_blocks, indirect_blocks, double_blocks;
	int level_width;
	int width_order;
	int level = 0;
	int final = 0;

	/**
	 * 每一级的宽度，如果4096大小的块，可以容易1024个u32
	 * 那么宽度既为1024，相应的位数为10
	 */
	level_width = LEXT3_ADDR_PER_BLOCK(fnode->super);
	width_order = LEXT3_ADDR_PER_BLOCK_BITS(fnode->super);
	direct_blocks = LEXT3_DIRECT_BLOCKS;
	indirect_blocks = level_width;
	double_blocks = (1 << (width_order * 2));

	if (block < 0) {
		WARN("block < 0");
	} else if (block < direct_blocks) {
		offsets[level++] = block;
		final = direct_blocks;
	} else if ( (block -= direct_blocks) < indirect_blocks) {
		offsets[level++] = LEXT3_INDIRECT1_INDEX;
		offsets[level++] = block;
		final = level_width;
	} else if ((block -= indirect_blocks) < double_blocks) {
		offsets[level++] = LEXT3_INDIRECT2_INDEX;
		offsets[level++] = block >> width_order;
		offsets[level++] = block & (level_width - 1);
		final = level_width;
	} else if (((block -= double_blocks) >> (width_order * 2)) < level_width) {
		offsets[level++] = LEXT3_INDIRECT3_INDEX;
		offsets[level++] = block >> (width_order * 2);
		offsets[level++] = (block >> width_order) & (level_width - 1);
		offsets[level++] = block & (level_width - 1);
		final = level_width;
	} else {
		WARN("block is too big");
	}

	/**
	 * 正好处于边界的块，下一个块可能是间接块
	 * 提高IO性能用
	 */
	if (boundary)
		*boundary = (block & (level_width - 1)) == (final - 1);
	return level;
}

static int
try_to_extend_transaction(struct journal_handle *handle,
	struct file_node *fnode)
{
	if (handle->block_credits > EXT3_RESERVE_TRANS_BLOCKS)
		return 0;

	if (!journal_extend(handle, lext3_journal_blocks_truncate(fnode)))
		return 0;

	return 1;
}

/**
 * 释放一段连续的数据块
 */
static void
__free_data_blocks(struct journal_handle *handle, struct file_node *fnode,
	struct blkbuf_desc *blkbuf, unsigned long block_start,
	unsigned long count, __le32 *from, __le32 *to)
{
	__le32 *p;

	/**
	 * 扩展日志不成功
	 */
	if (try_to_extend_transaction(handle, fnode)) {
		/**
		 * 必须要提交日志了，将间接块和文件节点置脏
		 */
		if (blkbuf)
			lext3_journal_dirty_metadata(handle, blkbuf);
		lext3_mark_fnode_dirty(handle, fnode);
		/**
		 * 提交当前事务，并复位事务句柄
		 */
		lext3_restart_journal(handle, lext3_journal_blocks_truncate(fnode));
		/**
		 * 重新获得间接块的日志权限
		 */
		if (blkbuf)
			lext3_journal_get_write_access(handle, blkbuf);
	}

	/*
	 * 从日志和内存里面撤销相应的块
	 */
	for (p = from; p < to; p++) {
		u32 block = le32_to_cpu(*p);

		if (block) {
			struct blkbuf_desc *blkbuf;

			/**
			 * 将间接块或者文件节点相应位清0，待回写
			 */
			*p = 0;
			/**
			 * 查找块缓冲区，可能为空
			 */
			blkbuf = blkbuf_find_block(fnode->super, block);
			lext3_forget(handle, 0, fnode, blkbuf, block);
		}
	}

	/**
	 * 真正的将块释放给文件系统
	 */
	lext3_free_blocks(handle, fnode, block_start, count);
}

/**
 * 释放数据块
 *	handle:	事务句柄
 *	fnode:	要处理的文件节点
 *	blkbuf_indirect:	包含头尾数据块的间接块描述符
 *	from, to:	......
 */
static void
free_data_blocks(struct journal_handle *handle, struct file_node *fnode,
	struct blkbuf_desc *blkbuf_indirect, __le32 *from, __le32 *to)
{
	unsigned long first = 0;
	unsigned long count = 0;
	__le32 *pblock = NULL;
	unsigned long block;
	__le32 *p;
	int err;

	/**
	 * 从间接块中释放
	 */
	if (blkbuf_indirect) {
		/**
		 * 要修改间接块了，先获得其日志权限
		 */
		err = lext3_journal_get_write_access(handle, blkbuf_indirect);
		if (err)
			return;
	}

	for (p = from; p < to; p++) {
		block = le32_to_cpu(*p);
		/**
		 * 不是空洞
		 */
		if (block) {
			if (count == 0) {
				first = block;
				pblock = p;
				count = 1;
			} else if (block == first + count)
				count++;
			else {
				/**
				 * 与前一块不连续，必须提交
				 */
				__free_data_blocks(handle, fnode, blkbuf_indirect,
					first, count, pblock, p);
				first = block;
				pblock = p;
				count = 1;
			}
		}
	}

	if (count > 0)
		__free_data_blocks(handle, fnode, blkbuf_indirect,
			first, count, pblock, p);

	/**
	 * 将间接块作为元数据块，通知日志层将其置脏
	 */
	if (blkbuf_indirect)
		lext3_journal_dirty_metadata(handle, blkbuf_indirect);
}

/**
 * 找到要截断的块的起始位置
 * 如果要截断的块是空洞，必须向前找第一个非空洞的块
 */
static struct indirect_block_desc *
get_survive_pos(struct file_node *fnode, int depth, int offsets[4],
	struct indirect_block_desc chain[4], __le32 *top)
{
	struct indirect_block_desc *partial, *p;
	int i, err;

	*top = 0;
	/**
	 * i指向要保留的最深一级数据块
	 */
	i = depth;
	while (i > 1 && !offsets[i - 1])
		i--;

	/**
	 * 读取最后一个要保留的节点的间接块链
	 */
	partial = lext3_read_branch(fnode, i, offsets, chain, &err);
	/**
	 * 如果partial != NULL，表示某一级间接块还不存在
	 * 否则，末尾块的每一级间接块都存在
	 */
	if (!partial)
		partial = chain + i - 1;

	/**
	 * 这是一个空洞块，没有下一级了
	 */
	if (!partial->child_block_num && *partial->child_pos)
		goto no_top;

	/**
	 * 从当前层向上查找，直到该层左边叶子节点存在
	 * 这意味着这一层需要保留
	 */
	for (p = partial; p > chain; p--) {
		__le32 *p1, *p2;

		p1 = (__le32*)p->blkbuf->block_data;
		p2 = p->child_pos;
		while (p1 < p2)
			if (*p1++)
				break;
	}
	/**
	 * 找到最后一个需要保留的块(partial->child_block_num)
	 * 其后的块都应当从节点中删除
	 * 但是，如果文件最后一个块真的存在
	 * 那么它必然是一个叶子节点，可以简单的左移一个节点
	 * 并且返回的top为0，调用者会忽略下层节点
	 */
	if (p == chain + i - 1 && p > chain)
		p->child_pos--;
	else
		*top = *p->child_pos;

	/**
	 * 将空间接块释放掉，向上移间接块
	 */
	while(partial > p)
	{
		loosen_blkbuf(partial->blkbuf);
		partial--;
	}

no_top:

	return partial;
}

static void
free_branches(struct journal_handle *handle, struct file_node *fnode,
	struct blkbuf_desc *parent_bh, __le32 *first, __le32 *last, int depth)
{
	unsigned long block;
	__le32 *p;

	if (handle_is_aborted(handle))
		return;

	if (depth--) {
		struct blkbuf_desc *blkbuf;
		int block_count;

		block_count = LEXT3_ADDR_PER_BLOCK(fnode->super);
		p = last;

		while (--p >= first) {
			block = le32_to_cpu(*p);

			/**
			 * 空洞，直接略过
			 */
			if (!block)
				continue;

			/**
			 * 读取下一级间接块的内容
			 */
			blkbuf = blkbuf_read_block(fnode->super, block);

			if (!blkbuf) {
				lext3_enconter_error(fnode->super,
					"Read failure, fnode=%ld, block=%ld",
					fnode->node_num, block);
				continue;
			}

			/**
			 * 递归处理下一级间接块
			 */
			free_branches(handle, fnode, blkbuf, (__le32*)blkbuf->block_data,
				(__le32*)blkbuf->block_data + block_count, depth);

			/**
			 * 下级块已经释放，可以释放当前间接块
			 * 将其归还给系统，其日志处理过程非常复杂
			 */
			lext3_forget(handle, 1, fnode, blkbuf, blkbuf->block_num_dev);

			/**
			 * 处理该节点的位图，先清位图以释放块
			 * 然后在间接块中解除块的引用
			 * 这样可以在事务失败时，恢复过程只会抱怨释放空闲块而不是泄漏块。
			 */
			if (handle_is_aborted(handle))
				return;
			/**
			 * 如果扩展事务失败，那么就先提交间接块的修改
			 * 并结束当前事务
			 */
			if (try_to_extend_transaction(handle, fnode)) {
				lext3_mark_fnode_dirty(handle, fnode);
				lext3_restart_journal(handle, lext3_journal_blocks_truncate(fnode));				
			}

			/**
			 * 释放当前块
			 */
			lext3_free_blocks(handle, fnode, block, 1);

			/*
			 * 如果当前释放的块位于间接块，而不是文件节点中
			 */
			if (parent_bh) {
				/**
				 * 获得间接块的写权限并且置其脏标志
				 */
				if (!lext3_journal_get_write_access(handle, parent_bh)){
					*p = 0;
					lext3_journal_dirty_metadata(handle, parent_bh);
				}
			}
		}
	} else
		/**
		 * 只有一层，不包含间接块，直接释放数据块即可
		 */
		free_data_blocks(handle, fnode, parent_bh, first, last);
}

/**
 * 截断文件操作
 * VFS保证不会有针对同一文件的两个截断操作并发运行
 */
void lext3_truncate(struct file_node *fnode)
{
	int addr_per_block = LEXT3_ADDR_PER_BLOCK(fnode->super);
	struct lext3_file_node *lext3_fnode;
	struct file_cache_space *space;
	struct journal_handle *handle;
	struct page_frame *page;
	unsigned blocksize;
	struct indirect_block_desc chain[4];
	struct indirect_block_desc *partial;
	long last_block;
	__le32 nr = 0;
	int offsets[4];
	__le32 *data_blocks;
	int level;

	lext3_fnode = fnode_to_lext3(fnode);
	space = fnode->cache_space;
	blocksize = fnode->super->block_size;
	data_blocks = lext3_fnode->data_blocks;

	/**
	 * 只能截断常规文件
	 */
	if (!(S_ISREG(fnode->mode) || S_ISLNK(fnode->mode)))
		return;

	/**
	 * 文件只允许增加，或者文件只读
	 */
	if (IS_APPEND(fnode) || IS_IMMUTABLE(fnode))
		return;

	/**
	 * 截断文件，实际上也可能是加长
	 * 不过不管这么多，将保留窗口丢弃
	 */
	lext3_discard_reservation(fnode);

	/**
	 * 特殊处理最后一页
	 */
	if ((fnode->file_size & (blocksize - 1)) == 0) {
		page = NULL;
	} else {
		/**
		 * 将最后一页加载进内存
		 */
		page = pgcache_find_alloc_lock(space,
			fnode->file_size >> PAGE_CACHE_SHIFT,
			cache_space_get_allocflags(space));
		if (!page)
			return;
	}

	/**
	 * 由于此操作耗费资源，为了避免影响其他事务
	 * 强制将原事务提交并启动新事务
	 */
	handle = lext3_start_trunc_journal(fnode);
	if (IS_ERR(handle)) {
		if (page) {
			clear_highpage(page);
			flush_dcache_page(page);
			unlock_page(page);
			loosen_page_cache(page);
		}

		return;
	}

	last_block = (fnode->file_size + blocksize - 1)
		>> fnode->super->block_size_order;

	/**
	 * 处理最后一页
	 */
	if (page)
		trucate_last_block(handle, page, space, fnode->file_size);

	/**
	 * 找到最后一页的各个间接块逻辑地址
	 */
	level = lext3_block_to_path(fnode, last_block, offsets, NULL);
	if (level == 0)
		goto out_stop;

	/**
	 * 先将节点挂接到内存孤儿链表中
	 * 防止本过程在多个事务中途崩溃
	 * 这样在重启恢复时可以继续截断操作
	 */
	if (lext3_add_orphan(handle, fnode))
		goto out_stop;

	/**
	 * 节点已经添加进孤儿链表
	 * 现在可以放心的修改filesize_disk
	 * 此字段将被写入磁盘以反映文件的新大小
	 */
	lext3_fnode->filesize_disk = fnode->file_size;

	/**
	 * 文件节点的逻辑块布局将会变化
	 * 使用该锁防止读写过程获得物理块号
	 */
	down(&lext3_fnode->truncate_sem);

	/**
	 * 要保留的块与间接块无关，比较简单的情况
	 */
	if (level == 1) {
		/**
		 * 首先释放文件节点中，第一层的数据块
		 */
		free_data_blocks(handle, fnode, NULL, data_blocks+offsets[0],
			data_blocks + LEXT3_DIRECT_BLOCKS);
		/**
		 * 释放所有间接块
		 */
		goto do_indirects;
	}

	partial = get_survive_pos(fnode, level, offsets, chain, &nr);
	/**
	 * 删除最底层右边的节点
	 */
	if (nr) {
		/**
		 * 从顶层节点开始删除
		 */
		if (partial == chain) {
			free_branches(handle, fnode, NULL, &nr, &nr + 1,
				(chain + level - 1) - partial);
			/**
			 * 先释放下级节点，再设置脏标志
			 * 避免块泄漏
			 */
			*partial->child_pos = 0;
		} else {
			/**
			 * 从间接块处开始删除下级块
			 */
			free_branches(handle, fnode, partial->blkbuf, partial->child_pos,
				partial->child_pos + 1, (chain + level - 1) - partial);
		}
	}
	/**
	 * 删除上层中，节点右边的块
	 */
	while (partial > chain) {
		free_branches(handle, fnode, partial->blkbuf, partial->child_pos + 1,
				   (__le32*)partial->blkbuf->block_data + addr_per_block,
				   (chain + level - 1) - partial);
		loosen_blkbuf (partial->blkbuf);
		partial--;
	}
do_indirects:
	/**
	 * 视尾节点的位置，删除间接块子树
	 */
	switch (offsets[0]) {
		default:
			nr = data_blocks[LEXT3_INDIRECT1_INDEX];
			if (nr) {
				free_branches(handle, fnode, NULL,
						   &nr, &nr+1, 1);
				data_blocks[LEXT3_INDIRECT1_INDEX] = 0;
			}
		case LEXT3_INDIRECT1_INDEX:
			nr = data_blocks[LEXT3_INDIRECT2_INDEX];
			if (nr) {
				free_branches(handle, fnode, NULL,
						   &nr, &nr+1, 2);
				data_blocks[LEXT3_INDIRECT2_INDEX] = 0;
			}
		case LEXT3_INDIRECT2_INDEX:
			nr = data_blocks[LEXT3_INDIRECT3_INDEX];
			if (nr) {
				free_branches(handle, fnode, NULL,
						   &nr, &nr+1, 3);
				data_blocks[LEXT3_INDIRECT3_INDEX] = 0;
			}
		case LEXT3_INDIRECT3_INDEX:
			;
	}

	up(&lext3_fnode->truncate_sem);
	/**
	 * 记录文件访问时间
	 */
	fnode->data_modify_time = fnode->meta_modify_time = CURRENT_TIME_SEC;
	/**
	 * 标记文件元数据为脏
	 */
	lext3_mark_fnode_dirty(handle, fnode);

	/**
	 * 这导致随后的lext3_stop_journal立即提交事务
	 */
	if (IS_SYNC(fnode))
		handle->sync = 1;
out_stop:
	/**
	 * 如果是由于删除节点而执行的截断操作
	 * 则将其从孤儿节点中移除
	 */
	if (fnode->link_count)
		lext3_orphan_del(handle, fnode);

	lext3_stop_journal(handle);
}

/**
 * 计算写页面所需要保留的日志块数量
 *	1、N个数据块
 *	2、2个间接块
 *	3、2个二级间接块
 *	4、1个三级间接块
 *	5、N+5个位图块
 *	6、N+5个组描述符块
 *	7、1个文件节点 块
 *	8、1个超级块
 *	共计3 * (N + 5) + 2
 */
static int get_trans_block_writepage(struct file_node *fnode)
{
	int data_blocks;
	int indirects;
	int ret;

	data_blocks = journal_blocks_per_page(fnode);
	/**
	 * 页面的块不会跨不能层次的间接块
	 * 这样间接块最多3个而不是5个
	 */
	indirects = (LEXT3_DIRECT_BLOCKS % data_blocks) ? 5 : 3;
	/**
	 * 数据块需要进入日志
	 */
	if (lext3_get_journal_type(fnode) == JOURNAL_TYPE_FULL)
		ret = 3 * (data_blocks + indirects) + 2;
	else
		ret = 2 * (data_blocks + indirects) + 2;

	return ret;
}

static int lext3_read_page(struct file *file, struct page_frame *page)
{
	return generic_readpage(page, lext3_get_datablock);
}

static int lext3_read_pages(struct file *file, struct file_cache_space *space,
		struct double_list *pages, unsigned nr_pages)
{
	return generic_readpages(space, pages, nr_pages, lext3_get_datablock);
}

/**
 * 查找文件逻辑块的物理块号
 * 调用者可能对这些块进行写操作
 *
 * 如果受影响的块仍然在日志中，这将是危险的操作
 */
static sector_t lext3_map_block(struct file_cache_space *space, sector_t block)
{
	struct file_node *fnode = space->fnode;
	struct journal *journal;
	int err;

	/**
	 * 页面数据在日志中，摊上大事了
	 */
	if (fnode_to_lext3(fnode)->state & LEXT3_FNODE_JDATA) {
		journal = lext3_get_fnode_journal(fnode);

		fnode_to_lext3(fnode)->state &= ~LEXT3_FNODE_JDATA;
		/**
		 * 刷新整个日志，确实是重量级的方法
		 */
		journal_lock_updates(journal);
		err = journal_flush(journal);
		journal_unlock_updates(journal);

		if (err)
			return 0;
	}

	/**
	 * 获得而不是*创建*逻辑块的物理块号
	 */
	return get_mapped_block(space, block, lext3_get_datablock);
}

static int lext3_invalidate_page(struct page_frame *page, unsigned long offset)
{
	struct journal *journal;

	journal = lext3_get_fnode_journal(page->cache_space->fnode);
	if (offset == 0)
		clear_page_pending_dirty(page);

	return journal_invalidatepage(journal, page, offset);
}

static int lext3_release_page(struct page_frame *page, int wait)
{
	struct journal *journal;

	journal = lext3_get_fnode_journal(page->cache_space->fnode);
	WARN_ON(pgflag_pending_dirty(page));
	
	return journal_release_page(journal, page, wait);
}

/**
 * 暂不支持direct-IO
 */
static ssize_t
lext3_direct_IO(int rw, struct async_io_desc *aio, const struct io_segment *iov,
	loff_t offset, unsigned long nr_segs)
{
	return -ENOSYS;
}

static int lext3_get_write_access(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf)
{
	/**
	 * 没有与磁盘映射，或者块已经无用了
	 */
	if (!blkbuf_is_mapped(blkbuf) || blkbuf_is_freed(blkbuf))
		return 0;

	return lext3_journal_get_write_access(handle, blkbuf);
}

static int lext3_prepare_write(struct file *file, struct page_frame *page,
	unsigned from, unsigned to)
{
	struct journal_handle *handle;
	struct file_node *fnode;
	int retries = 0;
	int ret;

	fnode = page->cache_space->fnode;

try_again:
	/**
	 * 新块可能需要分配磁盘空间
	 * 这里保留日志块
	 */
	handle = lext3_start_journal(fnode, get_trans_block_writepage(fnode));
	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		goto out;
	}

	ret = blkbuf_prepare_write(page, from, to, lext3_get_datablock);
	if (!ret && (lext3_get_journal_type(fnode) == JOURNAL_TYPE_FULL))
		ret = for_each_blkbuf(handle, page_first_block(page),
				from, to, NULL, lext3_get_write_access);

	if (ret)
		lext3_stop_journal(handle);

	if (ret == -ENOSPC && lext3_should_retry_alloc(fnode->super, &retries))
		goto try_again;

out:
	return ret;
}

static int hold_func(struct journal_handle *handle, struct blkbuf_desc *blkbuf)
{
	hold_blkbuf(blkbuf);

	return 0;
}

static int loosen_func(struct journal_handle *handle, struct blkbuf_desc *blkbuf)
{
	loosen_blkbuf(blkbuf);

	return 0;
}

static int
dirty_data_func(struct journal_handle *handle, struct blkbuf_desc *blkbuf)
{
	if (blkbuf_is_mapped(blkbuf))
		return lext3_journal_dirty_data(handle, blkbuf);

	return 0;
}

static int lext3_writepage_ordered(struct page_frame *page,
			struct writeback_control *control)
{
	struct journal_handle *handle;
	struct file_node *fnode;
	int ret = 0;
	int err;

	ASSERT(pgflag_locked(page));
	fnode = page->cache_space->fnode;

	/**
	 * 当前进程正在日志过程中
	 * 可能是内存紧张进入了回收流程
	 * 不能继续，否则可能死锁
	 */
	if (lext3_get_journal_handle())
		goto fail;

	/**
	 * 获得日志块配额
	 */
	handle = lext3_start_journal(fnode, get_trans_block_writepage(fnode));

	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		goto fail;
	}

	if (!page_has_blocks(page))
		blkbuf_create_desc_page(page, fnode->super->block_size,
				(1 << BS_DIRTY) | (1 << BS_UPTODATE));

	/**
	 * 获得所有块的引用
	 */
	for_each_blkbuf(handle, page_first_block(page), 0,
			PAGE_CACHE_SIZE, NULL, hold_func);

	/**
	 * 提交页面块，进入block层
	 */
	ret = blkbuf_write_page(page, lext3_get_datablock, control);

	if (ret == 0) {
		/**
		 * 对页面中的每一个缓冲区，调用dirty_data_func函数。
		 * 将相应的块放入日志的数据缓冲区链表
		 */
		err = for_each_blkbuf(handle, page_first_block(page),
			0, PAGE_CACHE_SIZE, NULL, dirty_data_func);
		if (!ret)
			ret = err;
	}

	/**
	 * 释放对缓冲区块的引用
	 */
	for_each_blkbuf(handle, page_first_block(page), 0,
			PAGE_CACHE_SIZE, NULL, loosen_func);

	/**
	 * 通知日志模块，当前日志操作完成
	 */
	err = lext3_stop_journal(handle);

	if (!ret)
		ret = err;

	return ret;

fail:
	redirty_page_for_writepage(control, page);
	unlock_page(page);
	return ret;
}

static int commit_write_ordered(struct file *file, struct page_frame *page,
			     unsigned from, unsigned to)
{
	struct journal_handle *handle = lext3_get_journal_handle();
	struct file_node *fnode = page->cache_space->fnode;
	int ret = 0, err;

	handle = lext3_get_journal_handle();
	/**
	 * file可能为NULL，从page取文件节点
	 */
	fnode = page->cache_space->fnode;

	/**
	 * 设置页面块的脏标记，注意此标记由日志模块管理
	 * 不能直接提交给block层
	 */
	ret = for_each_blkbuf(handle, page_first_block(page),
		from, to, NULL, lext3_journal_dirty_data);

	if (ret == 0) {
		loff_t end;

		end = ((loff_t)page->index << PAGE_CACHE_SHIFT) + to;
		if (end > fnode_to_lext3(fnode)->filesize_disk)
			fnode_to_lext3(fnode)->filesize_disk = end;

		ret = generic_commit_write(file, page, from, to);
	}

	err = lext3_stop_journal(handle);
	if (!ret)
		ret = err;

	return ret;
}

static int writepage_writeback(struct page_frame *page,
				struct writeback_control *control)
{
	struct journal_handle *handle;
	struct file_node *fnode;
	int ret = 0;
	int err;

	fnode = page->cache_space->fnode;

	if (lext3_get_journal_handle())
		goto fail;

	/**
	 * 扩充日志保留块，在lext3_get_block可能需要分配元数据
	 */
	handle = lext3_start_journal(fnode, get_trans_block_writepage(fnode));
	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		goto fail;
	}

	ret = blkbuf_write_page(page, lext3_get_datablock, control);
	err = lext3_stop_journal(handle);
	if (!ret)
		ret = err;

	return ret;

fail:
	redirty_page_for_writepage(control, page);
	unlock_page(page);
	return ret;
}

static int
commit_write_writeback(struct file *file, struct page_frame *page,
	unsigned from, unsigned to)
{
	struct journal_handle *handle;
	struct file_node *fnode;
	int ret = 0, err;
	loff_t end;

	handle = lext3_get_journal_handle();
	fnode = page->cache_space->fnode;

	end = ((loff_t)page->index << PAGE_CACHE_SHIFT) + to;
	/**
	 * 由于本次写的原因，将文件变大了
	 * 应当修改元数据以反映这一事实
	 */
	if (end > fnode_to_lext3(fnode)->filesize_disk)
		fnode_to_lext3(fnode)->filesize_disk = end;

	ret = generic_commit_write(file, page, from, to);

	err = lext3_stop_journal(handle);
	if (!ret)
		ret = err;

	return ret;
}

/**
 * 向日志模块提交页面数据块日志
 */
static int
commit_blkbuf_journal(struct journal_handle *handle, struct blkbuf_desc *blkbuf)
{
	if (!blkbuf_is_mapped(blkbuf) || blkbuf_is_freed(blkbuf))
		return 0;

	blkbuf_set_uptodate(blkbuf);

	/**
	 * 将数据块缓冲区直接作为元数据日志块提交既可
	 */
	return lext3_journal_dirty_metadata(handle, blkbuf);
}

static int writepage_journalled(struct page_frame *page,
	struct writeback_control *control)
{
	struct journal_handle *handle = NULL;
	struct file_node *fnode;
	int ret = 0;
	int err;

	fnode = page->cache_space->fnode;
	if (lext3_get_journal_handle())
		goto fail;

	/**
	 * 扩充日志保留块
	 */
	handle = lext3_start_journal(fnode, get_trans_block_writepage(fnode));
	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		goto fail;
	}

	/**
	 * 这里需要补上一些prepare_write的操作
	 */
	if (!page_has_blocks(page) || pgflag_pending_dirty(page)) {
		clear_page_pending_dirty(page);
		/**
		 * 为页面准备块缓冲区
		 */
		ret = blkbuf_prepare_write(page, 0, PAGE_CACHE_SIZE,
					lext3_get_datablock);
		if (ret != 0)
			goto unlock;

		/**
		 * 首先获得页面块的日志写权限，然后提交块
		 */
		ret = for_each_blkbuf(handle, page_first_block(page), 0,
			PAGE_CACHE_SIZE, NULL, lext3_get_write_access);

		err = for_each_blkbuf(handle, page_first_block(page), 0,
			PAGE_CACHE_SIZE, NULL, commit_blkbuf_journal);

		if (ret == 0)
			ret = err;

		fnode_to_lext3(fnode)->state |= LEXT3_FNODE_JDATA;
		unlock_page(page);
	} else
		ret = blkbuf_write_page(page, lext3_get_datablock, control);

	err = lext3_stop_journal(handle);
	if (!ret)
		ret = err;
out:
	return ret;

fail:
	redirty_page_for_writepage(control, page);
unlock:
	unlock_page(page);
	goto out;
}

static int
commit_write_journalled(struct file *file, struct page_frame *page,
	unsigned from, unsigned to)
{
	struct journal_handle *handle;
	struct file_node *fnode;
	int ret = 0, err;
	int partial = 0;
	loff_t end;

	handle = lext3_get_journal_handle();
	fnode = page->cache_space->fnode;

	end = ((loff_t)page->index << PAGE_CACHE_SHIFT) + to;

	/**
	 * 遍历提交每一个脏数据块
	 */
	ret = for_each_blkbuf(handle, page_first_block(page), from,
				to, &partial, commit_blkbuf_journal);
	/**
	 * 没有出错，并且所有块都是最新的
	 * 则更新整个页面的标志
	 */
	if (!ret && !partial)
		set_page_uptodate(page);

	/**
	 * 文件长度变大，更新文件大小
	 */
	if (end > fnode->file_size)
		fnode_update_file_size(fnode, end);
	/**
	 * 做标记，表示文件的数据块正在日志中
	 */
	fnode_to_lext3(fnode)->state |= LEXT3_FNODE_JDATA;

	/**
	 * 由于本次写的原因，将文件变大了
	 * 应当修改元数据以反映这一事实
	 */
	if (fnode->file_size > fnode_to_lext3(fnode)->filesize_disk) {
		fnode_to_lext3(fnode)->filesize_disk = fnode->file_size;
		err = lext3_mark_fnode_dirty(handle, fnode);
		if (!ret) 
			ret = err;
	}

	/**
	 * 结束本次日志操作
	 */
	err = lext3_stop_journal(handle);
	if (!ret)
		ret = err;
	return ret;
}

/**
 * 设置页面为脏，并且页面需要被记录到日志
 * 这些页面可能并没有经过VFS层，日志系统无法感知脏页变化
 *
 * 这里不能弃块缓冲区于不顾，也不能直接设置块缓冲区为脏
 * 因为这绕开了日志系统
 *
 * 因此这里设置*挂起*标志，待后期处理
 */
static int set_page_dirty_journalled(struct page_frame *page)
{
	/**
	 * 设置*挂起*标志，在write_page回调中再处理日志相关事情
	 */
	set_page_pending_dirty(page);
	return __set_page_dirty_nobuffers(page);
}

static struct cache_space_ops ordered_cache_ops = {
	.readpage	= lext3_read_page,
	.readpages	= lext3_read_pages,
	.writepage	= lext3_writepage_ordered,
	.sync_page	= blkbuf_sync_page,
	.prepare_write	= lext3_prepare_write,
	.commit_write	= commit_write_ordered,
	.map_block		= lext3_map_block,
	.invalidatepage	= lext3_invalidate_page,
	.releasepage	= lext3_release_page,
	.direct_IO	= lext3_direct_IO,
};

static struct cache_space_ops writeback_cache_ops = {
	.readpage	= lext3_read_page,
	.readpages	= lext3_read_pages,
	.writepage	= writepage_writeback,
	.sync_page	= blkbuf_sync_page,
	.prepare_write	= lext3_prepare_write,
	.commit_write	= commit_write_writeback,
	.map_block		= lext3_map_block,
	.invalidatepage	= lext3_invalidate_page,
	.releasepage	= lext3_release_page,
	.direct_IO	= lext3_direct_IO,
};

static struct cache_space_ops journalled_cache_ops = {
	.readpage	= lext3_read_page,
	.readpages	= lext3_read_pages,
	.writepage	= writepage_journalled,
	.sync_page	= blkbuf_sync_page,
	.prepare_write	= lext3_prepare_write,
	.commit_write	= commit_write_journalled,
	.set_page_dirty	= set_page_dirty_journalled,
	.map_block		= lext3_map_block,
	.invalidatepage	= lext3_invalidate_page,
	.releasepage	= lext3_release_page,
};

enum lext3_journal_type lext3_get_journal_type(struct file_node *fnode)
{
	if (fnode->cache_space->ops == &ordered_cache_ops)
		return JOURNAL_TYPE_ORDERD;

	if (fnode->cache_space->ops == &writeback_cache_ops)
		return JOURNAL_TYPE_WRITEBACK;

	if (fnode->cache_space->ops == &journalled_cache_ops)
		return JOURNAL_TYPE_FULL;

	return JOURNAL_TYPE_NOJOURNAL;
}

/**
 * 初始化普通文件的缓存空间
 * 仅仅针对普通文件和链接文件
 */
void lext3_init_cachespace(struct file_node *fnode)
{
	ASSERT(S_ISREG(fnode->mode) || S_ISLNK(fnode->mode));

	if ((fnode_to_lext3(fnode)->flags & LEXT3_JOURNAL_DATA_FL)
		|| !S_ISREG(fnode->mode)) {
		fnode->cache_space->ops = &journalled_cache_ops;
		return;
	}

	switch (lext3_test_opt(fnode->super, LEXT3_MOUNT_DATA_FLAGS)) {
	case LEXT3_MOUNT_ORDERED_DATA:
		fnode->cache_space->ops = &ordered_cache_ops;
		break;
	case LEXT3_MOUNT_WRITEBACK_DATA:
		fnode->cache_space->ops = &writeback_cache_ops;
		break;
	case LEXT3_MOUNT_JOURNAL_DATA:
		fnode->cache_space->ops = &journalled_cache_ops;
		break;
	default:
		BUG();
	}
}
