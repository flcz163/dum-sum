#include <dim-sum/fs.h>
#include <dim-sum/lext3_fs.h>
#include <dim-sum/writeback.h>

#include "internal.h"

/**
 * fsync回调
 * sys_fsync(), sys_fdatasync() and sys_msync()
 */
int lext3_sync_file(struct file *file, struct filenode_cache *fnode_cache, int datasync)
{
	struct file_node *fnode;
	int ret = 0;

	fnode = fnode_cache->file_node;
	/**
	 * 本回调只能由系统调用sync*系统调用使用
	 * 因此不可能处于事务中
	 */
	ASSERT(!lext3_get_journal_handle());

	/**
	 * 当data=journal时，sync_fnode什么也不会做
	 * 所以数据通过提交日志来实现回写
	 */
	if (lext3_get_journal_type(fnode) == JOURNAL_TYPE_FULL) {
		ret = lext3_commit_journal(fnode->super);

		return ret;
	}

	/**
	 * 对于writeback和ordered类型的日志来说
	 * 如果节点数据脏，那么通过提交并等待数据块回写
	 * 来实现sync*调用
	 */
	if (fnode->state & (FNODE_DIRTY_SYNC | FNODE_DIRTY_DATASYNC)) {
		struct writeback_control control = {
			.sync_mode = WB_SYNC_WAIT,
			.remain_page_count = 0,
		};

		ret = sync_fnode(fnode, &control);
	}

	return ret;
}
