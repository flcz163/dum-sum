#include <dim-sum/errno.h>
#include <dim-sum/uaccess.h>
#include <dim-sum/dirent.h>
#include <dim-sum/fs.h>
#include <dim-sum/syscall.h>

struct getdents_callback64 {
	struct dirent64 __user * current_dir;
	struct dirent64 __user * previous;
	int count;
	int error;
};

struct getdents_callback {
	struct dirent __user * current_dir;
	struct dirent __user * previous;
	int count;
	int error;
};

#define NAME_OFFSET(de) ((int) ((de)->d_name - (char __user *) (de)))
#define ROUND_UP(x) (((x)+sizeof(long)-1) & ~(sizeof(long)-1))
#define ROUND_UP64(x) (((x)+sizeof(u64)-1) & ~(sizeof(u64)-1))

static int filldir64(void * __buf, const char * name, int namlen, loff_t offset,
		     fnode_num_t fnode_num, unsigned int d_type)
{
	struct dirent64 *dirent;
	struct getdents_callback64 * buf = (struct getdents_callback64 *)__buf;
	int reclen = ROUND_UP64(NAME_OFFSET(dirent) + namlen + 1);

	buf->error = -EINVAL;
	if (reclen > buf->count)
		return -EINVAL;

	dirent = buf->previous;
	if (dirent) {
		if (__put_user(offset, &dirent->d_off))
			goto fault;
	}
	dirent = buf->current_dir;
	if (__put_user(fnode_num, &dirent->d_ino))
		goto fault;
	if (__put_user(0, &dirent->d_off))
		goto fault;
	if (__put_user(reclen, &dirent->d_reclen))
		goto fault;
	if (__put_user(d_type, &dirent->d_type))
		goto fault;
	if (copy_to_user(dirent->d_name, name, namlen))
		goto fault;
	if (__put_user(0, dirent->d_name + namlen))
		goto fault;

	buf->previous = dirent;
	dirent = (void __user *)dirent + reclen;
	buf->current_dir = dirent;
	buf->count -= reclen;

	return 0;

fault:
	buf->error = -EFAULT;
	return -EFAULT;
}

static int filldir(void * __buf, const char * name, int namlen, loff_t offset,
		   fnode_num_t fnode_num, unsigned int d_type)
{
	struct dirent __user * dirent;
	struct getdents_callback * buf = (struct getdents_callback *)__buf;
	int reclen = ROUND_UP(NAME_OFFSET(dirent) + namlen + 2);

	buf->error = -EINVAL;
	if (reclen > buf->count)
		return -EINVAL;

	dirent = buf->previous;
	if (dirent) {
		if (__put_user(offset, &dirent->d_off))
			goto fault;
	}
	dirent = buf->current_dir;
	if (__put_user(fnode_num, &dirent->d_ino))
		goto fault;
	if (__put_user(reclen, &dirent->d_reclen))
		goto fault;
	if (copy_to_user(dirent->d_name, name, namlen))
		goto fault;
	if (__put_user(0, dirent->d_name + namlen))
		goto fault;
	if (__put_user(d_type, (char __user *) dirent + reclen - 1))
		goto fault;
	buf->previous = dirent;
	dirent = (void __user *)dirent + reclen;
	buf->current_dir = dirent;
	buf->count -= reclen;

	return 0;

fault:
	buf->error = -EFAULT;
	return -EFAULT;
}

int vfs_readdir(struct file *file, filldir_t filler, void *buf)
{
	struct file_node *fnode = file->fnode_cache->file_node;
	int res = -ENOTDIR;

	if (!file->file_ops || !file->file_ops->readdir)
		goto out;

	down(&fnode->sem);
	res = -ENOENT;
	if (!IS_DEADDIR(fnode)) {
		res = file->file_ops->readdir(file, buf, filler);
		file_accessed(file);
	}
	up(&fnode->sem);

out:
	return res;
}

asmlinkage long sys_getdents64(unsigned int fd, struct dirent64 *dirent, unsigned int count)
{
	struct getdents_callback64 buf;
	struct dirent64 * lastdirent;
	struct file *file;
	int ret;

	ret = -EBADF;
	file = file_find(fd);
	if (!file)
		goto out;

	buf.current_dir = dirent;
	buf.previous = NULL;
	buf.count = count;
	buf.error = 0;

	ret = vfs_readdir(file, filldir64, &buf);
	if (ret < 0)
		goto loosen;

	ret = buf.error;
	lastdirent = buf.previous;
	if (lastdirent) {
		if (__put_user(file->pos, &lastdirent->d_off))
			ret = -EFAULT;
		else
			ret = count - buf.count;
	}

loosen:
	loosen_file(file);
out:
	return ret;
}

asmlinkage long sys_getdents(unsigned int fd, struct dirent *dirent, unsigned int count)
{
	struct dirent __user *lastdirent;
	struct getdents_callback buf;
	struct file *file;
	int ret;

	ret = -EBADF;
	file = file_find(fd);
	if (!file)
		goto out;

	buf.current_dir = dirent;
	buf.previous = NULL;
	buf.count = count;
	buf.error = 0;

	ret = vfs_readdir(file, filldir, &buf);
	if (ret < 0)
		goto loosen;

	ret = buf.error;
	lastdirent = buf.previous;
	if (lastdirent) {
		if (put_user(file->pos, &lastdirent->d_off))
			ret = -EFAULT;
		else
			ret = count - buf.count;
	}

loosen:
	loosen_file(file);
out:
	return ret;
}
