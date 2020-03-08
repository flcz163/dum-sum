#include <dim-sum/pagemap.h>
#include <dim-sum/blk_dev.h>

struct device_partitions;

typedef int (*scan_partition_f)(struct device_partitions *, struct block_device *);

#define MAX_PART 256

struct device_partitions {
	unsigned int partition_count;
	struct partition_info {
		sector_t start_sector;
		sector_t sector_count;
	} parts[MAX_PART];
};
