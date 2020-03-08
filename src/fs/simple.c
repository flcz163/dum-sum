#include <dim-sum/err.h>
#include <dim-sum/errno.h>
#include <dim-sum/fs.h>
#include <dim-sum/highmem.h>
#include <dim-sum/limits.h>
#include <dim-sum/mm.h>
#include <dim-sum/pagemap.h>
#include <dim-sum/statfs.h>

#include <asm/cacheflush.h>

int simple_statfs(struct super_block *sb, struct kstatfs *buf)
{
	buf->f_type = sb->magic;
	buf->f_bsize = PAGE_CACHE_SIZE;
	buf->f_namelen = NAME_MAX;
	return 0;
}

int simple_readpage(struct file *file, struct page_frame *page)
{
	void *kaddr;

	if (pgflag_uptodate(page))
		goto out;

	kaddr = kmap_atomic(page, KM_USER0);
	memset(kaddr, 0, PAGE_CACHE_SIZE);
	kunmap_atomic(kaddr, KM_USER0);
	flush_dcache_page(page);
	set_page_uptodate(page);
out:
	unlock_page(page);
	return 0;
}

int simple_prepare_write(struct file *file, struct page_frame *page,
			unsigned from, unsigned to)
{
	if (!pgflag_uptodate(page)) {
		if (to - from != PAGE_CACHE_SIZE) {
			void *kaddr = kmap_atomic(page, KM_USER0);

			memset(kaddr, 0, from);
			memset(kaddr + to, 0, PAGE_CACHE_SIZE - to);
			flush_dcache_page(page);
			kunmap_atomic(kaddr, KM_USER0);
		}
		set_page_uptodate(page);
	}

	return 0;
}

int simple_commit_write(struct file *file, struct page_frame *page,
			unsigned offset, unsigned to)
{
	struct file_node *fnode = page->cache_space->fnode;
	loff_t pos = ((loff_t)page->index << PAGE_CACHE_SHIFT) + to;

	if (pos > fnode->file_size)
		fnode_update_file_size(fnode, pos);
	set_page_dirty(page);

	return 0;
}

int simple_getattr(struct mount_desc *mnt, struct filenode_cache *filenode_cache,
		   struct file_attribute *stat)
{
	struct file_node *fnode = filenode_cache->file_node;

	generic_get_file_attribute(fnode, stat);
	stat->blocks = fnode->cache_space->page_count << (PAGE_CACHE_SHIFT - 9);

	return 0;
}

static int simple_delete_dentry(struct filenode_cache *filenode_cache)
{
	return 1;
}

static struct fnode_cache_ops simple_fnode_cache_ops = {
	.may_delete = simple_delete_dentry,
};

struct filenode_cache *simple_lookup(struct file_node *dir, struct filenode_cache *filenode_cache, struct filenode_lookup_s *look)
{
	if (filenode_cache->file_name.len > NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);

	filenode_cache->ops = &simple_fnode_cache_ops;
	fnode_cache_stick(filenode_cache, NULL);
	putin_fnode_cache(filenode_cache);
	
	return NULL;
}

int simple_link(struct filenode_cache *old_fnode_cache, struct file_node *dir, struct filenode_cache *filenode_cache)
{
	struct file_node *fnode = old_fnode_cache->file_node;

	fnode->meta_modify_time = dir->meta_modify_time = dir->data_modify_time = CURRENT_TIME;
	fnode->link_count++;
	accurate_inc(&fnode->ref_count);
	hold_dirnode_cache(filenode_cache);
	fnode_cache_stick(filenode_cache, fnode);

	return 0;
}

int simple_unlink(struct file_node *dir, struct filenode_cache *filenode_cache)
{
	struct file_node *fnode = filenode_cache->file_node;

	fnode->meta_modify_time = dir->meta_modify_time = dir->data_modify_time = CURRENT_TIME;
	fnode->link_count--;
	loosen_filenode_cache(filenode_cache);

	return 0;
}

static inline int simple_positive(struct filenode_cache *filenode_cache)
{
	return filenode_cache->file_node &&
		(filenode_cache->flags & FNODECACHE_INHASH);
}

int simple_empty(struct filenode_cache *filenode_cache)
{
	struct filenode_cache *child;
	int ret = 0;

	smp_lock(&filenode_cache_lock);
	list_for_each_entry(child, &filenode_cache->children, child)
		if (simple_positive(child))
			goto out;
	ret = 1;
out:
	smp_unlock(&filenode_cache_lock);
	return ret;
}

int simple_rmdir(struct file_node *dir, struct filenode_cache *filenode_cache)
{
	if (!simple_empty(filenode_cache))
		return -ENOTEMPTY;

	filenode_cache->file_node->link_count--;
	simple_unlink(dir, filenode_cache);
	dir->link_count--;

	return 0;
}

int simple_rename(struct file_node *old_dir,
	struct filenode_cache *old_fnode_cache,
	struct file_node *new_dir,
	struct filenode_cache *new_fnode_cache)
{
	struct file_node *fnode = old_fnode_cache->file_node;
	int is_dir = S_ISDIR(old_fnode_cache->file_node->mode);

	if (!simple_empty(new_fnode_cache))
		return -ENOTEMPTY;

	if (new_fnode_cache->file_node) {
		simple_unlink(new_dir, new_fnode_cache);
		if (is_dir)
			old_dir->link_count--;
	} else if (is_dir) {
		old_dir->link_count--;
		new_dir->link_count++;
	}

	old_dir->meta_modify_time = old_dir->data_modify_time = new_dir->meta_modify_time =
		new_dir->data_modify_time = fnode->meta_modify_time = CURRENT_TIME;

	return 0;
}

int simple_dir_open(struct file_node *fnode, struct file *file)
{
	static struct file_name cursor_name = {.len = 1, .name = "."};

	file->private_data = fnode_cache_alloc(file->fnode_cache, &cursor_name);

	return file->private_data ? 0 : -ENOMEM;
}

int simple_dir_close(struct file_node *fnode, struct file *file)
{
	loosen_filenode_cache(file->private_data);

	return 0;
}

loff_t simple_dir_lseek(struct file *file, loff_t offset, int origin)
{
	switch (origin) {
	case SEEK_CUR:
		offset += file->pos;
	case SEEK_SET:
		if (offset >= 0)
			break;
	default:
		return -EINVAL;
	}

	down(&file->fnode_cache->file_node->sem);

	if (offset != file->pos) {
		file->pos = offset;
		if (file->pos >= 2) {
			struct double_list *list;
			struct filenode_cache *first = file->private_data;
	
			loff_t n = file->pos - 2;

			smp_lock(&filenode_cache_lock);
			list_del(&first->child);
			list = file->fnode_cache->children.next;
			while (n && list != &file->fnode_cache->children) {
				struct filenode_cache *next;
				next = list_container(list, struct filenode_cache, child);
				if ((next->flags & FNODECACHE_INHASH) && next->file_node)
					n--;
				list = list->next;
			}
			list_insert_behind(&first->child, list);
			smp_unlock(&filenode_cache_lock);
		}
	}

	up(&file->fnode_cache->file_node->sem);

	return offset;
}

/**
 * 目录节点的read回调，直接返回错误
 */
ssize_t generic_read_dir(struct file *file, char __user *buf, size_t siz, loff_t *ppos)
{
	return -EISDIR;
}

/**
 * 目录的锁已经被持有了
 */
int simple_dir_readdir(struct file *file, void *dirent, filldir_t filldir)
{
	struct filenode_cache *filenode_cache = file->fnode_cache;
	struct filenode_cache *first = file->private_data;
	struct double_list *next_list;
	fnode_num_t node_num;
	int pos = file->pos;
	unsigned char dt_type;
	int fill_ret;

	switch (pos) {
	case 0:
		node_num = filenode_cache->file_node->node_num;
		if (filldir(dirent, ".", 1, pos, node_num, DT_DIR) < 0)
			break;
		file->pos++;
		pos++;
	case 1:
		node_num = parent_fnode_num(filenode_cache);
		if (filldir(dirent, "..", 2, pos, node_num, DT_DIR) < 0)
			break;
		file->pos++;
		pos++;
	default:
		smp_lock(&filenode_cache_lock);

		/**
		 * 开始真正的遍历文件
		 * 首先把'.'文件移动到最前面
		 */
		if (file->pos == 2)
			list_move_to_front(&first->child, &filenode_cache->children);

		/**
		 * 从.节点的下一个节点开始
		 */
		for (next_list = first->child.next;
		    next_list != &filenode_cache->children;
		    next_list = next_list->next) {
			struct filenode_cache *next_node;

			next_node = list_container(next_list, struct filenode_cache, child);
			if (!(next_node->flags & FNODECACHE_INHASH) || !next_node->file_node)
				continue;

			smp_unlock(&filenode_cache_lock);
			/**
			 * 将当前节点信息返回给上层
			 */
			dt_type = (next_node->file_node->mode >> 12) & 15;
			fill_ret = filldir(dirent, next_node->file_name.name,
				next_node->file_name.len, file->pos,
				next_node->file_node->node_num, dt_type);
			if (fill_ret < 0)
				return 0;

			smp_lock(&filenode_cache_lock);
			/**
			 * 这里有一个奇怪的地方
			 * next链表节点被两个链表所包含
			 */
			list_move_to_front(&first->child, next_list);
			file->pos++;
		}
		smp_unlock(&filenode_cache_lock);
	}

	return 0;
}

struct file_ops simple_dir_ops = {
	.open		= simple_dir_open,
	.release	= simple_dir_close,
	.llseek		= simple_dir_lseek,
	.read		= generic_read_dir,
	.readdir	= simple_dir_readdir,
};

/**
 * 对于基于缓存的内存文件系统来说
 * 没有任何需要同步的事情
 */
int simple_sync_file(struct file * file, struct filenode_cache *filenode_cache, int datasync)
{
	return 0;
}
