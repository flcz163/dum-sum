#include <dim-sum/beehive.h>
#include <dim-sum/blk_infrast.h>
#include <dim-sum/err.h>
#include <dim-sum/fnode.h>
#include <dim-sum/magic.h>
#include <dim-sum/mount.h>
#include <dim-sum/pagemap.h>
#include <dim-sum/sched.h>
#include <dim-sum/smp_rwlock.h>
#include <dim-sum/stacktrace.h>
#include <dim-sum/syscall.h>

#include "devfs.h"

static struct devfs_file_node *root_node;
static struct smp_lock devfs_lock = SMP_LOCK_UNLOCKED(devfs_lock);

static unsigned long fnode_num = 1;

static struct file_node *devfs_creat_fnode(struct super_block *super,
	struct devfs_file_node *node, struct filenode_cache *fnode_cache);

static struct devfs_file_node *next_node(struct devfs_file_node *node)
{
	struct double_list *next = node->child.next;

	return list_container(next, struct devfs_file_node, child);
}

static struct devfs_file_node *fnode_to_devfs(struct file_node *fnode)
{
	if (fnode == NULL)
		return NULL;

	return fnode->private;
}

static struct devfs_file_node *
hold_devfs_node(struct devfs_file_node *node)
{
	if (node)
		accurate_inc(&node->ref_count);

	return node;
}

static void
loosen_devfs_node(struct devfs_file_node *node)
{
	if (!node)
		return;

	if (!accurate_dec_and_test_zero(&node->ref_count))
		return;

	if (node == root_node)
		panic("devfs: try to free root node.\n");

	if (S_ISLNK(node->mode))
		kfree(node->symlink.linkname);

	kfree(node);
}

static struct devfs_file_node *
devfs_alloc(const char *name, unsigned int len, umode_t mode)
{
	struct devfs_file_node *ret;

	if (name && len <= 0)
		len = strlen(name);

	ret = kmalloc(sizeof(*ret) + len + 1, PAF_KERNEL | __PAF_ZERO);
	if (!ret)
		return NULL;

	accurate_set(&ret->ref_count, 1);
	ret->mode = mode;
	ret->name_len = len;
	if (name)
		memcpy(ret->name, name, len);
	smp_lock(&devfs_lock);
	fnode_num++;
	ret->node_num = fnode_num;
	smp_unlock(&devfs_lock);
	if (S_ISDIR(mode)) {
		smp_rwlock_init(&ret->dir.lock);
		list_init(&ret->dir.children);
	}
	list_init(&ret->child);

	return ret;
}

/**
 * 单实例分配根节点
 */
static struct devfs_file_node *devfs_alloc_root(void)
{
	struct devfs_file_node *node;


	if (root_node)
		return root_node;

	node = devfs_alloc(NULL, 0, MODE_DIR);
	if (!node) {
		panic("devfs: fail to alloc root node.\n");
		return NULL;
	}

	smp_lock(&devfs_lock);
	if (root_node) {
		smp_unlock(&devfs_lock);
		loosen_devfs_node(node);
		return root_node;
	}
	root_node = node;
	smp_unlock(&devfs_lock);

	return root_node;
}

static void devfs_remove_fnode_cache(struct devfs_file_node *node)
{
	struct filenode_cache *fnode_cache;

	smp_lock(&filenode_cache_lock);

	fnode_cache = node->fnode_cache;
	if (!fnode_cache) {
		smp_unlock(&filenode_cache_lock);
		return;
	}

	if (fnode_cache->file_node != NULL)
		fnode_cache->file_node->link_count = 0;
	takeout_fnode_cache(fnode_cache);

	smp_unlock(&filenode_cache_lock);
}

/**
 * 从父节点的链表中摘除
 */
static int devfs_disconnect(struct devfs_file_node *node)
{
	if (!node || list_is_empty(&node->child))
		return false;

	list_del_init(&node->child);

	return true;
}

static int cache_ops_may_delete(struct filenode_cache *fnode_cache)
{
	return !!fnode_cache->file_node;
}

static void cache_ops_loosen(struct filenode_cache *fnode_cache,
	struct file_node *fnode)
{
	struct devfs_file_node *node;

	node = fnode_to_devfs(fnode);

	if (node->fnode_cache && (node->fnode_cache != fnode_cache))
		panic("(%s): node: %p filenode_cache: %p node->filenode_cache: %p\n",
		     node->name, node, fnode_cache, node->fnode_cache);

	node->fnode_cache = NULL;
	loosen_file_node(fnode);
	loosen_devfs_node(node);
}

static struct fnode_cache_ops devfs_fnode_cache_ops = {
	.may_delete = cache_ops_may_delete,
	.loosen_file_node = cache_ops_loosen,
};

/**
 * 根据文件名，在目录中搜索指定的文件
 */
static struct devfs_file_node *__find_in_dir(struct devfs_file_node *dir,
					     const char *name,
					     unsigned int len)
{
	struct devfs_file_node *node;
	struct double_list *list;

	if (!S_ISDIR(dir->mode)) {
		printk("(%s): not a directory\n", dir->name);
		return NULL;
	}

	list_for_each(list, &dir->dir.children) {
		node = list_container(list, struct devfs_file_node, child);

		if (node->name_len != len)
			continue;

		if (memcmp(node->name, name, len) == 0)
			return hold_devfs_node(node);
	}

	return NULL;
}

/**
 * 将文件节点与目录绑定起来
 */
static int devfs_node_stick(struct devfs_file_node *dir,
	struct devfs_file_node *node, struct devfs_file_node **pold)
{
	int ret;

	if (pold)
		*pold = NULL;

	if (!S_ISDIR(dir->mode)) {
		printk("(%s): dir: \"%s\" is not a directory\n", node->name,
		       dir->name);
		loosen_devfs_node(node);

		return -ENOTDIR;
	}

	smp_write_lock(&dir->dir.lock);
	/**
	 * 目录已经被移除失效了
	 */
	if (dir->dir.inactive)
		ret = -ENOENT;
	else {
		struct devfs_file_node *old;

		old = __find_in_dir(dir, node->name, node->name_len);
		if (pold)
			*pold = old;
		else
			loosen_devfs_node(old);

		if (old == NULL) {
			node->parent = dir;
			list_insert_behind(&node->child, &dir->dir.children);
			ret = 0;
		} else
			ret = -EEXIST;
	}
	smp_write_unlock(&dir->dir.lock);

	if (ret)
		loosen_devfs_node(node);

	return ret;
}

static struct devfs_file_node *
advance_path(struct devfs_file_node *dir, const char *name, int len,
	int *next_pos)
{
	struct devfs_file_node *entry;
	const char *tail, *ptr;

	tail = name + len;
	for (ptr = name; (ptr < tail) && (*ptr != '/'); ++ptr);

	*next_pos = ptr - name;
	if ((*next_pos == 2) && (strncmp(name, "..", 2) == 0)) {
		*next_pos = 2;
		return hold_devfs_node(dir->parent);
	} else if ((*next_pos == 1) && (strncmp(name, ".", 1) == 0)) {
		*next_pos = 1;
		return hold_devfs_node(dir);
	}

	smp_read_lock(&dir->dir.lock);
	entry = __find_in_dir(dir, name, *next_pos);
	smp_read_unlock(&dir->dir.lock);

	return entry;
}

/**
 * 加载目录节点，并得到文件名的位置
 */
static struct devfs_file_node *
load_dir_node(struct devfs_file_node *dir, const char *name,
	int len, int *fname_pos)
{
	int next_pos = 0;
	int pos;

	if (len == 0)
		return NULL;
	pos = len - 1;

	if (dir == NULL)
		dir = devfs_alloc_root();

	if (dir == NULL)
		return NULL;

	hold_devfs_node(dir);

	while ((pos > 0) && (name[pos] != '/'))
		pos--;
	*fname_pos = 0;
	if (name[pos] == '/')
		*fname_pos = pos + 1;

	while (pos > 0) {
		struct devfs_file_node *node, *old = NULL;

		node = advance_path(dir, name, pos, &next_pos);
		if (!node) {
			node = devfs_alloc(name, next_pos, MODE_DIR);
			if (!node)
				return NULL;

			hold_devfs_node(node);
			if (devfs_node_stick(dir, node, &old)) {
				loosen_devfs_node(node);
				/**
				 * 万幸，目录竟然存在
				 */
				if (old && S_ISDIR(old->mode))
					node = old;
				else{
					loosen_devfs_node(old);
					loosen_devfs_node(dir);
					return NULL;
				}
			}
		}

		if (node == dir->parent) {
			loosen_devfs_node(dir);
			loosen_devfs_node(node);
			return NULL;
		}

		loosen_devfs_node(dir);
		dir = node;
		if (name[next_pos] == '/')
			next_pos++;

		name += next_pos;
		pos -= next_pos;
	}

	return dir;
}

static struct devfs_file_node *
load_dir_node_alloc(struct devfs_file_node **dir, const char *name,
	umode_t mode)
{
	struct devfs_file_node *ret;
	int len, fname_pos;

	len = strlen(name);
	*dir = load_dir_node(*dir, name, len, &fname_pos);
	if (*dir == NULL)
		return ERR_PTR(-ENOTDIR);

	ret = devfs_alloc(name + fname_pos, len - fname_pos, mode);
	if (!ret) {
		loosen_devfs_node(*dir);
		return ERR_PTR(-ENOMEM);
	}

	return ret;
}

/**
 * 查找文件节点
 */
static struct devfs_file_node *
load_file_node(struct devfs_file_node *dir, const char *name,
	int len, bool read_link)
{
	int next_pos = 0;

	current->fs_search.nested_count++;
	if (current->fs_search.nested_count >= MAX_NESTED_LINKS)
		return NULL;

	if (dir == NULL)
		dir = devfs_alloc_root();

	if (dir == NULL)
		return NULL;

	hold_devfs_node(dir);
	while (len > 0) {
		struct devfs_file_node *node, *link;

		if (!S_ISDIR(dir->mode)) {
			loosen_devfs_node(dir);
			return NULL;
		}

		node = advance_path(dir, name, len, &next_pos);
		if (node == NULL) {
			loosen_devfs_node(dir);
			return NULL;
		}

		if (S_ISLNK(node->mode) && read_link) {
			link = load_file_node(dir, node->symlink.linkname,
						node->symlink.length, true);
			loosen_devfs_node(node);
			if (!link) {
				loosen_devfs_node(dir);
				return NULL;
			}
			node = link;
		}
		loosen_devfs_node(dir);
		dir = node;
		if (name[next_pos] == '/')
			next_pos++;

		name += next_pos;
		len -= next_pos;
	}

	return dir;
}

static struct filenode_cache *
devfs_lookup(struct file_node *dir, struct filenode_cache *fnode_cache,
	struct filenode_lookup_s *look)
{
	struct devfs_file_node *dir_node, *node;
	struct filenode_cache *ret = NULL;
	struct file_node *fnode;

	dir_node = fnode_to_devfs(dir);
	smp_read_lock(&dir_node->dir.lock);
	node = __find_in_dir(dir_node, fnode_cache->file_name.name,
			       fnode_cache->file_name.len);
	smp_read_unlock(&dir_node->dir.lock);

	if (!node)
		goto out;

	/**
	 * 文件节点存在，为它创建VFS层节点描述符
	 */
	fnode = devfs_creat_fnode(dir->super, node, fnode_cache);
	if (!fnode) {
		ret = ERR_PTR(-ENOMEM);
		goto out;
	}
	
	fnode_cache_stick(fnode_cache, fnode);

out:
	smp_write_lock(&dir_node->dir.lock);
	fnode_cache->ops = &devfs_fnode_cache_ops;
	smp_write_unlock(&dir_node->dir.lock);
	loosen_devfs_node(node);

	return ret;
}

static int devfs_unlink(struct file_node *dir, struct filenode_cache *fnode_cache)
{
	struct devfs_file_node *dev_node;
	struct file_node *fnode;
	int connected;

	fnode = fnode_cache->file_node;
	dev_node = fnode_to_devfs(fnode);
	if (!dev_node)
		return -ENOENT;

	if (!dev_node->may_delete)
		return -EPERM;

	smp_write_lock(&dev_node->parent->dir.lock);
	connected = devfs_disconnect(dev_node);
	smp_write_unlock(&dev_node->parent->dir.lock);
	if (!connected)
		return -ENOENT;
	
	devfs_remove_fnode_cache(dev_node);
	loosen_devfs_node(dev_node);

	return 0;
}

static int
do_symlink(struct devfs_file_node *dir, const char *name,
	const char *link, struct devfs_file_node **pnode)
{
	struct devfs_file_node *node;
	unsigned int len;
	char *symname;
	int err;

	if (pnode != NULL)
		*pnode = NULL;

	if (!name || !link)
		return -EINVAL;

	len = strlen(link);
	symname = kmalloc(len + 1, PAF_KERNEL);
	if (!symname)
		return -ENOMEM;
	memcpy(symname, link, len);
	symname[len] = '\0';

	node = load_dir_node_alloc(&dir, name, S_IFLNK | S_IRUGO | S_IXUGO);
	if (IS_ERR(node)) {
		kfree(symname);
		return PTR_ERR(node);
	}

	node->symlink.linkname = symname;
	node->symlink.length = len;

	err = devfs_node_stick(dir, node, NULL);
	if (!err && pnode)
		*pnode = node;

	loosen_devfs_node(dir);
	return err;
}

static int
devfs_symlink(struct file_node *dir, struct filenode_cache *fnode_cache,
	const char *symlink)
{
	struct devfs_file_node *dir_node, *node;
	struct file_node *fnode;
	int err;

	dir_node = fnode_to_devfs(dir);
	if (!dir_node)
		return -ENOENT;

	err = do_symlink(dir_node, fnode_cache->file_name.name,
		symlink, &node);
	if (err < 0)
		return err;

	node->may_delete = true;
	node->uid = current->euid;
	node->gid = current->egid;
	node->access_time = CURRENT_TIME;
	node->data_modify_time = CURRENT_TIME;
	node->meta_modify_time = CURRENT_TIME;

	fnode = devfs_creat_fnode(dir->super, node, fnode_cache);
	if (!fnode)
		return -ENOMEM;

	fnode_cache_stick(fnode_cache, fnode);
	
	return 0;
}

static int devfs_mkdir(struct file_node *dir,
	struct filenode_cache *fnode_cache, int mode)
{
	struct devfs_file_node *dir_node, *node;
	struct file_node *fnode;
	int err;

	mode = (mode & ~S_IFMT) | S_IFDIR;
	dir_node = fnode_to_devfs(dir);
	if (!dir_node)
		return -ENOENT;

	node = devfs_alloc(fnode_cache->file_name.name,
		fnode_cache->file_name.len, mode);
	if (!node)
		return -ENOMEM;

	node->uid = current->euid;
	node->gid = current->egid;
	node->access_time = CURRENT_TIME;
	node->data_modify_time = CURRENT_TIME;
	node->meta_modify_time = CURRENT_TIME;
	node->may_delete = true;

	err = devfs_node_stick(dir_node, node, NULL);
	if (err)
		return err;

	fnode = devfs_creat_fnode(dir->super, node, fnode_cache);
	if (!fnode)
		return -ENOMEM;

	fnode_cache_stick(fnode_cache, fnode);
	
	return 0;
}	

static int devfs_rmdir(struct file_node *dir, struct filenode_cache *fnode_cache)
{
	struct devfs_file_node *dev_node;
	struct file_node *fnode;
	bool connected;
	int err = 0;

	fnode = fnode_cache->file_node;
	if (dir->super->fs_info != fnode->super->fs_info)
		return -EINVAL;

	dev_node = fnode_to_devfs(fnode);
	if (dev_node == NULL)
		return -ENOENT;

	if (!S_ISDIR(dev_node->mode))
		return -ENOTDIR;

	if (!dev_node->may_delete)
		return -EPERM;

	smp_write_lock(&dev_node->dir.lock);
	if (!list_is_empty(&dev_node->dir.children))
		err = -ENOTEMPTY;
	else
		dev_node->dir.inactive = true;
	smp_write_unlock(&dev_node->dir.lock);

	if (err)
		return err;

	smp_write_lock(&dev_node->parent->dir.lock);
	connected = devfs_disconnect(dev_node);
	if (!connected)
		err = -ENOENT;
	smp_write_unlock(&dev_node->parent->dir.lock);

	if (err)
		return err;

	devfs_remove_fnode_cache(dev_node);
	loosen_devfs_node(dev_node);

	return 0;
}	

static int
devfs_mknod(struct file_node *dir, struct filenode_cache *fnode_cache,
	int mode, devno_t dev_num)
{
	struct devfs_file_node *dir_node, *node;
	struct file_node *fnode;
	int err = 0;

	dir_node = fnode_to_devfs(dir);
	if (!dir_node)
		return -ENOENT;

	node = devfs_alloc(fnode_cache->file_name.name,
		fnode_cache->file_name.len, mode);
	if (!node)
		return -ENOMEM;

	switch (mode & S_IFMT) {
	case S_IFCHR:
	case S_IFBLK:
		node->dev = dev_num;
	case 0:
		mode = S_IFREG;
	case S_IFREG:
	case S_IFIFO:
	case S_IFSOCK:
		break;
	case S_IFDIR:
		err = -EPERM;
		break;
	default:
		err = -EINVAL;
		break;
	}
	if (err)
		return err;

	node->may_delete = true;
	node->uid = current->euid;
	node->gid = current->egid;
	node->access_time = CURRENT_TIME;
	node->data_modify_time = CURRENT_TIME;
	node->meta_modify_time = CURRENT_TIME;

	err = devfs_node_stick(dir_node, node, NULL);
	if (err)
		return err;

	fnode = devfs_creat_fnode(dir->super, node, fnode_cache);
	if (!fnode)
		return -ENOMEM;

	fnode_cache_stick(fnode_cache, fnode);

	return 0;
}

static int devfs_follow_link(struct filenode_cache *fnode_cache,
	struct filenode_lookup_s *look)
{
	struct devfs_file_node *dev_node = fnode_to_devfs(fnode_cache->file_node);

	if (dev_node)
		look_save_symlink_name(look, dev_node->symlink.linkname);
	else
		look_save_symlink_name(look, ERR_PTR(-ENODEV));
	
	return 0;
}

static int devfs_readdir(struct file *file, void *dirent, filldir_t filldir)
{
	struct devfs_file_node *dev_node, *node = NULL, *next = NULL;
	struct file_node *fnode;
	struct double_list *list;
	int err, count;
	int stored = 0;

	if ((long)file->pos < 0)
		return -EINVAL;

	fnode = file->fnode_cache->file_node;
	dev_node = fnode_to_devfs(file->fnode_cache->file_node);
	
	switch ((long)file->pos) {
	case 0:
		err = filldir(dirent, "..", 2, file->pos,
			parent_fnode_num(file->fnode_cache), DT_DIR);

		if (err == -EINVAL)
			break;

		if (err < 0)
			return err;

		file->pos++;
		stored++;
	case 1:
		err = filldir(dirent, ".", 1, file->pos,
			fnode->node_num, DT_DIR);

		if (err == -EINVAL)
			break;

		if (err < 0)
			return err;

		file->pos++;
		stored++;
	default:
		count = file->pos - 1;
		smp_read_lock(&dev_node->dir.lock);
		list_for_each (list, &dev_node->dir.children) {
			node = list_container(list, struct devfs_file_node, child);

			count--;
			if (count <= 0)
				break;

			if (node->child.next == &dev_node->dir.children) {
				node = NULL;
				break;
			}
		}
 		hold_devfs_node(node);
		smp_read_unlock(&dev_node->dir.lock);

		while (node) {
			err = (*filldir) (dirent, node->name, node->name_len,
					  file->pos, node->node_num, node->mode >> 12);
			if (err < 0)
				loosen_devfs_node(node);
			else {
				file->pos++;
				stored++;
			}
		
			if (err == -EINVAL)
				break;

			if (err < 0)
				return err;

			smp_read_lock(&dev_node->dir.lock);
			if (node->child.next == &dev_node->dir.children)
				next = NULL;
			else
				next = hold_devfs_node(next_node(node));
			smp_read_unlock(&dev_node->dir.lock);
			loosen_devfs_node(node);
			node = next;
		}

		break;
	}

	return stored;
}

static struct file_node_ops devfs_dir_fnode_ops = {
	.lookup = devfs_lookup,
	.unlink = devfs_unlink,
	.symlink = devfs_symlink,
	.mkdir = devfs_mkdir,
	.rmdir = devfs_rmdir,
	.mknod = devfs_mknod,
};

static struct file_node_ops devfs_symlink_fnode_ops = {
	.read_link = generic_read_link,
	.follow_link = devfs_follow_link,
};

static struct file_ops devfs_dir_file_ops = {
	.read = generic_read_dir,
	.readdir = devfs_readdir,
};

static struct super_block_ops devfs_superblock_ops = {
	.release = generic_delete_fnode,
	.statfs = simple_statfs,
};

int devfs_remove(const char *fmt, ...)
{
	struct devfs_file_node *node;
	int connected;
	char buf[64];
	char *name;
	va_list args;
	int len;

	va_start(args, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, args);
	if (len >= sizeof(buf) || !buf[0])
		return -EINVAL;

	name = buf;
	/**
	 * 去掉前导的'/'
	 */
	if (name[0] == '/') {
		while (*name == '/') {
			name++;
			len--;
		}
		if (!len)
			return -EINVAL;
	}

	current->fs_search.nested_count = 0;
	node = load_file_node(NULL, name, len, false);

	smp_write_lock(&node->parent->dir.lock);

	connected = devfs_disconnect(node);
	if (!connected) {
		smp_write_unlock(&node->parent->dir.lock);
		loosen_devfs_node(node);
		return 0;
	}

	if (S_ISDIR(node->mode) && !list_is_empty(&node->dir.children)) {
		smp_write_unlock(&node->parent->dir.lock);
		loosen_devfs_node(node);
		return -ENOTEMPTY;
	}

	smp_write_unlock(&node->parent->dir.lock);

	devfs_remove_fnode_cache(node);
	/**
	 * 注意，是释放*两次*
	 */
	loosen_devfs_node(node);
	loosen_devfs_node(node);

	return 0;
}

/**
 * 在devfs中创建一个目录
 * 以devfs根目录为起始点
 */
int devfs_mk_dir(const char *fmt, ...)
{
	struct devfs_file_node *dir = NULL, *node = NULL, *old;
	int error, len;
	char buf[64];
	va_list args;

	va_start(args, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, args);
	if (len >= sizeof(buf) || !buf[0])
		return -EINVAL;

	node = load_dir_node_alloc(&dir, buf, MODE_DIR);
	if (IS_ERR(node))
		return PTR_ERR(node);

	error = devfs_node_stick(dir, node, &old);
	if (error == -EEXIST && S_ISDIR(old->mode))
		error = 0;
	else if (error)
		loosen_devfs_node(old);

	loosen_devfs_node(dir);
	return error;
}

/**
 * 在devfs文件系统中，创建一个符号链接节点
 */
int devfs_mk_symlink(const char *from, const char *to)
{
	struct devfs_file_node *node;
	int err;

	err = do_symlink(NULL, from, to, &node);
	if (!err)
		node->may_delete = true;

	return err;
}

static int
devfs_mk_dev(devno_t dev, umode_t mode, const char *fmt, va_list args)
{
	struct devfs_file_node *dir = NULL, *node;
	char buf[64];
	int error, len;

	len = vsnprintf(buf, sizeof(buf), fmt, args);
	if (WARN_ON(len >= sizeof(buf) || !buf[0],
	    "%s: invalid format string %s\n", __FUNCTION__, fmt))
		return -EINVAL;

	/**
	 * 从根节点开始，查找设备文件的路径
	 * 并创建空文件节点
	 */
	node = load_dir_node_alloc(&dir, buf, mode);
	if (IS_ERR(node))
		return PTR_ERR(node);

	/**
	 * 将新创建的文件节点与上级目录绑定
	 */
	node->dev = dev;
	error = devfs_node_stick(dir, node, NULL);
	loosen_devfs_node(dir);

	return error;
}

int devfs_mk_blkdev(devno_t dev, umode_t mode, const char *fmt, ...)
{
	va_list args;

	if (WARN_ON(!S_ISBLK(mode), "%s: invalide mode (%u) for %s\n",
	    __FUNCTION__, mode, fmt))
		return -EINVAL;

	va_start(args, fmt);

	return devfs_mk_dev(dev, mode, fmt, args);
}

int devfs_mk_chrdev(devno_t dev, umode_t mode, const char *fmt, ...)
{
	va_list args;

	if (WARN_ON(!S_ISCHR(mode), "%s: invalide mode (%u) for %s\n",
	    __FUNCTION__, mode, fmt))
		return -EINVAL;

	va_start(args, fmt);

	return devfs_mk_dev(dev, mode, fmt, args);
}

static struct file_node *
devfs_creat_fnode(struct super_block *super, struct devfs_file_node *node,
	struct filenode_cache *fnode_cache)
{
	struct file_node *fnode;

	/**
	 * 分配一个VFS文件节点
	 */
	fnode = fnode_alloc(super);
	if (!fnode) {
		printk("(%s): failed to alloc file node, node: %p\n", node->name, node);
		return NULL;
	}

	if (node != root_node) {
		smp_read_lock(&node->parent->dir.lock);

		if (list_is_empty(&node->child)) {
			smp_read_unlock(&node->parent->dir.lock);
			loosen_file_node(fnode);
			return NULL;
		}

		node->fnode_cache = fnode_cache;
		smp_read_unlock(&node->parent->dir.lock);
	} else
		node->fnode_cache = fnode_cache;

	fnode->private = hold_devfs_node(node);
	fnode->node_num = node->node_num;
	fnode->block_count = 0;
	fnode->block_size = 1024;
	fnode->mode = node->mode;
	if (S_ISDIR(node->mode)) {
		fnode->node_ops = &devfs_dir_fnode_ops;
		fnode->file_ops = &devfs_dir_file_ops;
	} else if (S_ISLNK(node->mode)) {
		fnode->node_ops = &devfs_symlink_fnode_ops;
		fnode->file_size = node->symlink.length;
	} else if (S_ISCHR(node->mode) || S_ISBLK(node->mode))
		init_special_filenode(fnode, node->mode, node->dev);
	else if (S_ISFIFO(node->mode) || S_ISSOCK(node->mode))
		init_special_filenode(fnode, node->mode, 0);
	else {
		printk("(%s): unknown mode %o node: %p\n",
		       node->name, node->mode, node);
		loosen_file_node(fnode);
		loosen_devfs_node(node);
		return NULL;
	}

	fnode->uid = node->uid;
	fnode->gid = node->gid;
	fnode->access_time = node->access_time;
	fnode->data_modify_time = node->data_modify_time;
	fnode->meta_modify_time = node->meta_modify_time;

	return fnode;
}

static int devfs_fill_super(struct super_block *super, void *data, int silent)
{
	struct file_node *fnode = NULL;
	struct devfs_file_node *root;

	root = devfs_alloc_root();
	if (!root)
		goto fail;

	super->fs_info = NULL;
	super->block_size = 1024;
	super->block_size_order = 10;
	super->magic = DEVFS_SUPER_MAGIC;
	super->ops = &devfs_superblock_ops;
	super->gran_ns = 1;

	if ((fnode = devfs_creat_fnode(super, root, NULL)) == NULL)
		goto fail;

	super->root_fnode_cache = fnode_cache_alloc_root(fnode);
	if (!super->root_fnode_cache)
		goto fail;
	
	return 0;

fail:
	printk("devfs: fail to fill super_block.\n");
	if (fnode)
		loosen_file_node(fnode);

	return -EINVAL;
}

static struct super_block *
devfs_load_filesystem(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return load_single_filesystem(fs_type, flags, data, devfs_fill_super);
}

static struct file_system_type devfs_type = {
	.name = "devfs",
	.load_filesystem = devfs_load_filesystem,
	.unload_filesystem = unload_isolate_filesystem,
};

void __init mount_devfs_fs(void)
{
	int err;

	sys_mkdir("/dev", 0);

	err = do_mount("none", "/dev", "devfs", 0, NULL);
	if (err == 0)
		printk("Mounted devfs on /dev\n");
	else
		printk("Unable to mount devfs, err: %d\n", err);
}

int __init init_devfs(void)
{
	int err;

	devfs_alloc_root();

	err = register_filesystem(&devfs_type);

	return err;
}
