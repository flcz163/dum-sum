#include <dim-sum/blk_infrast.h>
#include <dim-sum/errno.h>
#include <dim-sum/fs.h>
#include <dim-sum/magic.h>
#include <dim-sum/pagemap.h>
#include <dim-sum/sched.h>
#include <dim-sum/time.h>

struct file_node *
ramfs_fnode_alloc(struct super_block *super, int mode, devno_t dev);

static int
ramfs_new_fnode(struct file_node *dir, struct filenode_cache *fnode_cache,
	int mode, devno_t dev)
{
	struct file_node *fnode;
	int error = -ENOSPC;

	fnode = ramfs_fnode_alloc(dir->super, mode, dev);
	if (fnode) {
		if (dir->mode & S_ISGID) {
			fnode->gid = dir->gid;
			if (S_ISDIR(mode))
				fnode->mode |= S_ISGID;
		}
		fnode_cache_stick(fnode_cache, fnode);
		hold_dirnode_cache(fnode_cache);
		error = 0;
	}

	return error;
}

static int ramfs_create(struct file_node *dir,
	struct filenode_cache *fnode_cache, int mode,
	struct filenode_lookup_s *look)
{
	return ramfs_new_fnode(dir, fnode_cache, mode | S_IFREG, 0);
}

static int ramfs_mkdir(struct file_node * dir,
	struct filenode_cache *fnode_cache, int mode)
{
	int retval = ramfs_new_fnode(dir, fnode_cache, mode | S_IFDIR, 0);

	/**
	 * for *..*
	 */
	if (!retval)
		dir->link_count++;

	return retval;
}

static int
ramfs_symlink(struct file_node *dir, struct filenode_cache *fnode_cache,
	const char * symname)
{
	struct file_node *fnode;
	int error = -ENOSPC;

	fnode = ramfs_fnode_alloc(dir->super, S_IFLNK | S_IRWXUGO, 0);
	if (fnode) {
		int len = strlen(symname) + 1;

		error = generic_symlink(fnode, symname, len);
		if (!error) {
			if (dir->mode & S_ISGID)
				fnode->gid = dir->gid;

			fnode_cache_stick(fnode_cache, fnode);
			hold_dirnode_cache(fnode_cache);
		} else
			loosen_file_node(fnode);
	}

	return error;
}

static struct super_block_ops ramfs_super_ops = {
	.statfs		= simple_statfs,
	.release	= generic_delete_fnode,
};

static struct cache_space_ops ramfs_cachespace_ops = {
	.readpage	= simple_readpage,
	.prepare_write	= simple_prepare_write,
	.commit_write	= simple_commit_write
};

static struct blkdev_infrast ramfs_infrast = {
	.max_ra_pages	= 0,
	.mem_device	= 1,
};

struct file_ops ramfs_file_ops = {
	.read		= generic_file_read,
	.write		= generic_file_write,
	.mmap		= generic_file_mmap,
	.fsync		= simple_sync_file,
	.sendfile	= generic_file_sendfile,
	.llseek		= generic_file_llseek,
};

static struct file_node_ops ramfs_file_fnode_ops = {
	.get_attribute	= simple_getattr,
};

static struct file_node_ops ramfs_dir_fnode_ops = {
	.create		= ramfs_create,
	.lookup		= simple_lookup,
	.link		= simple_link,
	.unlink		= simple_unlink,
	.symlink	= ramfs_symlink,
	.mkdir		= ramfs_mkdir,
	.rmdir		= simple_rmdir,
	.mknod		= ramfs_new_fnode,
	.rename		= simple_rename,
};

struct file_node *
ramfs_fnode_alloc(struct super_block *super, int mode, devno_t dev)
{
	struct file_node * fnode = fnode_alloc(super);

	if (fnode) {
		fnode->mode = mode;
		fnode->uid = current->fsuid;
		fnode->gid = current->fsgid;
		fnode->block_size = PAGE_CACHE_SIZE;
		fnode->block_count = 0;
		fnode->cache_space->ops = &ramfs_cachespace_ops;
		fnode->cache_space->blkdev_infrast = &ramfs_infrast;
		fnode->access_time = fnode->data_modify_time = fnode->meta_modify_time = CURRENT_TIME;

		switch (mode & S_IFMT) {
		default:
			init_special_filenode(fnode, mode, dev);
			break;
		case S_IFREG:
			fnode->node_ops = &ramfs_file_fnode_ops;
			fnode->file_ops = &ramfs_file_ops;
			break;
		case S_IFDIR:
			fnode->node_ops = &ramfs_dir_fnode_ops;
			fnode->file_ops = &simple_dir_ops;
			/**
			 * for *.*
			 */
			fnode->link_count++;
			break;
		case S_IFLNK:
			fnode->node_ops = &generic_symlink_fnode_ops;
			break;
		}
	}

	return fnode;
}

static int ramfs_fill_super(struct super_block *super, void *data, int silent)
{
	struct filenode_cache *root;
	struct file_node *fnode;

	super->max_file_size = MAX_LFS_FILESIZE;
	super->block_size = PAGE_CACHE_SIZE;
	super->block_size_order = PAGE_CACHE_SHIFT;
	super->magic = RAMFS_MAGIC;
	super->ops = &ramfs_super_ops;
	super->gran_ns = 1;

	fnode = ramfs_fnode_alloc(super, S_IFDIR | 0755, 0);
	if (!fnode)
		return -ENOMEM;

	root = fnode_cache_alloc_root(fnode);
	if (!root) {
		loosen_file_node(fnode);
		return -ENOMEM;
	}

	super->root_fnode_cache = root;

	return 0;
}

/**
 * 初始根文件系统的get_sb。
 */
static struct super_block *
rootfs_load_filesystem(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return load_ram_filesystem(fs_type, flags/* | MFLAG_INTERNAL*/,
		data, ramfs_fill_super);
}

static void rootfs_unload_filesystem(struct super_block *super)
{
	if (super->root_fnode_cache)
		genocide_fnode_cache(super->root_fnode_cache);
	unload_isolate_filesystem(super);
}

static struct file_system_type rootfs_fs_type = {
	.name		= "rootfs",
	.load_filesystem		= rootfs_load_filesystem,
	.unload_filesystem	= rootfs_unload_filesystem,
};

/**
 * 向内核注册特殊文件系统类型rootfs，用于后期挂载初始根目录
 */
int __init init_rootfs(void)
{
	return register_filesystem(&rootfs_fs_type);
}
