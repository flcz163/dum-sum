#include <dim-sum/beehive.h>
#include <dim-sum/bitops.h>
#include <dim-sum/pagemap.h>
#include <dim-sum/err.h>
#include <dim-sum/ctype.h>
#include <dim-sum/disk.h>
#include <dim-sum/fs.h>

#include "msdos.h"
#include "scan.h"

static scan_partition_f scan_func[] = {
	scan_msdos_partitions,
};

void delete_partition(struct disk_device *disk, unsigned int part_idx)
{
	struct partition_desc *p;

	if (part_idx >= disk->max_partition_count)
		return;

	p = &disk->part[part_idx];

	devfs_remove("%s/%s", disk->sysobj_name, p->sysobj_name);
	takeout_blkdev_container(&p->device);
	unregister_device(&p->device);
	memset(p, 0, sizeof(*p));
	__set_bit(__PART_STATE_UNUSED, &p->state);
	disk->partition_count--;
}

void add_partition(struct disk_device *disk, int part_idx,
			sector_t start, sector_t len)
{
	struct partition_desc *p;

	if (part_idx >= disk->max_partition_count)
		return;

	if (!len)
		return;

	p = &disk->part[part_idx];

	__clear_bit(__PART_STATE_UNUSED, &p->state);
	p->start_sector = start;
	p->sector_count = len;
	snprintf(p->sysobj_name, sizeof(p->sysobj_name), "part%d", part_idx + 1); 

	devfs_mk_blkdev(MKDEV(disk->dev_major, part_idx + 1),
			S_IFBLK|S_IRUSR|S_IWUSR,
			"%s/%s", disk->sysobj_name, p->sysobj_name);

	p->device.dev_num = MKDEV(disk->dev_major, part_idx + 1);
	putin_blkdev_container(&p->device, disk);
	register_device(&p->device);

	disk->partition_count++;
}

static struct device_partitions *
scan_partitions(struct disk_device *disk, struct block_device *bdev)
{
	struct device_partitions *scan_result;
	int i, res;

	scan_result = kmalloc(sizeof(struct device_partitions),
					PAF_KERNEL | __PAF_ZERO);
	if (!scan_result)
		return NULL;

	res = 0;
	for (i = 0; i < ARRAY_SIZE(scan_func); i++) {
		res = scan_func[i](scan_result, bdev);
		if (res)
			break;
	}
	if (res > 0)
		return scan_result;

	WARN_ON(!res, " unknown partition table\n");
	kfree(scan_result);

	return NULL;
}

/* 扫描磁盘分区 */
int rescan_partitions(struct disk_device *disk, struct block_device *bdev)
{
	struct device_partitions *scan_result;
	int p, res;

	/**
	 * 分区打开计数次数大于0
	 * 表示有分区在使用
	 */
	if (bdev->open_partitions)
		return -EBUSY;

	/**
	 * 将所有磁盘块写入到磁盘中
	 * 并将内存中的文件节点失败
	 */
	res = invalidate_partition_sync(disk, 0);
	if (res)
		return res;

	/**
	 * 删除内存中的分区信息
	 */
	for (p = 0; p < disk->partition_count; p++)
		delete_partition(disk, p);

	/* 如果磁盘已经被移除，或者没有分区表，则退出 */
	scan_result = scan_partitions(disk, bdev);
	if (!scan_result)
		return 0;

	/* 将检测到的分区添加到系统中 */
	for (p = 0; p < scan_result->partition_count; p++) {
		struct partition_info *partition = &scan_result->parts[p];

		/* 将分区添加到系统中 */
		add_partition(disk, p, partition->start_sector,
				partition->sector_count);
	}
	kfree(scan_result);

	bdev->partition_uptodate = 1;

	return 0;
}
