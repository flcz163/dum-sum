#include <dim-sum/beehive.h>
#include <dim-sum/boot_allotter.h>
#include <dim-sum/err.h>
#include <dim-sum/fnode.h>
#include <dim-sum/fs.h>
#include <dim-sum/fs_context.h>
#include <dim-sum/hash.h>
#include <dim-sum/mount.h>
#include <dim-sum/sched.h>

#include "internal.h"

aligned_cacheline_in_smp struct smp_lock filenode_cache_lock =
	SMP_LOCK_UNLOCKED(filenode_cache_lock);
aligned_cacheline_in_smp static struct smp_seq_lock rename_lock =
	SMP_SEQ_LOCK_UNLOCKED(rename_lock);
 
/**
 * 用于分配节点缓存的内存分配器。
 */
static struct beehive_allotter *fnode_cache_allotter; 

/**
 * 目录项的散列表。
 * 它是一个指针数组
 * 每个指针是一个具有相同散列值的目录项链表
 */
static struct hash_list_bucket *__hash;
static unsigned int __hash_order;
static unsigned int __hash_mask;

static int advance_symlink(struct filenode_cache *filenode_cache,
	struct filenode_lookup_s *look);
static int
__generic_load_filenode(char * name, struct filenode_lookup_s *look);

/**
 * 分配一个文件节点缓存对象
 */
struct filenode_cache *
fnode_cache_alloc(struct filenode_cache *parent,
	const struct file_name *name)
{
	struct filenode_cache *fnode_cache;
	char *pname;

	fnode_cache = beehive_alloc(fnode_cache_allotter,
		PAF_KERNEL | __PAF_ZERO);
	if (!fnode_cache)
		return NULL;

	if (name->len > FNAME_INCACHE_LEN) {
		pname = kmalloc(name->len + 1, PAF_KERNEL);
		if (!pname) {
			beehive_free(fnode_cache_allotter, fnode_cache);
			return NULL;
		}
		fnode_cache->flags |= FNODECACHE_DYNAME;
	} else
		pname = fnode_cache->embed_name;
	memcpy(pname, name->name, name->len);
	pname[name->len] = 0;

	fnode_cache->file_name.name = pname;
	fnode_cache->file_name.len = name->len;
	fnode_cache->file_name.hash = name->hash;

	accurate_set(&fnode_cache->ref_count, 1);
	smp_lock_init(&fnode_cache->lock);
	hash_list_init_node(&fnode_cache->hash_node);
	list_init(&fnode_cache->children);
	list_init(&fnode_cache->child);

	if (parent) {
		fnode_cache->parent = hold_dirnode_cache(parent);
		smp_lock(&filenode_cache_lock);
		list_insert_front(&fnode_cache->child, &parent->children);
		smp_unlock(&filenode_cache_lock);
	} else
		fnode_cache->parent = fnode_cache;

	return fnode_cache;
}

/**
 * 为文件系统根节点分配缓存描述符
 */
struct filenode_cache *
fnode_cache_alloc_root(struct file_node *root_node)
{
	struct filenode_cache *ret = NULL;

	if (root_node) {
		static const struct file_name name = { .name = "/", .len = 1 };

		ret = fnode_cache_alloc(NULL, &name);
		if (ret)
			ret->file_node = root_node;
	}

	return ret;
}

/**
 * 释放文件节点缓存描述符
 */
static void fnode_cache_free(struct filenode_cache *fnode_cache)
{
	/**
	 * 如果文件系统需要额外的释放相关资源
	 * 则调用其回调函数
	 */
	if (fnode_cache->ops && fnode_cache->ops->d_release)
		fnode_cache->ops->d_release(fnode_cache);

	/**
	 * 释放名称字符串和对象描述符
	 */
 	if (fnode_cache->flags & FNODECACHE_DYNAME)
		kfree(fnode_cache->file_name.name);
	beehive_free(fnode_cache_allotter, fnode_cache); 
}

void loosen_fnode_look(struct filenode_lookup_s *look)
{
	loosen_filenode_cache(look->filenode_cache);
	loosen_mount(look->mnt);
}

/**
 * 将文件节点与其缓存节点绑在一起
 */
void fnode_cache_stick(struct filenode_cache *fnode_cache, struct file_node *fnode)
{
	smp_lock(&filenode_cache_lock);
	fnode_cache->file_node = fnode;
	smp_unlock(&filenode_cache_lock);
}

static struct hash_list_bucket *
hash(struct filenode_cache *parent, unsigned long hash)
{
	return __hash + hash_pair(parent, hash, __hash_order, __hash_mask);
}

static void __putin_fnode_cache(struct filenode_cache *fnode_cache, struct hash_list_bucket *list)
{
 	fnode_cache->flags |= FNODECACHE_INHASH;
 	hlist_add_head(&fnode_cache->hash_node, list);
}

void putin_fnode_cache(struct filenode_cache * fnode_cache)
{
	struct hash_list_bucket *list = hash(fnode_cache->parent,
				fnode_cache->file_name.hash);

	smp_lock(&filenode_cache_lock);
	smp_lock(&fnode_cache->lock);
	__putin_fnode_cache(fnode_cache, list);
	smp_unlock(&fnode_cache->lock);
	smp_unlock(&filenode_cache_lock);
}

static void __takeout_fnode_cache(struct filenode_cache *fnode_cache)
{
	if (fnode_cache->flags & FNODECACHE_INHASH) {
		fnode_cache->flags &= ~FNODECACHE_INHASH;
		hlist_del(&fnode_cache->hash_node);
	}
}

void takeout_fnode_cache(struct filenode_cache *filenode_cache)
{
	smp_lock(&filenode_cache_lock);
 	__takeout_fnode_cache(filenode_cache);
	smp_unlock(&filenode_cache_lock);
}

static struct filenode_cache *
__find_in_cache(struct filenode_cache *parent, struct file_name *name)
{
	unsigned int name_len = name->len;
	unsigned int name_hash = name->hash;
	const unsigned char *str = name->name;
	struct filenode_cache *ret = NULL;
	struct hash_list_bucket *bucket;
	struct hash_list_node *node;

	bucket = hash(parent, name_hash);
	lock_hash_bucket(bucket);
	
	hlist_for_each(node, bucket) {
		struct filenode_cache *fnode_cache; 
		struct file_name *file_name;

		fnode_cache = hlist_entry(node, struct filenode_cache, hash_node);

		if (fnode_cache->file_name.hash != name_hash)
			continue;
		if (fnode_cache->parent != parent)
			continue;

		smp_lock(&fnode_cache->lock);

		/**
		 * 获取锁以后重新检查节点父目录
		 * 防止在获得锁的时候被移到其他目录
		 */
		if (fnode_cache->parent != parent)
			goto conti;

		file_name = &fnode_cache->file_name;
		/**
		 * 如果文件系统有自定义的比较方法
		 */
		if (parent->ops && parent->ops->d_compare) {
			/**
			 * 文件系统回调返回结果表明两个文件不相等
			 */
			if (parent->ops->d_compare(parent, file_name, name))
				goto conti;
		} else {
			/**
			 * 默认的文件名比较，大小写敏感
			 */
			if (file_name->len != name_len)
				goto conti;
			if (memcmp(file_name->name, str, name_len))
				goto conti;
		}

		/**
		 * 成功找到文件，递增引用计数后退出搜索过程
		 */
		if (fnode_cache->flags & FNODECACHE_INHASH) {
			accurate_inc(&fnode_cache->ref_count);
			ret = fnode_cache;
		}
		smp_unlock(&fnode_cache->lock);
		break;
conti:
		smp_unlock(&fnode_cache->lock);
 	}

	unlock_hash_bucket(bucket);

 	return ret;
}

/**
 * 在哈希表中查找目录中的文件。
 * rename_lock防止与重命名操作的并发访问。
 */
static struct filenode_cache *
find_in_cache(struct filenode_cache *parent, struct file_name *name)
{
	struct filenode_cache * fnode_cache = NULL;
	unsigned long seq;

	do {
		seq = smp_seq_read_begin(&rename_lock);
		fnode_cache = __find_in_cache(parent, name);
		if (fnode_cache)
			break;
	} while (smp_seq_read_retry(&rename_lock, seq));

	return fnode_cache;
}

struct filenode_cache *hold_filenode_cache(struct filenode_cache *fnode_cache)
{
	accurate_inc(&fnode_cache->ref_count);

	return fnode_cache;
}

/**
 * 释放对缓存节点的引用
 */
void loosen_filenode_cache(struct filenode_cache *fnode_cache)
{
	struct filenode_cache *parent;

	if (!fnode_cache)
		return;

repeat:
	smp_lock(&filenode_cache_lock);
	if (!accurate_dec_and_test_zero(&fnode_cache->ref_count)) {
		smp_unlock(&filenode_cache_lock);
		return;
	}

	smp_lock(&fnode_cache->lock);

	if (fnode_cache->ops && fnode_cache->ops->may_delete) {
		if (fnode_cache->ops->may_delete(fnode_cache)) {
			__takeout_fnode_cache(fnode_cache);
			goto kill_it;
		}
	}

	/**
	 * 此时不应当在哈希表里面，一定时发生什么逻辑错误了
	 */
 	if (WARN_ONCE(fnode_cache->flags & FNODECACHE_INHASH,
			"file node still in cache")) {
	 	smp_unlock(&fnode_cache->lock);
		smp_unlock(&filenode_cache_lock);
		return;
 	}

kill_it:
	list_del(&fnode_cache->child);
	/**
	 * 释放文件节点的引用计数
	 */
	if (fnode_cache->file_node) {
		struct file_node *file_node = fnode_cache->file_node;

		fnode_cache->file_node = NULL;
		smp_unlock(&fnode_cache->lock);
		smp_unlock(&filenode_cache_lock);

		if (fnode_cache->ops && fnode_cache->ops->loosen_file_node)
			fnode_cache->ops->loosen_file_node(fnode_cache, file_node);
		else
			loosen_file_node(file_node);
	} else {
		smp_unlock(&fnode_cache->lock);
		smp_unlock(&filenode_cache_lock);
	}

	parent = fnode_cache->parent;
	fnode_cache_free(fnode_cache);
	/**
	 * 根节点
	 */
	if (fnode_cache == parent)
		return;
	/**
	 * 不是根结点，减少对上级目录的引用
	 */
	fnode_cache = parent;
	/**
	 * 循环而不是**递归**
	 */
	goto repeat;
}

static void swap_name(struct filenode_cache *old, struct filenode_cache *new)
{
	if (new->flags & FNODECACHE_DYNAME) {
		if (old->flags & FNODECACHE_DYNAME)
			swap(new->file_name.name, old->file_name.name);
		else {
			old->flags |= FNODECACHE_DYNAME;
			old->file_name.name = new->file_name.name;
			new->flags &= (~FNODECACHE_DYNAME);
			new->file_name.name = new->embed_name;
		}
	} else {
		memcpy(old->embed_name, new->embed_name,
				new->file_name.len + 1);
		if (old->flags & FNODECACHE_DYNAME) {
			new->flags |= FNODECACHE_DYNAME;
			new->file_name.name = old->file_name.name;
			old->flags &= (~FNODECACHE_DYNAME);
			old->file_name.name = old->embed_name;
		}
	}
}


/**
 * 当更名文件时，同时修改缓存中的数据
 */
void rename_fnode_cache(struct filenode_cache *old, struct filenode_cache *new)
{
	struct hash_list_bucket *list;

	if (WARN_ONCE(!old->file_node, "VFS: moving negative dcache entry\n"))
		return;

	smp_lock(&filenode_cache_lock);
	smp_seq_write_lock(&rename_lock);

	if (new < old) {
		smp_lock(&new->lock);
		smp_lock(&old->lock);
	} else {
		smp_lock(&old->lock);
		smp_lock(&new->lock);
	}

	/**
	 * 将节点从旧哈希桶里取出
	 */
	__takeout_fnode_cache(old);
	__takeout_fnode_cache(new);

	list_del(&old->child);
	list_del(&new->child);

	swap_name(old, new);
	swap(old->file_name.len, new->file_name.len);
	swap(old->file_name.hash, new->file_name.hash);

	if (IS_ROOT(old)) {
		old->parent = new->parent;
		new->parent = new;
		list_init(&new->child);
	} else {
		swap(old->parent, new->parent);
		/**
		 * 将旧节点插到原来的父目录链表中
		 */
		list_insert_front(&new->child, &new->parent->children);
	}

	/**
	 * 旧节点的名称已经改变
	 * 重新入哈希表
	 */
	list = hash(old->parent, old->file_name.hash);
	__putin_fnode_cache(old, list);
	list_insert_front(&old->child, &old->parent->children);
	/**
	 * 释放节点的锁，实际上这里的顺序并不重要
	 */
	if (new < old) {
		smp_unlock(&old->lock);
		smp_unlock(&new->lock);
	} else {
		smp_unlock(&new->lock);
		smp_unlock(&old->lock);
	}
	smp_seq_write_unlock(&rename_lock);
	smp_unlock(&filenode_cache_lock);
}

/**
 * 当删除物理文件时，释放缓存描述符的引用
 * 并将其从哈希表中删除
 */
void remove_fnode_cache(struct filenode_cache *fnode_cache)
{
	/**
	 * 获取全局锁及缓存节点锁
	 */
	smp_lock(&filenode_cache_lock);
	smp_lock(&fnode_cache->lock);
	/**
	 * 最后一个使用者了
	 */
	if (accurate_read(&fnode_cache->ref_count) == 1) {
		if (fnode_cache->file_node) {
			struct file_node *file_node = fnode_cache->file_node;

			/**
			 * 解除缓存节点与文件节点之间的引用
			 */
			fnode_cache->file_node = NULL;
			smp_unlock(&fnode_cache->lock);
			smp_unlock(&filenode_cache_lock);

			/**
			 * 如果文件系统自定义了删除节点引用的方法
			 * 就调用文件系统的方法
			 * 否则用默认的方法释放引用
			 */
			if (fnode_cache->ops && fnode_cache->ops->loosen_file_node)
				fnode_cache->ops->loosen_file_node(fnode_cache, file_node);
			else
				loosen_file_node(file_node);
		} else {
			smp_unlock(&fnode_cache->lock);
			smp_unlock(&filenode_cache_lock);
		}

		return;
	}

	/**
	 * 从哈希表中移除缓存节点
	 */
	__takeout_fnode_cache(fnode_cache);

	smp_unlock(&fnode_cache->lock);
	smp_unlock(&filenode_cache_lock);
}

/**
 * 在卸载特殊文件系统时，删除所有的缓存节点
 */
void genocide_fnode_cache(struct filenode_cache *root)
{
	struct filenode_cache *this_parent = root;
	struct double_list *next;

	smp_lock(&filenode_cache_lock);
repeat:
	next = this_parent->children.next;
resume:
	while (next != &this_parent->children) {
		struct double_list *tmp = next;
		struct filenode_cache *fnode_cache =
			list_container(tmp, struct filenode_cache, child);
		next = tmp->next;
		if (!(fnode_cache->flags & FNODECACHE_INHASH)
		    || !fnode_cache->file_node)
			continue;
		if (!list_is_empty(&fnode_cache->children)) {
			this_parent = fnode_cache;
			goto repeat;
		}
		accurate_dec(&fnode_cache->ref_count);
	}
	if (this_parent != root) {
		next = this_parent->child.next; 
		accurate_dec(&this_parent->ref_count);
		this_parent = this_parent->parent;
		goto resume;
	}
	smp_unlock(&filenode_cache_lock);
}

void shrink_dcache_parent(struct filenode_cache *parent)
{
	/* TO-DO */
}

void shrink_fnode_cache(struct super_block * sb)
{
	/* TO-DO */
}

/**
 * 在目录中查找指定的文件
 * 如果文件不存在，在缓存中创建一个缓存项
 * 调用者必须持有目录锁
 */
struct filenode_cache *
__find_file_indir_alloc(struct file_name *name, struct filenode_cache * base,
	struct filenode_lookup_s *look)
{
	struct filenode_cache * ret;
	struct file_node *file_node = base->file_node;
	int err;

	/**
	 * 如果文件系统有自己的哈希生成算法
	 */
	if (base->ops && base->ops->d_hash) {
		/**
		 * 调用文件系统的回调来生成哈希值
		 */
		err = base->ops->d_hash(base, name);
		ret = ERR_PTR(err);
		if (err < 0)
			goto out;
	}

	/**
	 * 先在节点缓存中搜索
	 */
	ret = find_in_cache(base, name);

	/**
	 * 缓存中不存在
	 */
	if (!ret) {
		struct filenode_cache *new = fnode_cache_alloc(base, name);

		/**
		 *  分配不到内存
		 */
		ret = ERR_PTR(-ENOMEM);
		if (!new)
			goto out;

		/**
		 * 调用文件系统的回调，在块设备中看看有没有
		 */
		ret = file_node->node_ops->lookup(file_node, new, look);
		/**
		 * 如果块设备上没有，则返回新创建的空对象
		 */
		if (!ret)
			ret = new;
		else
			loosen_filenode_cache(new);
	}

out:
	return ret;
}

/**
 * 得到与给定的父目录和文件名相关的目录项对象.
 */
static int find_file_indir(struct filenode_lookup_s *look, struct file_name *name,
		     struct mount_desc **ret_mnt, struct filenode_cache **ret_node)
{
	struct mount_desc *mnt = look->mnt;
	/**
	 * 先在缓存中搜索，看看文件是否存在
	 */
	struct filenode_cache *fnode_cache = __find_in_cache(look->filenode_cache, name);

	/**
	 * 在缓存中没有找到
	 * 必须在块设备上找一找了
	 */
	if (!fnode_cache) {
		struct file_node *dir = look->filenode_cache->file_node;

		/**
		 * 获取目录的锁
		 */
		down(&dir->sem);
		/**
		 * 调用文件系统的回调，从设备上装载文件节点
		 */
		fnode_cache = __find_file_indir_alloc(name, look->filenode_cache, look);
		up(&dir->sem);
		if (IS_ERR(fnode_cache))
			return PTR_ERR(fnode_cache);
	}

	*ret_mnt = mnt;
	*ret_node = fnode_cache;

	return 0;
}

/**
 * 设置查找起始点
 */
static int
set_beginning(const char *name, struct filenode_lookup_s *look)
{
	/**
	 * 获取fs的lock自旋锁。
	 */
	smp_read_lock(&current->fs_context->lock);
	/**
	 * 从根目录开始查找
	 */
	if (*name=='/') {
		look->mnt = hold_mount(current->fs_context->root_mount);
		look->filenode_cache = 
			hold_dirnode_cache(current->fs_context->root_fnode_cache);
	} else {
		/**
		 * 从当前目录开始查找。
		 */
		look->mnt = hold_mount(current->fs_context->curr_dir_mount);
		look->filenode_cache =
			hold_dirnode_cache(current->fs_context->curr_dir_fnode_cache);
	}
	smp_read_unlock(&current->fs_context->lock);

	return 0;
}

/**
 * 找到当前目录的最后一个挂载点
 * 并切换到该挂载点
 */
void advance_mount_point(struct mount_desc **mnt,
	struct filenode_cache **fnode_cache)
{
	/**
	 * 当前路径有挂载点
	 */
	while ((*fnode_cache)->mount_count) {
		/**
		 * 查找当前路径的挂载点
		 */
		struct mount_desc *child = lookup_mount(*mnt, *fnode_cache);

		/**
		 * 未挂载文件系统，退出
		 */
		if (!child)
			break;

		/**
		 * 释放对当前挂载点的引用
		 */
		loosen_mount(*mnt);
		loosen_filenode_cache(*fnode_cache);
		/**
		 * 转到下级挂载点
		 */
		*mnt = child;
		*fnode_cache = hold_dirnode_cache(child->sticker);
	}
}

/**
 * 将搜索点退回到上一级目录
 */
static void recede_parent(struct mount_desc **mnt, struct filenode_cache **fnode_cache)
{
	while(1) {
		struct mount_desc *parent;
		struct filenode_cache *prev = *fnode_cache;

		smp_read_lock(&current->fs_context->lock);
		/**
		 * 已经到达当前进程的根目录，退出 
		 */
		if (*fnode_cache == current->fs_context->root_fnode_cache &&
		    *mnt == current->fs_context->root_mount) {
			smp_read_unlock(&current->fs_context->lock);
			break;
		}
		smp_read_unlock(&current->fs_context->lock);

		smp_lock(&filenode_cache_lock);
		/**
		  * 当前目录还不是所在目录的根目录
		  */
		if (*fnode_cache != (*mnt)->sticker) {
			/**
			 * 返回到上一级目录
			 */
			*fnode_cache = hold_dirnode_cache((*fnode_cache)->parent);
			smp_unlock(&filenode_cache_lock);
			loosen_filenode_cache(prev);
			break;
		}
		smp_unlock(&filenode_cache_lock);

		/**
		 * 到达挂载点的根
		 */
		smp_lock(&mount_lock);
		/**
		 * 转到上一级挂载文件系统
		 */
		parent = (*mnt)->parent;
		/**
		 * 已经是顶级根目录了
		 */
		if (parent == *mnt) {
			smp_unlock(&mount_lock);
			break;
		}

		hold_mount(parent);
		/**
		 * 转到上级挂载点
		 */
		*fnode_cache = hold_dirnode_cache((*mnt)->mountpoint);
		smp_unlock(&mount_lock);
		/**
		 * 释放对当前挂载文件系统和文件节点的引用
		 */
		loosen_filenode_cache(prev);
		loosen_mount(*mnt);
		*mnt = parent;
	}

	/**
	 * 最后找到的目录可能是一个挂载点
	 * 找到该挂载点当前实际挂载的目录
	 */
	advance_mount_point(mnt, fnode_cache);
}

/**
 * 符号链接查找，不考虑嵌套计数
 */
int __advance_symlink(struct filenode_cache *fnode_cache, struct filenode_lookup_s *look)
{
	int err;

	/**
	 * 更新索引结点的访问时间。
	 */
	fnode_update_atime(fnode_cache->file_node);
	look->symlink_names[look->nested_count] = NULL;
	/**
	 * 调用文件系统相关的函数实现的follow_link方法。
	 * 它读取存放在符号链接索引结点中的路径名，并把这个路径名保存在nd->save_names数组的项中
	 */
	err = fnode_cache->file_node->node_ops->follow_link(fnode_cache, look);
	if (!err) {
		/* nd_get_link获得follow_link读取的字符串 */
		char *symlink = look->symlink_names[look->nested_count];

		if (!IS_ERR(symlink)) {
	 		/**
			 * 检查符号链接是否以'/'开头
			 */
			if (*symlink == '/') {
				/**
				 * 是以'/'开头，已经找到一个绝对路径了
				 * 因此没有必要保留前一个路径的任何信息。
				 * 一切从头开始。
				 */
				loosen_fnode_look(look);
				/**
				 * 设置搜索对象，以使它们指向当前进程的根目录
				 */
				set_beginning(symlink, look);
			}
			/**
			 * 执行通用的路径搜索过程
			 */
			err = __generic_load_filenode(symlink, look);
		} else {
			err = PTR_ERR(symlink);
			loosen_fnode_look(look);
		}

		/**
		 * 如果有loosen_link方法，就执行它
		 * 释放由follow_link方法分配的临时数据结构。
		 */
		if (fnode_cache->file_node->node_ops->loosen_link)
			fnode_cache->file_node->node_ops->loosen_link(fnode_cache, look);
	}

	return err;
}

/**
 * 查找符号链接
 * 	filenode_cache:原文件
 *	look:存放查找结果
 */
static int advance_symlink(struct filenode_cache *fnode_cache, struct filenode_lookup_s *look)
{
	int err = -ELOOP;

	/**
	 * 由于符号链接可能嵌套
	 * 如果不判断一下，可能在此形成递归调用、死循环。
	 */
	if (current->fs_search.nested_count >= MAX_NESTED_LINKS)
		goto loosen;
	if (current->fs_search.link_count >= MAX_LINK_COUNT)
		goto loosen;

	/**
	 * 计数，分别代表嵌套调用次数及符号链接总次数
	 */
	current->fs_search.nested_count++;
	current->fs_search.link_count++;
	look->nested_count++;
	err = __advance_symlink(fnode_cache, look);
	current->fs_search.nested_count--;
	/**
	 * 本次查找的递归次数。
	 */
	look->nested_count--;

	return err;

loosen:
	loosen_fnode_look(look);
	return err;
}

/**
 * 真正的路径查找实现。
 */
static int __generic_load_filenode(char *dir_name, struct filenode_lookup_s *look)
{
	struct mount_desc *next_mnt;
	struct filenode_cache *next_node;
	struct file_node *file_node;
	int err;
	unsigned int ch;

	/**
	 * 查找标志，如FNODE_LOOKUP_READLINK
	 */
	unsigned int lookup_flags = look->flags;

	/**
	 * 符号链接查找。
	 */
	if (look->nested_count)
		lookup_flags = FNODE_LOOKUP_READLINK;

	/**
	 * 在最前面的多个/仅代表一个/ 
	 */
	while (*dir_name == '/')
		dir_name++;
	/**
	 * 目标目录是根目录或者空目录，直接返回
	 */
	if (!*dir_name){
		look->path_type = PATHTYPE_ROOT;
		return 0;
	}

	/* 每次在inode所在的目录下进行查找 */
	file_node = look->filenode_cache->file_node;

	/**
	 * 循环处理每一个路径分量。
	 */
	while (1) {
		ch = *dir_name;
		look->cur.name = dir_name;
		/**
		 * 计算散列值
		 */
		look->cur.hash = 0;
		do {
			hash_append(ch, &look->cur.hash);
			dir_name++;
			ch = *dir_name;
		} while (ch && (ch != '/'));
		look->cur.len = dir_name - (const char *)look->cur.name;
		/**
		 * 如果文件系统有自定义的散列函数
		 * 则计算它的散列值，并存放在文件名描述符中
		 */
		if (look->filenode_cache->ops && look->filenode_cache->ops->d_hash) {
			err = look->filenode_cache->ops->d_hash(look->filenode_cache, &look->cur);
			if (err < 0)
				goto loosen_look;
		}

		/**
		 * 最后一个分量，并且没有以"/"结束，是一个文件。
		 */
		if (!ch)
			goto normal;

		/**
		 * 如果分量以"/"结束，退出。
		 */
		do {
			dir_name++;
		} while (*dir_name == '/');
		if (!*dir_name)
			goto end_with_slashes;

		/**
		 * 当前路径以"."开头
		 */
		if ((look->cur.name[0] == '.') && (look->cur.len <= 2)) {
			if (look->cur.len == 1)
				continue;

			if (look->cur.name[1] == '.') {
				/* 转到上级目录并继续。 */				
				recede_parent(&look->mnt, &look->filenode_cache);
				file_node = look->filenode_cache->file_node;
				continue;
			}
		}

		/**
		 * 在当前目录中搜索下一个分量。
		 */
		err = find_file_indir(look, &look->cur, &next_mnt, &next_node);
		if (err)
			goto loosen_look;

		/**
		 * 向前推进到当前目录最后一个安装点
		 */
		advance_mount_point(&next_mnt, &next_node);

		err = -ENOENT;
		file_node = next_node->file_node;
		/**
		 * 目录下不存在请求的文件
		 */
		if (!file_node)
			goto loosen_next;

		/**
		 * 不是目录也不是符号链接，但是后面还跟着/
		 */
		err = -ENOTDIR; 
		if (!file_node->node_ops)
			goto loosen_next;

		/**
		 * 这是一个符号链接文件
		 */
		if (file_node->node_ops->follow_link) {
			/**
			 * 处理符号链接。
			 */
			err = advance_symlink(next_node, look);
			loosen_filenode_cache(next_node);
			if (err)
				return err;

			err = -ENOENT;
			file_node = look->filenode_cache->file_node;
			if (!file_node)
				goto loosen_look;

			err = -ENOTDIR; 
			if (!file_node->node_ops || !file_node->node_ops->lookup)
				goto loosen_look;
		/**
		 * 常规目录
		 */
		} else if (file_node->node_ops->lookup) {
			loosen_filenode_cache(look->filenode_cache);
			look->mnt = next_mnt;
			look->filenode_cache = next_node;
		} else {			
			err = -ENOTDIR; 
			goto loosen_look;
		}
	}
/**
 * 处理路径中最后一个文件名
 */
end_with_slashes:
	/**
	 * 文件名最后一个字符是"/"
	 * 因此必须解析符号链接，并要求最终指向目录
	 */
	lookup_flags |= FNODE_LOOKUP_READLINK | FNODE_LOOKUP_DIRECTORY;
normal:
	/**
	 * 不解析最后一个文件名
	 */
	if (lookup_flags & FNODE_LOOKUP_NOLAST) {
		/**
		 * 将last设置为最后一个分量
		 */
		look->last = look->cur;
		/**
		 * 如果最后一个分量不是"."或者".."，那么最后一个分量默认就是LAST_NORM
		 */
		look->path_type = PATHTYPE_NORMAL;
		if (look->cur.name[0] != '.')
			return 0;

		/**
		 * 对"."和".."进行特殊处理
		 */
		if (look->last.len == 1)
			look->path_type = PATHTYPE_DOT;
		else if (look->last.len == 2 && look->last.name[1] == '.')
			look->path_type = PATHTYPE_DOTDOT;

		return 0;
	}

	if ((look->cur.name[0] == '.') && (look->cur.len <= 2)) {
		if (look->cur.len == 1)
			return 0;
		/**
		 * 不是".."，应当是正常的文件名
		 */
		if (look->cur.name[1] == '.') {
			/**
			 * ".."，尝试回到父目录
			 */
			recede_parent(&look->mnt, &look->filenode_cache);
			file_node = look->filenode_cache->file_node;
			return 0;
		}
	}

	/**
	 * 在缓存或者磁盘上搜索文件。
	 */
	err = find_file_indir(look, &look->cur, &next_mnt, &next_node);
	if (err)
		goto loosen_look;

	advance_mount_point(&next_mnt, &next_node);
	file_node = next_node->file_node;
	/**
	 * 需要检查符号链接
	 * 并且真的是一个符号链接
	 */
	if ((lookup_flags & FNODE_LOOKUP_READLINK)
	    && file_node && file_node->node_ops && file_node->node_ops->follow_link) {
		err = advance_symlink(next_node, look);
		loosen_filenode_cache(next_node);
		if (err)
			return err;
		file_node = look->filenode_cache->file_node;
	/**
	 * 不需要符号链接查找
	 */
	} else {
		/**
		 * 释放dentry的引用，因为要将最后查找的分量设置为返回结果的dentry
		 */
		loosen_filenode_cache(look->filenode_cache);
		look->mnt = next_mnt;
		look->filenode_cache = next_node;
	}
	/**
	 * 检查文件是否真的存在
	 */
	err = -ENOENT;
	if (!file_node)
		goto loosen_look;
	/**
	 * 要求最后一个文件必须是目录
	 * 例如cd进入目录的情况，或者最后一个字符是/
	 */
	if (lookup_flags & FNODE_LOOKUP_DIRECTORY) {
		err = -ENOTDIR; 
		/**
		 * 没有lookup说明这不是一个目录，返回ENOTDIR错误。
		 */
		if (!file_node->node_ops || !file_node->node_ops->lookup)
			goto loosen_look;
	}

	return 0;

loosen_next:
	loosen_filenode_cache(next_node);
loosen_look:
	loosen_fnode_look(look);
	return err;
}

/**
 * 查找路径名
 *	name:要查找的文件路径名
 *	flags:表示怎样访问查找的文件标志，如FNODE_LOOKUP_READLINK
 *	look:存放查找的结果。
 */
int fastcall load_filenode_cache(char *dir_name, unsigned int flags,
	struct filenode_lookup_s *look)
{
	int ret;

	/**
	 * 在结果中设置初值。
	 */
	look->path_type = PATHTYPE_NOTHING;
	look->flags = flags;
	look->nested_count = 0;
	/**
	 * 当前进程的符号链接查找计数进行初始化。
	 */
	current->fs_search.link_count = 0;
	current->fs_search.nested_count = 0;

	/**
	 * 设置开始搜索的初始目录
	 */
	set_beginning(dir_name, look);

	/**
	 * __generic_load_filenode执行真正的路径查找。
	 */
	ret = __generic_load_filenode(dir_name, look);

	return ret;
}

/**
 * 加载文件节点的数据到缓存中
 * 其中文件名称是应用程序传入
 */
int fastcall load_filenode_user(char __user *dir_name, unsigned flags,
		struct filenode_lookup_s *look)
{
	char *tmp;
	int err = 0;

	clone_user_string(dir_name, &tmp);
	if (!IS_ERR(tmp)) {
		err = load_filenode_cache(tmp, flags, look);
		discard_user_string(tmp);
	}

	return err;
}

void __init init_filenode_cache_early(void)
{
	int i;

	__hash = alloc_boot_mem_stretch(sizeof(struct hash_list_bucket),
					10, &__hash_order);
	__hash_mask = (1 << __hash_order) - 1;

	for (i = 0; i < (1 << __hash_order); i++)
		hash_list_init_bucket(&__hash[i]);
}

void __init init_filenode_cache(void)
{
	fnode_cache_allotter = beehive_create("dentry_cache",
					 sizeof(struct filenode_cache),
					 0,
					 BEEHIVE_RECLAIM_ABLE | BEEHIVE_PANIC,
					 NULL);
}
