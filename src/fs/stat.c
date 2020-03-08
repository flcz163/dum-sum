#include <dim-sum/fnode.h>
#include <dim-sum/fs.h>
#include <dim-sum/syscall.h>

#include "internal.h"

void generic_get_file_attribute(struct file_node *file_node, struct file_attribute *attr)
{
	attr->dev = file_node->super->dev_num;
	attr->ino = file_node->node_num;
	attr->mode = file_node->mode;
	attr->nlink = file_node->link_count;
	attr->uid = file_node->uid;
	attr->gid = file_node->gid;
	attr->rdev = file_node->devno;
	attr->atime = file_node->access_time;
	attr->mtime = file_node->data_modify_time;
	attr->ctime = file_node->meta_modify_time;
	attr->size = fnode_size(file_node);
	attr->blocks = file_node->block_count;
	attr->blksize = file_node->block_size;
}

static int
get_attribute(struct mount_desc *mnt, struct filenode_cache *filenode_cache,
	struct file_attribute *attr)
{
	struct file_node *file_node = filenode_cache->file_node;

	if (file_node->node_ops->get_attribute)
		return file_node->node_ops->get_attribute(mnt, filenode_cache, attr);

	generic_get_file_attribute(file_node, attr);
	if (!attr->blksize) {
		struct super_block *s = file_node->super;
		unsigned blocks;

		blocks = (attr->size + s->block_size - 1) >> s->block_size_order;
		attr->blocks = (s->block_size / 512) * blocks;
		attr->blksize = s->block_size;
	}

	return 0;
}

static int do_stat(char __user *name, struct file_attribute *attr, bool link)
{
	struct filenode_lookup_s look = {0};
	int ret;

	/**
	 * 在路径缓存中，查找文件缓存
	 * 必要时从磁盘加载文件信息
	 */
	ret = load_filenode_user(name, link ? FNODE_LOOKUP_READLINK : 0, &look);
	if (!ret) {
		ret = get_attribute(look.mnt, look.filenode_cache, attr);
		loosen_fnode_look(&look);
	}

	return ret;
}

/**
 * stat系统调用实现
 * 列出文件属性
 */
asmlinkage int sys_stat(const char * filename, struct stat *stat)
{
	struct file_attribute attr;

	int ret = do_stat((char *)filename, &attr, false);

	if (!ret)
		ret = copy_stat_to_user(&attr, stat);

	return ret;
}

/**
 * lstat系统调用实现
 * 列出文件属性及其链接对象
 */
asmlinkage int sys_lstat(const char * filename, struct stat *stat)
{
	struct file_attribute attr;
	int ret;

	ret = do_stat((char *)filename, &attr, true);
	if (!ret)
		ret = copy_stat_to_user(&attr, stat);

	return ret;
}
