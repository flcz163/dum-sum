#include <dim-sum/aio.h>
#include <dim-sum/err.h>
#include <dim-sum/fs.h>
#include <dim-sum/lext3_fs.h>

#include "internal.h"

/**
 * release回调
 * 当全部关闭的时候调用
 */
static int lext3_release_file (struct file_node *fnode, struct file *filp)
{
	/**
	 * 最后一个写者
	 */
	if ((filp->f_mode & FMODE_WRITE) &&
	    (accurate_read(&fnode->write_users) == 1))
		lext3_discard_reservation(fnode);

	return 0;
}

static ssize_t
lext3_file_write(struct async_io_desc *aio, const char __user *buf,
	size_t count, loff_t pos)
{
	struct file_node *fnode;
	struct file *file;
	ssize_t ret;
	int err;

	file = aio->file;
	fnode = file->fnode_cache->file_node;
	ret = generic_file_aio_write(aio, buf, count, pos);

	/**
	 * 遇到错误，直接返回
	 */
	if (ret <= 0)
		return ret;

	/*
	 * 文件打开标志表明需要同步写数据
	 */
	if (file->flags & O_SYNC) {
		/*
		 * 如果文件的数据块不受日志管理
		 * 那么通用流程会回写数据，这里直接退出
		 */
		if (lext3_get_journal_type(fnode) != JOURNAL_TYPE_FULL)
			return ret;

		/**
		 * 如果数据块由日志管理，那么为了确保数据被写入
		 * 需要强制提交日志
		 */
		goto force_commit;
	}

	/*
	 * 如果文件节点不需要强制回写数据也退出
	 */
	if (!IS_SYNC(fnode))
		return ret;

force_commit:
	err = lext3_commit_journal(fnode->super);
	if (err) 
		return err;

	return ret;
}

struct file_ops lext3_file_ops = {
	.llseek = generic_file_llseek,
	.read = file_sync_read,
	.write = file_sync_write,
	.aio_read = generic_file_aio_read,
	.aio_write = lext3_file_write,
	.readv = generic_file_readv,
	.writev = generic_file_writev,
	.ioctl	 = lext3_ioctl,
	.mmap = generic_file_mmap,
	.open = generic_file_open,
	.release = lext3_release_file,
	.fsync = lext3_sync_file,
	.sendfile = generic_file_sendfile,
};
