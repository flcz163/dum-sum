#include <dim-sum/beehive.h>
#include <dim-sum/boot_allotter.h>
#include <dim-sum/err.h>
#include <dim-sum/fnode.h>
#include <dim-sum/fs.h>
#include <dim-sum/fs_context.h>
#include <dim-sum/init.h>
#include <dim-sum/iosched.h>
#include <dim-sum/mount.h>
#include <dim-sum/sched.h>
#include <dim-sum/syscall.h>

/**
 * 用于分配file结构的内存分配器。
 */
struct beehive_allotter *file_allotter;

struct timespec current_fs_time(struct super_block *sb)
{
	struct timespec now = current_kernel_time();
	return timespec_trunc(now, sb->gran_ns);
}

static void __init mount_block_root(int flags)
{
	struct mount_desc *mnt;

	/**
	 * 加载文件系统超级块
	 */
	mnt = do_internal_mount("lext3", flags, "/dev/vda/part1", NULL);
	if (!IS_ERR(mnt))
		goto out;

	panic("VFS: Unable to mount root fs.\n");

out:
	set_fs_root(current->fs_context, mnt, mnt->sticker);
	set_fs_pwd(current->fs_context, mnt, mnt->sticker);

	mount_devfs_fs();

	sys_mkdir("/tmp", 0);
	do_mount(NULL, "/tmp", "rootfs", 0, NULL);
}

void __init mount_file_systems(void)
{
	mount_devfs_fs();
	mount_block_root(0);
}

void __init init_vfs_early(void)
{
	init_chrdev_early();
	init_disk_early();
	init_file_node_early();
	init_filenode_cache_early();
}

void __init init_vfs(void)
{
	file_allotter = beehive_create("filp", sizeof(struct file), 0,
			BEEHIVE_HWCACHE_ALIGN |BEEHIVE_PANIC, NULL);
	init_iosched();
	pdflush_init();
	init_file_node();
	init_filenode_cache();
	mnt_init();
	init_buffer_module();
	init_block_layer();
	init_blkdev();
}
