#include <dim-sum/aio.h>
#include <dim-sum/beehive.h>
#include <dim-sum/errno.h>
#include <dim-sum/fs_context.h>
#include <dim-sum/fs.h>
#include <dim-sum/file.h>
#include <dim-sum/mount.h>
#include <dim-sum/sched.h>
#include <dim-sum/syscall.h>

#include "internal.h"

aligned_cacheline_in_smp struct smp_lock files_lock = 
			SMP_LOCK_UNLOCKED(files_lock);
/**
 * 可分配的文件对象的最大数目。可通过/proc/sys/fs/file-max文件来修改这个值。
 */
struct files_stat_struct files_stat = {
	.max_files = NR_FILE,
};

/**
 * 判断某个文件系统内的所有文件是否均为只读
 */
int file_readonly_all(struct super_block *sb)
{
	struct double_list *p;

	smp_lock(&files_lock);
	list_for_each(p, &sb->files) {
		struct file *file = list_container(p, struct file, super);
		struct file_node *fnode = file->fnode_cache->file_node;

		/**
		 * 孤儿节点待删除
		 */
		if (fnode->link_count == 0)
			goto barrier;

		/**
		 * 可写文件
		 */
		if (S_ISREG(fnode->mode) && (file->f_mode & FMODE_WRITE))
			goto barrier;
	}
	smp_unlock(&files_lock);
	return 1;
barrier:
	smp_unlock(&files_lock);
	return 0;
}

/**
 * 分配文件描述符
 */
struct file *file_alloc(void)
{
	struct file *file;

	if (files_stat.file_refcount < files_stat.max_files) {
		/**
		 * 分配文件索引结点
		 */
		file = beehive_alloc(file_allotter, PAF_KERNEL | __PAF_ZERO);
		if (file) {
			accurate_set(&file->f_count, 1);
			file->f_uid = current->fsuid;
			file->f_gid = current->fsgid;
			list_init(&file->super);
			file->max_bytes = INT_MAX;

			return file;
		}
	}

	WARN("VFS: file allocation failed\n");

	return NULL;
}

/**
 * 将文件描述符移动到另外的链表中
 * 不再保留于文件系统链表
 */
void file_move_to_list(struct file *file, struct double_list *list)
{
	if (!list)
		return;

	smp_lock(&files_lock);
	list_move_to_front(&file->super, list);
	smp_unlock(&files_lock);
}

void file_detach_superblock(struct file *file)
{
	if (!list_is_empty(&file->super)) {
		smp_lock(&files_lock);
		list_del_init(&file->super);
		smp_unlock(&files_lock);
	}
}

/**
 * 根据进程文件描述符获得文件对象的地址。
 * 并增加其引用计数。
 */
struct file fastcall *file_find(unsigned int fd)
{
	struct task_file_handles *files = current->files;
	struct file *file = NULL;

	smp_lock(&files->file_lock);

	if (fd < files->max_handle) {
		file = files->fd_array[fd];
		if (file)
			hold_file(file);
	}

	smp_unlock(&files->file_lock);

	return file;
}

void fastcall release_file(struct file *file)
{
	struct filenode_cache *fnode_cache = file->fnode_cache;
	struct file_node *fnode = fnode_cache->file_node;
	struct mount_desc *mount = file->mount;

	might_sleep();

	if (file->file_ops && file->file_ops->release)
		file->file_ops->release(fnode, file);

	if (unlikely(fnode->chrdev))
		loosen_chrdev(fnode->chrdev);

	if (file->f_mode & FMODE_WRITE)
		put_write_access(fnode);

	file_detach_superblock(file);
	file->fnode_cache = NULL;
	file->mount = NULL;

	beehive_free(file_allotter, file);

	loosen_filenode_cache(fnode_cache);
	loosen_mount(mount);
}

/**
 * 释放对file的引用。
 */
void fastcall loosen_file(struct file *file)
{
	if (accurate_dec_and_test_zero(&file->f_count))
		release_file(file);
}

loff_t no_llseek(struct file *file, loff_t offset, int origin)
{
	return -ESPIPE;
}

loff_t generic_file_llseek(struct file *file, loff_t offset, int origin)
{
	long long ret;
	struct file_node *fnode = file_fnode(file);

	down(&fnode->sem);

	switch (origin) {
		case SEEK_END:
			offset += fnode->file_size;
			break;
		case SEEK_CUR:
			offset += file->pos;
	}

	ret = -EINVAL;
	if (offset>=0 && offset<= fnode->super->max_file_size) {
		if (offset != file->pos) {
			file->pos = offset;
			file->version = 0;
		}
		ret = offset;
	}

	up(&fnode->sem);
	return ret;
}

loff_t vfs_llseek(struct file *file, loff_t offset, int origin)
{
	loff_t ret = -ESPIPE;

	if (file->f_mode & FMODE_LSEEK) {
		if (file->file_ops && file->file_ops->llseek)
			ret = file->file_ops->llseek(file, offset, origin);
		else
			ret = generic_file_llseek(file, offset, origin);
	}

	return ret;
}

asmlinkage long sys_llseek(unsigned int fd, unsigned long offset_high,
	unsigned long offset_low, loff_t * result, unsigned int origin)
{
	struct file *file;
	loff_t offset;
	int ret;

	ret = -EINVAL;
	if (origin > SEEK_END)
		return ret;

	ret = -EBADF;
	file = file_find(fd);
	if (!file)
		return ret;

	offset = ((loff_t)offset_high << 32) | offset_low;
	offset = vfs_llseek(file, offset, origin);

	ret = (int)offset;
	if (offset >= 0) {
		ret = -EFAULT;
		if (!copy_to_user(result, &offset, sizeof(offset)))
			ret = 0;
	}

	loosen_file(file);

	return ret;
}

ssize_t file_sync_read(struct file *file, char __user *buf, size_t len, loff_t *pos)
{
	struct async_io_desc aio;
	ssize_t ret;

	init_async_io(&aio, file);
	aio.pos = *pos;
	/**
	 * 大部分指向generic_file_aio_read
	 */
	ret = file->file_ops->aio_read(&aio, buf, len, aio.pos);
	if (-EIOCBQUEUED == ret)
		ret = wait_on_async_io(&aio);
	*pos = aio.pos;

	return ret;
}

ssize_t vfs_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	ssize_t ret;

	/**
	 * 检查文件读权限
	 */
	if (!(file->f_mode & FMODE_READ))
		return -EBADF;

	if (!file->file_ops)
		return -EINVAL;

	if (!file->file_ops->read && !file->file_ops->aio_read)
		return -EINVAL;

	/**
	 * 调用文件系统的read或者aio_read
	 */
	if (file->file_ops->read)
		ret = file->file_ops->read(file, buf, count, pos);
	else
		ret = file_sync_read(file, buf, count, pos);

	return ret;
}

/**
 * 读文件
 */
asmlinkage ssize_t sys_read(unsigned int fd, char __user * buf, size_t count)
{
	ssize_t ret = -EBADF;
	struct file *file;

	file = file_find(fd);
	if (file) {
		loff_t pos = file->pos;

		ret = vfs_read(file, buf, count, &pos);
		file->pos = pos;
		loosen_file(file);
	}

	return ret;
}

ssize_t file_sync_write(struct file *file, const char __user *buf, size_t len, loff_t *pos)
{
	struct async_io_desc aio;
	ssize_t ret;

	init_async_io(&aio, file);
	aio.pos = *pos;
	ret = file->file_ops->aio_write(&aio, buf, len, aio.pos);
	if (ret == -EIOCBQUEUED)
		ret = wait_on_async_io(&aio);
	*pos = aio.pos;

	return ret;
}

ssize_t vfs_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	ssize_t ret;

	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;

	if (!file->file_ops || (!file->file_ops->write && !file->file_ops->aio_write))
		return -EINVAL;

	if (file->file_ops->write)
		ret = file->file_ops->write(file, buf, count, pos);
	else
		ret = file_sync_write(file, buf, count, pos);

	return ret;
}

asmlinkage ssize_t sys_write(unsigned int fd, const char __user * buf, size_t count)
{
	ssize_t ret = -EBADF;
	struct file *file;

	file = file_find(fd);
	if (file) {
		loff_t pos = file->pos;

		ret = vfs_write(file, buf, count, &pos);
		file->pos = pos;
		loosen_file(file);
	}

	return ret;
}

struct task_file_handles globle_files_struct;
struct task_fs_context globle_fs_struct;

/**
 * 为新进程设置文件系统属性
 * parent当前进程，对init进程来说，parent为null
 * new新创建的进程
 */
int init_task_fs(struct task_desc *parent, struct task_desc *new)
{
	if (parent == NULL)
	{
		memset(&globle_files_struct, 0, sizeof(globle_files_struct));
		memset(&globle_fs_struct, 0, sizeof(globle_fs_struct));
		globle_files_struct.max_handle = __FD_SETSIZE;
		globle_files_struct.first_free = 0;
		smp_lock_init(&globle_files_struct.file_lock);
		smp_rwlock_init(&globle_fs_struct.lock);
	}

	new->files = &globle_files_struct;
	new->fs_context = &globle_fs_struct;

	return 0;
}
