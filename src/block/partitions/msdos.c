#include <dim-sum/errno.h>

#include "msdos.h"
#include "scan.h"

#include <asm/unaligned.h>

/**
 * 检查分区魔法数
 */
#define MSDOS_MAGIC1	0x55
#define MSDOS_MAGIC2	0xAA
static inline int check_magic(unsigned char *p)
{
	return (p[510] == MSDOS_MAGIC1 && p[511] == MSDOS_MAGIC2);
}

/**
 * 是否扩展分区
 */
static inline int is_extended_partition(struct msdos_partition *p)
{
	unsigned char partition_type = get_unaligned(&p->partition_type);

	return (partition_type == DOS_EXTENDED_PARTITION ||
		partition_type == WIN98_EXTENDED_PARTITION ||
		partition_type == LINUX_EXTENDED_PARTITION);
}

static inline int get_sector_count(struct msdos_partition *p)
{
	unsigned int sector_count = get_unaligned(&p->sector_count);

	return le32_to_cpu(sector_count);
}

static inline int get_start_sector(struct msdos_partition *p)
{
	unsigned int start_sector = get_unaligned(&p->start_sector);

	return le32_to_cpu(start_sector);
}

static bool active_flag_valid(char flag)
{
	return !flag || flag == ACTIVE_PARTITION;
}

static inline void found_new_partition(struct device_partitions *p,
	sector_t start, sector_t count)
{
	if (p->partition_count < MAX_PART) {
		p->parts[p->partition_count].start_sector = start;
		p->parts[p->partition_count].sector_count = count;
	}
	p->partition_count++;
}

static void parse_extended(struct device_partitions *scan_result,
	struct block_device *bdev, u32 first_sector, u32 first_size)
{
	int sector_size = blkdev_hardsect_size(bdev);
	struct msdos_partition *partition;
	struct page_frame *page;
	unsigned char *sec_data;
	u32 cur_sector, cur_size;
	int loop = 0;
	int i;

	cur_sector = first_sector;
	cur_size = first_size;

	while (1) {
		if (++loop > 100)
			return;

		if (scan_result->partition_count == MAX_PART)
			return;

		sec_data = blkdev_read_sector(bdev, cur_sector, &page);
		if (!sec_data)
			return;

		if (!check_magic(sec_data))
			goto done; 

		partition = (struct msdos_partition *) (sec_data + 0x1be);
		for (i = 0; i < 4; i++, partition++) {
			u32 offs, size, next;

			/**
			 * 第一遍不处理扩展分区
			 */
			if (!get_sector_count(partition)
			    || is_extended_partition(partition))
				continue;

			offs = get_start_sector(partition) * sector_size >> 9;
			size = get_sector_count(partition) * sector_size >> 9;
			next = cur_sector + offs;
			/**
			 * 对某些系统来说，第3、4项可能有垃圾数据
			 * 这里做一些简单的检查
			 */
			if (i >= 2) {
				if (offs + size > cur_size)
					continue;
				if (next < first_sector)
					continue;
				if (next + size > first_sector + first_size)
					continue;
			}

			found_new_partition(scan_result, next, size);
			loop = 0;
			if (scan_result->partition_count == MAX_PART)
				goto done;
		}

		/**
		 * 第二遍，处理第一个扩展分区
		 */
		partition = (struct msdos_partition *) (sec_data + 0x1be);
		for (i = 0; i < 4; i++, partition++)
			if (get_sector_count(partition)
			    && is_extended_partition(partition))
				break;
		if (i == 4)
			goto done;

		cur_sector = first_sector
			+ get_start_sector(partition) * (sector_size >> 9);
		cur_size = get_sector_count(partition) * (sector_size >> 9);
		loosen_page_cache(page);
	}

done:
	loosen_page_cache(page);
}

int scan_msdos_partitions(struct device_partitions *scan_result,
	struct block_device *bdev)
{
	int sector_size = blkdev_hardsect_size(bdev);
	struct page_frame *page;
	unsigned char *sec_data;
	struct msdos_partition *partition;
	int part_idx;

printk("xby-debug, step 1\n");
	sec_data = blkdev_read_sector(bdev, 0, &page);
	if (!sec_data)
		return -EIO;

printk("xby-debug, step 2\n");
	if (!check_magic(sec_data)) {
		loosen_page_cache(page);
		return 0;
	}

printk("xby-debug, step 3\n");
	partition = (struct msdos_partition *) (sec_data + 0x1be);
	/**
	 * 最多4个分区，逐一检查分区表的有效性
	 */
	for (part_idx = 0; part_idx < 4; part_idx++, partition++) {
		/**
		 * 既不是普通分区，也不是启动分区
		 * 扇区可能有问题
		 */
		if (!active_flag_valid(partition->active_flag)) {
			loosen_page_cache(page);
			return 0;
		}
printk("xby-debug, step 4\n");
	}

	scan_result->partition_count = 0;
	partition = (struct msdos_partition *) (sec_data + 0x1be);
	for (part_idx = 0 ; part_idx < 4; part_idx++, partition++) {
		u32 start = get_start_sector(partition) * (sector_size >> 9);
		u32 size = get_sector_count(partition) * (sector_size >> 9);

		if (!size)
			continue;

		if (is_extended_partition(partition)) {
			found_new_partition(scan_result, start, size == 1 ? 1 : 2);
			parse_extended(scan_result, bdev, start, size);
			continue;
		}

		found_new_partition(scan_result, start, size);
	}

	loosen_page_cache(page);

	return 1;
}
