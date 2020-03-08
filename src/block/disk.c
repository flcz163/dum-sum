#include <dim-sum/beehive.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/disk.h>
#include <dim-sum/fs.h>

/**
 * kobj_map建立设备驱动程序和设备的主设备号（及相关范围内的次设备号）之间的联接。
 */
static struct device_container blk_container;

/**
 * 分配并初始化一个新的磁盘对象
 */
struct disk_device *alloc_disk(int max_partition_count)
{
	/* 分配磁盘描述符空间 */
	struct disk_device *disk;
	int size = max_partition_count * sizeof(struct partition_desc)
		+ sizeof(struct disk_device);

	disk = kmalloc(size, PAF_KERNEL | __PAF_ZERO);
	if (disk) {
		disk->max_partition_count = max_partition_count;
		disk->logic_devices = max_partition_count + 1;
	}

	return disk;
}

/**
 * 增加磁盘设备的引用计数。
 */
struct disk_device *hold_disk(struct disk_device *disk)
{
	if (!disk || !disk->fops)
		return NULL;

	hold_device(&disk->device);

	return disk;
}

void loosen_disk(struct disk_device *disk)
{
	if (disk)
		loosen_device(&disk->device);
}

int putin_blkdev_container(struct device *device, void *owner)
{
	return putin_device_container(&blk_container, device, owner);
}

void takeout_blkdev_container(struct device *device)
{
	takeout_device_container(&blk_container, device);
}

#define device_to_disk(obj) (struct disk_device *)(obj->owner)
/**
 * 根据设备号，返回磁盘及其分区序号
 * 0表示非分区
 */
struct disk_device *blkdev_container_lookup(devno_t dev, int *part)
{
	struct device *device = device_container_lookup(&blk_container, dev, part);

	return device ? device_to_disk(device) : NULL;
}

/**
 * 注册并激活磁盘
 */
void add_disk(struct disk_device *disk)
{
	struct block_device *bdev;

	/**
	 * 设置DISK_FLAG_REGISTERED标志。
	 */
	disk->flags |= DISK_FLAG_REGISTERED;
	disk->dev_major = alloc_dev_major();
	disk->device.dev_num = MKDEV(disk->dev_major, 0);
	putin_device_container(&blk_container, &disk->device, disk);

	/**
	 * 磁盘设备注册到设备文件系统中，这样用户就能从/dev/目录中访问到磁盘设备了。
	 */
	devfs_add_disk(disk);

	/* 获取设备的引用计数 */
	bdev = find_hold_block_device(disk->dev_major, 0);
	if (!bdev)
		return;

	/**
	 * 磁盘分区不是最新的
	 * 强制扫描分区
	 */
	bdev->partition_uptodate = 0;

	/**
	 * 扫描磁盘分区，建立磁盘与分区的关系
	 * 并将分区添加到系统中
	 */
	if (open_block_device(bdev, FMODE_READ, 0)) {
		loosen_block_device(bdev);
		return;
	}

	close_block_device(bdev);
}

/**
 * 卸载磁盘。
 */
void delete_disk(struct disk_device *disk)
{
	int p;

	for (p = disk->logic_devices - 1; p > 0; p--) {
		invalidate_partition_sync(disk, p);
		delete_partition(disk, p);
	}

	invalidate_partition_sync(disk, 0);
	disk->capacity = 0;
	disk->flags &= ~DISK_FLAG_REGISTERED;
	blk_loosen_queue(disk->queue);

	devfs_remove_disk(disk);
}

void set_disk_readonly(struct disk_device *disk, int flag)
{
	int i;

	disk->read_only = flag;
	for (i = 0; i < disk->partition_count - 1; i++)
		disk->part[i].read_only = flag;
}

/**
 * 将所有磁盘块写入到磁盘中
 * 并将内存中的文件节点失效
 */
int invalidate_partition_sync(struct disk_device *disk, int index)
{
	int res = 0;
	struct block_device *bdev;

	bdev = find_hold_block_device(disk->dev_major, index);

	if (bdev) {
		res = invalidate_fnode_device(bdev, 1);
		loosen_block_device(bdev);
	}

	return res;
}

int __init init_disk_early(void)
{
	device_container_init(&blk_container);

	return 0;
}
