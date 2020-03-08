#include <dim-sum/beehive.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/ctype.h>
#include <dim-sum/errno.h>
#include <dim-sum/disk.h>
#include <dim-sum/idr.h>
#include <dim-sum/string.h>

static struct idr_space
cdrom_numspace = NUMSPACE_INITIALIZER(cdrom_numspace);
static struct idr_space
disk_numspace = NUMSPACE_INITIALIZER(disk_numspace);
static struct idr_space
partition_numspace = NUMSPACE_INITIALIZER(partition_numspace);

char *format_disk_name(struct disk_device *hd, char *buf)
{
	snprintf(buf, BDEVNAME_SIZE, "%s", hd->name);

	return buf;
}

char *format_block_devname(struct block_device *bdev, char *buf)
{
	int part = MINOR(bdev->devno);

	if (!part)
		return format_disk_name(bdev->disk, buf);

	snprintf(buf, BDEVNAME_SIZE, "%s-%d", bdev->disk->name, part);

	return buf;
}

/**
 * 将磁盘注册到sysfs文件系统，并扫描其分区
 */
void devfs_add_disk(struct disk_device *disk)
{
	char dir_name[SYSOBJ_NAME_LEN], link_name[32];

	/**
	 * 没有逻辑分区，将设备添加到dev文件系统中
	 */
	if (disk->logic_devices == 1) {
		if (disk->sysobj_name[0] != '\0') {
			devfs_mk_blkdev(MKDEV(disk->dev_major, 0),
					(disk->flags & DISK_FLAG_CD) ?
						S_IFBLK|S_IRUGO|S_IWUGO :
						S_IFBLK|S_IRUSR|S_IWUSR,
					"%s", disk->sysobj_name);

			/**
			 * 创建CD软链接
			 */
			if (disk->flags & DISK_FLAG_CD) {
				disk->device_sequence = get_idr_number(&cdrom_numspace);
				sprintf(link_name, "cd-rom/cd%d", disk->device_sequence);
				sprintf(dir_name, "../%s", disk->sysobj_name);
				devfs_mk_symlink(link_name, dir_name);
			/**
			 * 创建硬盘软链接
			 */
			} else {
				disk->device_sequence = get_idr_number(&disk_numspace);
				sprintf(link_name, "disk/disk%d", disk->device_sequence);
				sprintf(dir_name, "../%s", disk->sysobj_name);
				devfs_mk_symlink(link_name, dir_name);
			}
		}

		return;
	}

	/* 处理磁盘分区 */
	devfs_mk_dir(disk->sysobj_name);
	devfs_mk_blkdev(MKDEV(disk->dev_major, 0),
			S_IFBLK|S_IRUSR|S_IWUSR,
			"%s/disk", disk->sysobj_name);

	disk->device_sequence = get_idr_number(&partition_numspace);

	sprintf(link_name, "discs/disc%d", disk->device_sequence);
	sprintf(dir_name, "../%s", disk->sysobj_name);
	devfs_mk_symlink(link_name, dir_name);
}

void devfs_remove_disk(struct disk_device *disk)
{
	/* TO-DO */
}