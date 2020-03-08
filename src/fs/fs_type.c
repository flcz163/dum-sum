#include <dim-sum/fs.h>
#include <dim-sum/smp_rwlock.h>

/**
 * 保护file_systems的读写锁。
 */
static struct smp_rwlock fs_lock =
		SMP_RWLOCK_UNLOCKED(fs_lock);
/**
 * 文件系统类型链表头指针。
 */
static struct double_list all_fs = LIST_HEAD_INITIALIZER(all_fs);

void loosen_file_system(struct file_system_type *fs)
{
}

/**
 * 查找指定类型的文件系统。
 */
static struct file_system_type *__find_filesystem(const char *name)
{
	struct double_list *list;
	struct file_system_type *p = NULL;

	list_for_each(list, &all_fs) {
		p = list_container(list, struct file_system_type, list);
		if (strcmp(p->name,name) == 0)
			return p;
	}

	return NULL;
}

struct file_system_type *lookup_file_system(const char *name)
{
	struct file_system_type *fs;

	smp_read_lock(&fs_lock);
	fs = __find_filesystem(name);
	smp_read_unlock(&fs_lock);

	return fs;
}

/**
 * 注册文件系统，将相应的file_system_type加入到链表中。
 */
int register_filesystem(struct file_system_type * fs)
{
	struct file_system_type *p;
	int ret = 0;

	if (!fs)
		return -EINVAL;

	list_init(&fs->superblocks);
	list_init(&fs->list);
	smp_write_lock(&fs_lock);
	p = __find_filesystem(fs->name);
	if (p)
		ret = -EBUSY;
	else
		list_insert_front(&fs->list, &all_fs);
	smp_write_unlock(&fs_lock);

	return ret;
}
