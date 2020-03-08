#include <dim-sum/err.h>
#include <dim-sum/fnode.h>
#include <dim-sum/file.h>
#include <dim-sum/fs.h>
#include <dim-sum/fs_context.h>
#include <dim-sum/mount.h>
#include <dim-sum/sched.h>
#include <dim-sum/syscall.h>

#include "internal.h"

/**
 * 为任务分配一个文件句柄
 */
int file_alloc_handle(void)
{
	struct task_file_handles * files = current->files;
	int fd, ret;

  	ret = -EMFILE;
	smp_lock(&files->file_lock);

 	fd = find_next_zero_bit(files->open_fds.fds_bits, 
				files->max_handle, 
				files->first_free);
	if (fd >= files->max_handle)
		goto out;
	
	FD_SET(fd, &files->open_fds);
	files->first_free = fd + 1;

	if (WARN_ON(files->fd_array[fd] != NULL,
	    "file_alloc_handle: slot %d not NULL!\n", fd))
		files->fd_array[fd] = NULL;

	ret = fd;

out:
	smp_unlock(&files->file_lock);
	return ret;
}

static inline void __file_free_handle(struct task_file_handles *files, unsigned int fd)
{
	FD_CLR(fd, &files->open_fds);
	if (fd < files->first_free)
		files->first_free = fd;
}

void fastcall file_free_handle(unsigned int fd)
{
	struct task_file_handles *files = current->files;

	smp_lock(&files->file_lock);
	__file_free_handle(files, fd);
	smp_unlock(&files->file_lock);
}

void fastcall file_attach_handle(unsigned int fd, struct file * file)
{
	struct task_file_handles *files = current->files;

	smp_lock(&files->file_lock);
	BUG_ON(unlikely(files->fd_array[fd] != NULL));
	files->fd_array[fd] = file;
	smp_unlock(&files->file_lock);
}

/**
 * 通用函数，在打开文件时回调
 * 各文件系统也可以自定义实现函数
 */
int generic_file_open(struct file_node * file_node, struct file * file)
{
	if (!(file->flags & O_LARGEFILE) && fnode_size(file_node) > MAX_NON_LFS)
		return -EFBIG;

	return 0;
}

/**
 * 一般用于字符设备的open回调
 */
int nonseekable_open(struct file_node *file_node, struct file *file)
{
	file->f_mode &= ~(FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE);

	return 0;
}

struct file *real_open_file(struct filenode_lookup_s *look, int flags)
{
	struct filenode_cache *fnode_cache = look->filenode_cache;
	struct mount_desc *mnt = look->mnt;
	struct file_node *fnode;
	struct file *file;
	int err;

	err = -ENFILE;
	/**
	 * 分配一个新的文件对象
	 */
	file = file_alloc();
	if (!file)
		goto loosen_look;

	file->flags = flags;
	file->f_mode = ((flags+1) & O_ACCMODE) |
		(FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE);

	fnode = fnode_cache->file_node;
	if (file->f_mode & FMODE_WRITE) {
		/**
		 * 检查写权限，避免与共享映射冲突
		 */
		err = get_write_access(fnode);
		if (err)
			goto loosen_file;
	}

	file->cache_space = fnode->cache_space;
	file->fnode_cache = fnode_cache;
	file->mount = mnt;
	file->pos = 0;
	file->file_ops = fnode->file_ops;
	/**
	 * 普通文件，加入到文件系统节点的链表中
	 */
	file_move_to_list(file, &fnode->super->files);

	/**
	 * 如果文件系统的open方法被定义，则调用它。
	 * 一般是generic_file_open，判断一下文件长度是否超长。
	 * 字符设备是chrdev_open，用来进行设备的初始化工作
	 * 块设备是blkdev_open
	 */
	if (file->file_ops && file->file_ops->open) {
		err = file->file_ops->open(fnode,file);
		if (err)
			goto cleanup;
	}
	file->flags &= ~(O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC);

	/**
	 * 初始化预读的数据结构
	 */
	file_ra_state_init(&file->readahead,
		file->cache_space->fnode->cache_space);

	/**
	 * 如果O_DIRECT被设置，就检查直接IO操作是否可以作用于文件
	 */
	if (file->flags & O_DIRECT) {
		if (!file->cache_space->ops || !file->cache_space->ops->direct_IO) {
			loosen_file(file);
			return ERR_PTR(-EINVAL);
		}
	}

	return file;

cleanup:
	if (file->f_mode & FMODE_WRITE)
		put_write_access(fnode);
	file_detach_superblock(file);
	file->fnode_cache = NULL;
	file->mount = NULL;
loosen_file:
	loosen_file(file);
loosen_look:
	loosen_fnode_look(look);
	return ERR_PTR(err);
}

/**
 * 打开文件
 */
struct file *file_open(const char * filename, int flags, int mode)
{
	struct filenode_lookup_s look = {0};
	int node_flags, err;

	/**
	 * 根据传入的标志修改namei_flags标志
	 */
	node_flags = flags;
	if ((node_flags+1) & O_ACCMODE)
		node_flags++;
	if (node_flags & O_TRUNC)
		node_flags |= O_RDWR;

	/**
	 * 查找并打开文件节点
	 */
	err = open_fnode_cache(filename, node_flags, mode, &look);
	/**
	 * 对文件节点进行初始化
	 */
	if (!err)
		return real_open_file(&look, flags);

	return ERR_PTR(err);
}

/**
 * 关闭文件，调用驱动回调进行收尾工作
 * 并释放资源
 */
int file_close(struct file *file)
{
	int ret = 0;

	if (!file_refcount(file)) {
		WARN("VFS: Close: file refcount is 0\n");
		return ret;
	}

	/**
	 * 调用文件的flush方法，只有少数驱动才会设置这个方法。
	 */
	if (file->file_ops && file->file_ops->flush)
		ret = file->file_ops->flush(file);

	loosen_file(file);

	return ret;
}

/**
 * open系统调用
 *	filename:要打开的文件名
 *	flags:访问模式
 *	mode:创建文件需要的许可权限。
 */
asmlinkage int sys_open(const char *filename,int flags,int mode)
{
	char *tmp;
	int ret;

	flags |= O_LARGEFILE;
	/**
	 * 从用户态获取文件名
	 */
	ret = clone_user_string(filename, &tmp);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	/**
	 * 分配一个未用文件句柄
	 */
	ret = file_alloc_handle();
	if (ret >= 0) {
		/**
		 * 打开文件并返回file结构。
		 */
		struct file *file = file_open(tmp, flags, mode);
		if (IS_ERR(file)) {
			/**
			 * 打开文件失败，释放文件句柄
			 */
			file_free_handle(ret);
			ret = PTR_ERR(file);
		} else
			/**
			 * 将file指针赋给current->files->fd[fd]
			 */
			file_attach_handle(ret, file);
	}

	discard_user_string(tmp);
	return ret;
}

/**
 * close系统调用
 */
asmlinkage long sys_close(unsigned int fd)
{
	struct task_file_handles *files = current->files;
	struct file *file;

	smp_lock(&files->file_lock);
	if (fd >= files->max_handle)
		goto err;

	file = files->fd_array[fd];
	if (!file)
		goto err;

	files->fd_array[fd] = NULL;
	__file_free_handle(files, fd);
	smp_unlock(&files->file_lock);
	/**
	 * 关闭文件
	 */
	return file_close(file);

err:
	smp_unlock(&files->file_lock);
	return -EBADF;
}

asmlinkage int sys_dup(unsigned int fd)
{
	struct file *file = file_find(fd);
	int ret = -EBADF;

	if (file) {
		ret = file_alloc_handle();
		if (ret >= 0)
			file_attach_handle(ret, file);
		else
			loosen_file(file);
	}

	return ret;
}

asmlinkage int sys_chdir(const char *filename)
{
	struct filenode_lookup_s look = {0};
	int ret;

	ret = load_filenode_user((char *)filename,
		FNODE_LOOKUP_READLINK | FNODE_LOOKUP_DIRECTORY, &look);
	if (ret)
		return ret;

	set_fs_pwd(current->fs_context, look.mnt, look.filenode_cache);

	loosen_mount(look.mnt);
	loosen_filenode_cache(look.filenode_cache);

	return ret;
}

asmlinkage int sys_truncate(const char *path, unsigned long length)
{
	return -ENOSYS;
}

asmlinkage int sys_ftruncate(unsigned int fd, unsigned int length)
{
	return -ENOSYS;
}
