struct device_partitions;
struct block_device;

/**
 * 分区ID
 */
enum {
	DOS_EXTENDED_PARTITION = 5,
	LINUX_EXTENDED_PARTITION = 0x85,
	WIN98_EXTENDED_PARTITION = 0x0f,

	LINUX_SWAP_PARTITION = 0x82,
	LINUX_RAID_PARTITION = 0xfd,
};

#define ACTIVE_PARTITION	0x80

/**
 * MSDOS扇区描述符
 */
struct msdos_partition {
	/**
	 * 0x80->活动分区
	 */
	unsigned char active_flag;
	/**
	 * 老的扇区信息:起始磁头、扇区、柱面
	 */
	unsigned char unused_head;
	unsigned char unused_sector;
	unsigned char unused_cycle;
	/**
	 * 分区类型
	 */
	unsigned char partition_type;
	/**
	 * 老的扇区信息:结束磁头、扇区、柱面
	 */
	unsigned char unused_end_head;
	unsigned char unused_end_sector;
	unsigned char unused_end_cycle;
	/**
	 * 起始扇区编号，从0开始计数
	 */
	unsigned int start_sector;
	/**
	 * 分区所包含的扇区数
	 */
	unsigned int sector_count;		/* nr of sectors in partition */
} __attribute__((packed));

int scan_msdos_partitions(struct device_partitions *scan_result,
	struct block_device *bdev);
