#include <dim-sum/err.h>
#include <dim-sum/fs.h>
#include <dim-sum/fnode.h>
#include <dim-sum/timer.h>

static int bad_follow_link(struct filenode_cache *dent, struct filenode_lookup_s *look)
{
	look_save_symlink_name(look, ERR_PTR(-EIO));
	return 0;
}

static int return_EIO(void)
{
	return -EIO;
}

#define EIO_ERROR ((void *) (return_EIO))

static struct file_ops bad_file_ops =
{
	.llseek		= EIO_ERROR,
	.aio_read	= EIO_ERROR,
	.read		= EIO_ERROR,
	.write		= EIO_ERROR,
	.aio_write	= EIO_ERROR,
	.readdir	= EIO_ERROR,
	.poll		= EIO_ERROR,
	.ioctl		= EIO_ERROR,
	.mmap		= EIO_ERROR,
	.open		= EIO_ERROR,
	.flush		= EIO_ERROR,
	.release	= EIO_ERROR,
	.fsync		= EIO_ERROR,
	.aio_fsync	= EIO_ERROR,
	.fasync		= EIO_ERROR,
	.lock		= EIO_ERROR,
	.readv		= EIO_ERROR,
	.writev		= EIO_ERROR,
	.sendfile	= EIO_ERROR,
	.sendpage	= EIO_ERROR,
	.get_unmapped_area = EIO_ERROR,
};

struct file_node_ops bad_inode_ops =
{
	.create		= EIO_ERROR,
	.lookup		= EIO_ERROR,
	.link		= EIO_ERROR,
	.unlink		= EIO_ERROR,
	.symlink	= EIO_ERROR,
	.mkdir		= EIO_ERROR,
	.rmdir		= EIO_ERROR,
	.mknod		= EIO_ERROR,
	.rename		= EIO_ERROR,
	.read_link	= EIO_ERROR,
	.follow_link	= bad_follow_link,
	.truncate	= EIO_ERROR,
	.permission	= EIO_ERROR,
	.get_attribute	= EIO_ERROR,
	.setattr	= EIO_ERROR,
	.setxattr	= EIO_ERROR,
	.getxattr	= EIO_ERROR,
	.listxattr	= EIO_ERROR,
	.removexattr	= EIO_ERROR,
};


/**
 * 当驱动读取错误时，无法为文件节点构建正确的描述符
 * 使用此函数构建一个伪造的描述符
 */
void build_bad_file_node(struct file_node *fnode) 
{
	takeout_file_node(fnode);

	fnode->mode = S_IFREG;
	fnode->access_time = fnode->data_modify_time = fnode->meta_modify_time = CURRENT_TIME;
	fnode->node_ops = &bad_inode_ops;	
	fnode->file_ops = &bad_file_ops;	
}

int is_bad_file_node(struct file_node *fnode) 
{
	return (fnode->node_ops == &bad_inode_ops);	
}
