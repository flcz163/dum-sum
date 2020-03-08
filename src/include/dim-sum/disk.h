#ifndef __DIM_SUM_GENHD_H
#define __DIM_SUM_GENHD_H

#include <dim-sum/device.h>
#include <dim-sum/types.h>
#include <dim-sum/fs.h>
#include <dim-sum/devno.h>
#include <dim-sum/dev_container.h>

struct blk_request_queue;
struct block_device_operations;

#define DISK_MAX_PARTS			256
#define DISK_NAME_LEN			32
#define SYSOBJ_NAME_LEN			32

enum {
	/**
	 * CD磁盘设备
	 */
	__DISK_FLAG_CD,
	/**
	 * 已经注册到系统中
	 */
	__DISK_FLAG_REGISTERED,
};

/**
 * 磁盘设备标志
 */
#define DISK_FLAG_CD				(1UL << __DISK_FLAG_CD)
#define DISK_FLAG_REGISTERED		(1UL << __DISK_FLAG_REGISTERED)

enum {
	/**
	 * 未使用
	 */
	__PART_STATE_UNUSED,
};

#define PART_STATE_UNUSED (1UL << __PART_STATE_UNUSED)

/**
 * 硬盘分区描述符
 */
struct partition_desc {
	struct device device;
	/**
	 * 扇区状态标志
	 */
	unsigned long state;
	/**
	 * 起始扇区
	 */
	long start_sector;
	/**
	 * 扇区数量
	 */
	long sector_count;
	/**
	 * 是否只读
	 */
	int read_only;
	/**
	 * 对分区发出的读操作次数、读取的扇区数、写操作次数、写进分区的扇区数。
	 */
	unsigned read_times, read_sectors, write_times, write_sectors;
	/**
	 * 在系统中注册的对象名称
	 * 用于系统文件系统
	 */
	char sysobj_name[SYSOBJ_NAME_LEN];
};

struct disk_device {
	struct device device;
	/**
	 * 磁盘名称
	 */
	char name[DISK_NAME_LEN];
	/**
	 * 在系统中注册的对象名称
	 * 用于系统文件系统
	 */
	char sysobj_name[SYSOBJ_NAME_LEN];
	/**
	 * 用来描述驱动器状态的标志(很少使用)。
	 * 参见DISK_FLAG_CD
	 */
	int flags;
	/**
	 * 逻辑设备数
	 * 如果为1，表示不支持分区
	 */
	int logic_devices;
	/**
	 * 最大的分区数
	 */
	int max_partition_count;
	/**
	 * 主设备号。一个驱动器至少使用一个次设备号。
	 * 如果驱动器可被分区，将为每个可能的分区都分配一个次设备号。
	 * 通常取值为16，这样一个磁盘可以包含15个分区。某些驱动程序允许多达64个分区。
	 */
	int dev_major;
	/**
	 * 是否只读
	 */
	int read_only;
	/**
	 * 在CD/硬盘设备表中的序号
	 * 用于生成设备文件名
	 */
	int device_sequence;
	/**
	 * 以512字节为一个扇区时，该驱动器可包含的扇区数。
	 * 可以是64位长度，驱动程序不能直接设置该成员，而要将扇区数传递给set_capacity。
	 */
	sector_t capacity;
	/**
	 * 内核使用该结构为设备管理IO请求。
	 */
	struct blk_request_queue *queue;
	/**
	 * 正在等待执行的IO请求数量
	 */
	int request_count;
	const struct block_device_operations *fops;
	void *private_data;
	/**
	 * 分区个数
	 */
	int partition_count;
	/**
	 * 磁盘包含的分区
	 * 动态分配的内存，必须放在最后
	 */
	struct partition_desc part[0];
};

extern struct disk_device *alloc_disk(int max_partition_count);

extern void set_disk_readonly(struct disk_device *disk, int flag);

static inline sector_t get_disk_sectors(struct disk_device *disk)
{
	return disk->capacity;
}
static inline void set_capacity(struct disk_device *disk, sector_t size)
{
	disk->capacity = size;
}

extern void add_disk(struct disk_device *disk);
extern void delete_disk(struct disk_device *gp);
extern void loosen_disk(struct disk_device *disk);
extern struct disk_device *blkdev_container_lookup(devno_t dev, int *part);
extern int rescan_partitions(struct disk_device *disk, struct block_device *bdev);
extern void add_partition(struct disk_device *, int, sector_t, sector_t);
extern void delete_partition(struct disk_device *, unsigned int);
extern struct disk_device *hold_disk(struct disk_device *disk);
char *format_disk_name (struct disk_device *hd, char *buf);
#endif /* __DIM_SUM_GENHD_H */
