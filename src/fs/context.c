#include <dim-sum/fnode_cache.h>
#include <dim-sum/fs_context.h>
#include <dim-sum/mount.h>

void set_fs_root(struct task_fs_context *fs, struct mount_desc *mnt,
		 struct filenode_cache *fnode_cache)
{
	struct filenode_cache *old_node;
	struct mount_desc *old_mount;

	smp_write_lock(&fs->lock);
	old_mount = fs->root_mount;
	old_node = fs->root_fnode_cache;
	fs->root_mount = hold_mount(mnt);
	fs->root_fnode_cache = hold_dirnode_cache(fnode_cache);
	smp_write_unlock(&fs->lock);

	if (old_node) {
		loosen_filenode_cache(old_node);
		loosen_mount(old_mount);
	}
}

void set_fs_pwd(struct task_fs_context *fs, struct mount_desc *mnt,
		struct filenode_cache *fnode_cache)
{
	struct filenode_cache *old_node;
	struct mount_desc *old_mount;

	smp_write_lock(&fs->lock);
	old_mount = fs->curr_dir_mount;
	old_node = fs->curr_dir_fnode_cache;
	fs->curr_dir_mount = hold_mount(mnt);
	fs->curr_dir_fnode_cache = hold_dirnode_cache(fnode_cache);
	smp_write_unlock(&fs->lock);

	if (old_node) {
		loosen_filenode_cache(old_node);
		loosen_mount(old_mount);
	}
}
