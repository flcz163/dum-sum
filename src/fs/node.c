#include <dim-sum/beehive.h>
#include <dim-sum/err.h>
#include <dim-sum/errno.h>
#include <dim-sum/fnode.h>
#include <dim-sum/fs.h>
#include <dim-sum/fs_context.h>
#include <dim-sum/highmem.h>
#include <dim-sum/mount.h>
#include <dim-sum/pagemap.h>
#include <dim-sum/sched.h>
#include <dim-sum/string.h>
#include <dim-sum/syscall.h>

#include "internal.h"

int get_write_access(struct file_node *fnode)
{
	smp_lock(&fnode->i_lock);
	/**
	 * 共享映射，不能再通过open打开
	 */
	if (accurate_read(&fnode->write_users) < 0) {
		smp_unlock(&fnode->i_lock);
		return -ETXTBSY;
	}
	accurate_inc(&fnode->write_users);
	smp_unlock(&fnode->i_lock);

	return 0;
}

int do_truncate(struct filenode_cache *fnode_cache, loff_t length)
{
	/* TO-DO */
	return 0;
}

static int may_open(struct mount_desc *mnt,
	struct filenode_cache *fnode_cache, int flag)
{
	struct file_node *file_node = fnode_cache->file_node;
	int ret;

	if (!file_node)
		return -ENOENT;

	if (S_ISLNK(file_node->mode))
		return -ELOOP;
	
	if (S_ISDIR(file_node->mode) && (flag & FMODE_WRITE))
		return -EISDIR;

	if (S_ISFIFO(file_node->mode) || S_ISSOCK(file_node->mode)) {
		flag &= ~O_TRUNC;
	} else if (S_ISBLK(file_node->mode) || S_ISCHR(file_node->mode)) {
		if (mnt->flags & MNT_NODEV)
			return -EACCES;
		flag &= ~O_TRUNC;
	} else if (IS_RDONLY(file_node) && (flag & FMODE_WRITE))
		return -EROFS;

	if (IS_APPEND(file_node)) {
		if  ((flag & FMODE_WRITE) && !(flag & O_APPEND))
			return -EPERM;
		if (flag & O_TRUNC)
			return -EPERM;
	}

	if (flag & O_NOATIME)
		if (current->fsuid != file_node->uid)
			return -EPERM;

	if (flag & O_TRUNC) {
		ret = get_write_access(file_node);
		if (ret)
			return ret;

		ret = do_truncate(fnode_cache, 0);
		put_write_access(file_node);
		if (ret)
			return ret;
	}

	return 0;
}

static int get_look_flags(unsigned int flags)
{
	unsigned long ret = FNODE_LOOKUP_READLINK;

	if (flags & O_NOFOLLOW)
		ret &= ~FNODE_LOOKUP_READLINK;

	/**
	 * 用于进程互斥，不能考虑符号链接
	 */
	if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT|O_EXCL))
		ret &= ~FNODE_LOOKUP_READLINK;
	
	if (flags & O_DIRECTORY)
		ret |= FNODE_LOOKUP_DIRECTORY;

	return ret;
}

/**
 * 为打开文件操作而打开文件节点缓存
 */
int open_fnode_cache(const char *pathname, int flag, int mode,
	struct filenode_lookup_s *look)
{
	struct filenode_cache *fnode_cache;
	struct filenode_cache *dir_cache;
	struct file_node *dir_node;
	int count = 0;
	int ret = 0;

	/**
	 * 通常的情况，文件不存在时不用创建
	 */
	if (!(flag & O_CREAT)) {
		ret = load_filenode_cache((char *)pathname,
			get_look_flags(flag), look);
		if (ret)
			return ret;

		ret = may_open(look->mnt, look->filenode_cache, flag);
		if (ret) {
			loosen_fnode_look(look);
			return ret;
		}
	
		return 0;
	}

	/**
	 * 先找到父目录的位置
	 */
	ret = load_filenode_cache((char *)pathname, FNODE_LOOKUP_NOLAST, look);
	/**
	 * 父目录不存在，不玩了
	 */
	if (ret)
		return ret;

	ret = -EISDIR;
	/**
	 * 最后一个分量是目录。失败
	 */
	if (look->path_type != PATHTYPE_NORMAL || look->last.name[look->last.len])
		goto loosen_look;

	dir_cache = look->filenode_cache;
	dir_node = dir_cache->file_node;
	look->flags &= ~FNODE_LOOKUP_NOLAST;
	/**
	 * 获得目录节点的锁
	 */
	down(&dir_node->sem);
	fnode_cache = __find_file_indir_alloc(&look->last, look->filenode_cache, look);

recognize_last:
	/**
	 * 由于内存或者其他原因，无法查找文件
	 */
	ret = PTR_ERR(fnode_cache);
	if (IS_ERR(fnode_cache)) {
		up(&dir_cache->file_node->sem);
		goto loosen_look;
	}

	/**
	 * 在物理块设备上，文件还不存在
	 */
	if (!fnode_cache->file_node) {
		if (!IS_POSIXACL(dir_cache->file_node))
			mode &= ~current->fs_context->umask;

		if (!dir_node->node_ops || !dir_node->node_ops->create)
			ret = -EACCES;
		else {
			mode &= S_IALLUGO;
			mode |= S_IFREG;
			/**
			 * 调用文件系统的回调来创建真实的文件
			 */
			ret = dir_node->node_ops->create(dir_node, fnode_cache, mode, look);
		}

		up(&dir_node->sem);

		loosen_filenode_cache(look->filenode_cache);
		look->filenode_cache = fnode_cache;
		if (ret)
			goto loosen_look;

		flag &= ~O_TRUNC;
		ret = may_open(look->mnt, look->filenode_cache, flag);
		if (ret)
			goto loosen_look;

		return 0;
	}

	/**
	 * 运行到这里，说明文件已经存在
	 */
	up(&dir_cache->file_node->sem);

	ret = -EEXIST;
	/**
	 * 用户同时指定了O_CREAT和O_EXCL
	 * 表示在使用文件进行进程同步
	 * 按照协议规范，这里应当返回错误
	 */
	if (flag & O_EXCL)
		goto loosen_child;

	if (fnode_cache->mount_count) {
		ret = -ELOOP;
		if (flag & O_NOFOLLOW)
			goto loosen_child;
		advance_mount_point(&look->mnt,&fnode_cache);

		ret = -ENOENT;
		if (!fnode_cache->file_node)
			goto loosen_child;
	}

	/**
	 * 运行到这里，文件节点一定是存在的
	 * 处理最后一个节点是符号链接的情况
	 */
	if (fnode_cache->file_node->node_ops && fnode_cache->file_node->node_ops->follow_link)
		goto advance_symlink;

	/**
	 * 如果最后一级是目录，也失败
	 */
	ret = -EISDIR;
	if (fnode_cache->file_node && S_ISDIR(fnode_cache->file_node->mode))
		goto loosen_child;

	ret = may_open(look->mnt, fnode_cache, flag);
	loosen_filenode_cache(look->filenode_cache);
	/**
	 * 上层调用者需要这个结果
	 */
	look->filenode_cache = fnode_cache;

	if (ret)
		goto loosen_look;

	return 0;

loosen_child:
	loosen_filenode_cache(fnode_cache);
loosen_look:
	loosen_fnode_look(look);
	return ret;
advance_symlink:
	ret = -ELOOP;
	if (flag & O_NOFOLLOW)
		goto loosen_child;

	/**
	 * 因为我们不考虑最后一个文件名
	 * 因此也就不用担心嵌套层次的问题
	 * 这里可以直接调用__advance_symlink而不是advance_symlink
	 */
	look->flags |= FNODE_LOOKUP_NOLAST;
	ret = __advance_symlink(fnode_cache, look);
	loosen_filenode_cache(fnode_cache);
	if (ret)
		return ret;

	look->flags &= ~FNODE_LOOKUP_NOLAST;
	ret = -EISDIR;
	if (look->path_type != PATHTYPE_NORMAL)
		goto loosen_look;

	if (look->last.name[look->last.len])
		goto loosen_look;

	/**
	 * 这里来判断嵌套层次的问题
	 */
	ret = -ELOOP;
	count++;
	if (count >= MAX_LINK_COUNT)
		goto loosen_look;

	/**
	 * 前面查找到父目录一级
	 * 这里在父目录中查找最后一个文件
	 */
	dir_cache = look->filenode_cache;
	down(&dir_cache->file_node->sem);
	fnode_cache = __find_file_indir_alloc(&look->last, look->filenode_cache, look);
	goto recognize_last;
}

/**
 * 删除文件
 */
asmlinkage int sys_unlink(const char *pathname)
{
	struct filenode_cache *fnode_cache;
	struct filenode_lookup_s look = {0};
	struct file_node *file_node = NULL;
	struct file_node *dir_node;
	char *name;
	int ret = 0;

	clone_user_string(pathname, &name);
	if(IS_ERR(name))
		return PTR_ERR(name);

	ret = load_filenode_cache(name, FNODE_LOOKUP_NOLAST, &look);
	if (ret)
		goto dicard;

	ret = -EISDIR;
	if (look.path_type != PATHTYPE_NORMAL)
		goto loosen;

	dir_node = look.filenode_cache->file_node;
	down(&dir_node->sem);
	fnode_cache = __find_file_indir_alloc(&look.last, look.filenode_cache, NULL);
	if (IS_ERR(fnode_cache)) {
		ret = PTR_ERR(fnode_cache);
		goto up;
	}

	file_node = fnode_cache->file_node;

	if (look.last.name[look.last.len]) {
		if (!file_node)
			ret = -ENOENT;
		else
			ret = S_ISDIR(file_node->mode) ? -EISDIR : -ENOTDIR;
	} else {
		if (!file_node)
			ret = -ENOENT;
		else if (IS_APPEND(dir_node))
			ret = -EPERM;
		else if (IS_APPEND(file_node) || IS_IMMUTABLE(file_node))
			ret = -EPERM;
		else if (S_ISDIR(file_node->mode))
			ret = -EISDIR;
		else if (!dir_node->node_ops || !dir_node->node_ops->unlink) 
			ret = -EPERM;
		else {
			accurate_inc(&file_node->ref_count);
			down(&fnode_cache->file_node->sem);
			if (fnode_cache->mount_count)
				ret = -EBUSY;
			else
				ret = dir_node->node_ops->unlink(dir_node, fnode_cache);
			up(&fnode_cache->file_node->sem);

			if (!ret)
				remove_fnode_cache(fnode_cache);
			loosen_file_node(file_node);	/* truncate the file_node here */
		}
		//	ret = vfs_unlink(dir_node, fnode_cache);
	}

	loosen_filenode_cache(fnode_cache);	
up:
	up(&dir_node->sem);
loosen:
	loosen_fnode_look(&look);
dicard:
	discard_user_string(name);
	return ret;
}

asmlinkage int sys_link(const char *oldname, const char *newname)
{
	return -ENOSYS;
}

asmlinkage int sys_linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath)
{
	return -ENOSYS;
}

asmlinkage int sys_unlinkat(int dirfd, const char *pathname, int flags)
{
	return -ENOSYS;
}

asmlinkage long sys_mkdir(const char __user * pathname, int mode)
{
	struct filenode_cache *fnode_cache;
	struct filenode_lookup_s look = {0};
	struct file_node *dir_node;
	int ret = 0;
	char *name;

	clone_user_string(pathname, &name);
	if (IS_ERR(name))
		return PTR_ERR(name);

	/**
	 * 装载父目录的信息到内存中
	 */
	ret = load_filenode_cache(name, FNODE_LOOKUP_NOLAST, &look);
	if (ret || (look.path_type != PATHTYPE_NORMAL))
		goto discard;

	down(&look.filenode_cache->file_node->sem);

	/**
	 * 在目录 中搜索文件名
	 */
	look.flags &= ~FNODE_LOOKUP_NOLAST;
	fnode_cache = __find_file_indir_alloc(&look.last, look.filenode_cache, NULL);
	if (IS_ERR(fnode_cache)) {
		ret = PTR_ERR(fnode_cache);
		goto unlock;
	}

	if (!IS_POSIXACL(look.filenode_cache->file_node))
		mode &= ~current->fs_context->umask;

	dir_node = look.filenode_cache->file_node;
	/**
	 * 子路径已经与文件节点绑定
	 * 说明子路径已经存在了
	 */
	if (fnode_cache->file_node)
		ret = -EEXIST;
	/**
	 * 目录节点**不是**目录
	 */
	else if (!dir_node->node_ops || !dir_node->node_ops->mkdir)
		ret = -EPERM;
	/**
	 * 否则调用文件系统的方法创建目录
	 */
	else
		ret = dir_node->node_ops->mkdir(dir_node, fnode_cache, mode);

	loosen_filenode_cache(fnode_cache);
unlock:
	up(&look.filenode_cache->file_node->sem);
	loosen_fnode_look(&look);
discard:
	discard_user_string(name);
	return ret;
}

static inline int do_rename(const char * oldname, const char * newname)
{
	return -ENOSYS;
}

asmlinkage int sys_rename(const char __user * oldname, const char __user * newname)
{
	int error;
	char * from;
	char * to;

	clone_user_string(oldname, &from);
	if(IS_ERR(from))
		return PTR_ERR(from);

	clone_user_string(newname, &to);
	error = PTR_ERR(to);
	if (!IS_ERR(to)) {
		error = do_rename(from,to);
		discard_user_string(to);
	}
	discard_user_string(from);

	return error;
}

/**
 * 通常的处理符号链接方法
 */
int generic_symlink(struct file_node *fnode, const char *symname, int len)
{
	struct file_cache_space *cache_space = fnode->cache_space;
	struct page_frame *page;
	int err = -ENOMEM;
	char *kaddr;

	err = -EINVAL;
	if (len >= PAGE_CACHE_SIZE)
		return err;

	/**
	 * 无法分配页面缓存
	 */
	page = grab_page_cache(cache_space, 0);
	if (!page)
		return err;

	/**
	 * 告诉文件系统，想要写页面了
	 * 请文件系统完成自己的准备工作
	 */
	err = cache_space->ops->prepare_write(NULL, page, 0, len - 1);
	if (err)
		goto loosen;

	/**
	 * 映射页面到内核地址，并写入符号链接的内容
	 */
	kaddr = kmap_atomic(page, KM_USER0);
	memcpy(kaddr, symname, len - 1);
	kunmap_atomic(kaddr, KM_USER0);

	/**
	 * 告诉文件系统，文件页面已经写完了
	 * 请文件系统做一些收尾的工作
	 */
	cache_space->ops->commit_write(NULL, page, 0, len - 1);

	mark_fnode_dirty(fnode);
loosen:
	unlock_page(page);
	loosen_page_cache(page);

	return err;
}

/**
 * 通常的读取符号链接内容回调函数
 */
int generic_read_link(struct filenode_cache *fnode_cache,
	char __user *buffer, int buflen)
{
	struct filenode_lookup_s look = {0};
	int ret;

	if (buflen <= 0)
		return -EINVAL;

	/**
	 * 调用文件系统的回调，将符号链接的内容读到缓存中
	 */
	ret = fnode_cache->file_node->node_ops->follow_link(fnode_cache, &look);
	if (!ret) {
		const char *symlink_name = look_curr_symlink_name(&look);

		if (IS_ERR(symlink_name)) {
			ret = PTR_ERR(symlink_name);
			goto loosen;
		}
	
		ret = strlen(symlink_name);
		if (ret > buflen) {
			ret = -E2BIG;
			goto loosen;
		}

		if (copy_to_user(buffer, symlink_name, ret))
			ret = -EFAULT;
loosen:
		if (fnode_cache->file_node->node_ops->loosen_link)
			fnode_cache->file_node->node_ops->loosen_link(fnode_cache, &look);
	}

	return ret;
}

/**
 * 将符号链接的内容读取到内存中
 */
int generic_follow_link(struct filenode_cache *fnode_cache,
	struct filenode_lookup_s *look)
{
	struct page_frame *page;
	struct file_cache_space *cache_space;

	cache_space = fnode_cache->file_node->cache_space;

	/**
	 * 读取文件内容的第一个页面
	 * 默认符号链接不会超过一个页面大小
	 */
	page = read_cache_page(cache_space, 0, NULL);
	if (IS_ERR(page))
		return PTR_ERR(page);

	wait_on_page_locked(page);
	if (!pgflag_uptodate(page)) {
		loosen_page_cache(page);
		return -EIO;
	}

	look_save_symlink_name(look, kmap(page));

	return 0;
}

void generic_loosen_link(struct filenode_cache *fnode_cache, struct filenode_lookup_s *look)
{
	if (!IS_ERR(look_curr_symlink_name(look))) {
		struct page_frame *page;

		page = pgcache_find_page(fnode_cache->file_node->cache_space, 0);
		ASSERT(page);

		kunmap(page);
		/**
		 * 释放find_page_cache的引用
		 */
		loosen_page_cache(page);
		/**
		 * 释放follow_link中的引用
		 */
		loosen_page_cache(page);
	}
}

/**
 * 通常的符号链接文件回调表
 */
struct file_node_ops generic_symlink_fnode_ops = {
	.read_link	= generic_read_link,
	.follow_link	= generic_follow_link,
	.loosen_link	= generic_loosen_link,
};
