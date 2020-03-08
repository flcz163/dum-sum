#include <dim-sum/aio.h>
#include <dim-sum/beehive.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/blk_infrast.h>
#include <dim-sum/block_buf.h>
#include <dim-sum/disk.h>
#include <dim-sum/err.h>
#include <dim-sum/fnode.h>
#include <dim-sum/mount.h>
#include <dim-sum/pagemap.h>
#include <dim-sum/uio.h>

#include "internal.h"

static struct mount_desc *blkfs_mount;
struct super_block *blkfs_superblock;

static struct block_device *blkdev_get_desc(struct file_node *file_node);

/**
 * 保护设备链表的锁
 */
static struct smp_lock blkdev_lock = SMP_LOCK_UNLOCKED(blkdev_lock);
/**
 * 所有的块设备描述符被插入在一个全局链表中，链表首部是由变量all_bdevs表示的。
 * 链表链表所用的指针位于块设备描述符的bd_list字段中。
 */
static struct double_list all_bdevs = LIST_HEAD_INITIALIZER(all_bdevs);

static struct beehive_allotter * blkdev_fnode_allotter;

struct blkdev_file_node {
	struct block_device blkdev;
	struct file_node fnode;
};

static inline struct blkdev_file_node *fnode_to_blknode(struct file_node *fnode)
{
	return container_of(fnode, struct blkdev_file_node, fnode);
}

static inline struct block_device *fnode_to_blk(struct file_node *fnode)
{
	return &fnode_to_blknode(fnode)->blkdev;
}

/**
 * 根据块设备的块大小，以及文件系统的块大小
 * 计算最小的块大小。
 */
int sb_min_blocksize(struct super_block *sb, int size)
{
	int minsize = blkdev_hardsect_size(sb->blkdev);

	if (size < minsize)
		size = minsize;

	return superblock_set_blocksize(sb, size);
}

int set_blocksize(struct block_device *blkdev, int size)
{
	if (size > PAGE_SIZE || size < 512 || (size & (size-1)))
		return -EINVAL;

	if (size < blkdev_hardsect_size(blkdev))
		return -EINVAL;

	if (blkdev->bd_block_size != size) {
		blkdev_sync(blkdev);
		blkdev->bd_block_size = size;
		blkdev->fnode->block_size_order = blksize_bits(size);
		/**
		 * 使页面缓存全部失效
		 */
		blkdev_invalidate_pgcache(blkdev);
		truncate_inode_pages(blkdev->fnode->cache_space, 0);
	}
	return 0;
}

void blkdev_set_size(struct block_device *blkdev, loff_t size)
{
	unsigned bsize = blkdev_hardsect_size(blkdev);

	blkdev->fnode->file_size = size;
	while (bsize < PAGE_CACHE_SIZE) {
		if (size & bsize)
			break;
		bsize <<= 1;
	}
	blkdev->bd_block_size = bsize;
	blkdev->fnode->block_size_order = blksize_bits(bsize);
}

static sector_t max_block(struct block_device *blkdev)
{
	sector_t retval = ~((sector_t)0);
	loff_t sz = fnode_size(blkdev->fnode);

	if (sz) {
		unsigned int size = block_size(blkdev);
		unsigned int sizebits = blksize_bits(size);
		retval = (sz >> sizebits);
	}

	return retval;
}

static void __blkdev_detach(struct file_node *fnode)
{
	list_del_init(&fnode->device);
	fnode->blkdev = NULL;
	fnode->cache_space = &fnode->cache_data;
}

void blkdev_detach(struct file_node *fnode)
{
	smp_lock(&blkdev_lock);
	if (fnode->blkdev)
		__blkdev_detach(fnode);
	smp_unlock(&blkdev_lock);
}

static inline unsigned long hash(devno_t dev)
{
	return MAJOR(dev)+MINOR(dev);
}

static int bdev_test(struct file_node *fnode, void *data)
{
	return fnode_to_blknode(fnode)->blkdev.devno == *(devno_t *)data;
}

static int bdev_set(struct file_node *fnode, void *data)
{
	fnode_to_blknode(fnode)->blkdev.devno = *(devno_t *)data;
	return 0;
}

/**
 * 根据主次设备号获取块设备描述符的的地址。
 */
struct block_device *__find_hold_block_device(devno_t dev_num)
{
	struct block_device *blkdev;
	struct file_node *fnode;

	/**
	 * 在块设备文件系统中查找设备文件节点
	 */
	fnode = fnode_find_alloc_special(blkfs_mount->super_block,
		hash(dev_num), bdev_test, bdev_set, &dev_num);

	/**
	 * 不存在 的设备
	 */
	if (!fnode)
		return NULL;

	blkdev = &fnode_to_blknode(fnode)->blkdev;

	if (fnode->state & FNODE_NEW) {
		blkdev->container = NULL;
		blkdev->fnode = fnode;
		blkdev->bd_block_size = (1 << fnode->block_size_order);
		blkdev->partition_count = 0;
		blkdev->partition_uptodate = 1;
		fnode->mode = S_IFBLK;
		fnode->devno = dev_num;
		fnode->blkdev = blkdev;
		fnode->cache_data.ops = &def_blk_cachespace_ops;
		cache_space_set_allocflags(&fnode->cache_data, PAF_KERNEL);
		fnode->cache_data.blkdev_infrast = &default_blkdev_infrast;
		smp_lock(&blkdev_lock);
		list_insert_front(&blkdev->list, &all_bdevs);
		smp_unlock(&blkdev_lock);
		fnode->state &= ~(FNODE_TRANSFERRING|FNODE_NEW);
		wake_up_filenode(fnode);
	}

	return blkdev;
}

void loosen_block_device(struct block_device *blkdev)
{
	loosen_file_node(blkdev->fnode);
}

/**
 * 将文件内的逻辑扇区号转换为物理扇区号
 * 对块设备来说，这两个数是一样的
 */
static int
blkdev_map_block(struct file_node *fnode, sector_t block_num,
		struct blkbuf_desc *blkbuf, int create)
{
	/**
	 * 逻辑块号不能超过物理容量
	 */
	if (block_num >= max_block(fnode_to_blk(fnode)))
		return -EIO;

	/**
	 * 逻辑块号等于物理块号
	 */
	blkbuf->blkdev = fnode_to_blk(fnode);
	blkbuf->block_num_dev = block_num;
	/**
	 * 磁盘缓冲区对象的设置有效，可以向磁盘提交请求了
	 */
	blkbuf_set_mapped(blkbuf);
	return 0;
}

static int
blkdev_map_blocks(struct file_node *fnode, sector_t block_start,
	unsigned long block_count, struct blkbuf_desc *blkbuf, int create)
{
	sector_t end_block = max_block(fnode_to_blk(fnode));

	if ((block_start >= end_block) ||
	    (block_count >= end_block) ||
	    (block_start + block_count > end_block))
		return -EIO;

	blkbuf->blkdev = fnode_to_blk(fnode);
	blkbuf->block_num_dev = block_start;
	blkbuf->size = block_count << fnode->block_size_order;
	blkbuf_set_mapped(blkbuf);

	return 0;
}

/**
 * 通过设备文件名称，加载设备描述符到内存中
 * 但是还并不打开设备
 */
struct block_device *blkdev_load_desc(const char *path)
{
	struct block_device *blkdev;
	struct file_node *fnode;
	struct filenode_lookup_s look = {0};
	int error;

	if (!path || !*path)
		return ERR_PTR(-EINVAL);

	error = load_filenode_cache((char *)path, FNODE_LOOKUP_READLINK, &look);
	if (error)
		return ERR_PTR(error);

	fnode = look.filenode_cache->file_node;
	error = -ENOTBLK;
	if (!S_ISBLK(fnode->mode))
		goto fail;

	error = -EACCES;
	if (look.mnt->flags & MNT_NODEV)
		goto fail;

	error = -ENOMEM;
	blkdev = blkdev_get_desc(fnode);
	if (!blkdev)
		goto fail;

out:
	loosen_fnode_look(&look);
	return blkdev;
fail:
	blkdev = ERR_PTR(error);
	goto out;
}

static int
real_open_device(struct block_device *blkdev, mode_t mode, unsigned flags,
	struct disk_device *disk, int part_num)
{
	int ret;

	blkdev->disk = disk;
	/**
	 * 是一个整盘，而不是分区
	 */
	if (!part_num) {
		struct blkdev_infrast *infrast;

		blkdev->container = blkdev;
		if (disk->fops->open) {
			/**
			 * 该盘定义了打开方法。就执行它
			 * 该方法是由块设备驱动程序定义的定制函数。
			 */
			ret = disk->fops->open(blkdev, mode);
			if (ret)
				goto out;
		}

		if (!blkdev->open_count) {
			blkdev_set_size(blkdev, (loff_t)get_disk_sectors(disk) << 9);

			infrast = blk_get_infrastructure(blkdev);
			if (infrast == NULL)
				infrast = &default_blkdev_infrast;
			blkdev->fnode->cache_data.blkdev_infrast = infrast;
		}
	/**
	 * 设备是一个分区
	 */
	} else {
		struct partition_desc *partion;
		struct block_device *whole;

		/**
		 * 磁盘设备的次设备号为0
		 * 查找磁盘设备
		 */
		whole = find_hold_block_device(disk->dev_major, 0);
		if (!whole) {
			ret = -ENOMEM;
			goto out;
		}

		/**
		 * 打开磁盘设备
		 */
		ret = open_block_device(whole, mode, flags);
		if (ret) {
			loosen_block_device(whole);
			goto out;
		}

		blkdev->container = whole;
		down(&whole->sem);
		/**
		 * 找到分区的描述符
		 */
		partion = &disk->part[part_num - 1];
		blkdev->fnode->cache_data.blkdev_infrast =
			whole->fnode->cache_data.blkdev_infrast;

		if (!(disk->flags & DISK_FLAG_REGISTERED) || !partion
		    || !partion->sector_count) {
			up(&whole->sem);
			ret = -ENXIO;
			goto out;
		}

		blkdev->partition = partion;
		/**
		 * 设置索引结点中分区大小和扇区大小
		 */
		blkdev_set_size(blkdev, (loff_t)partion->sector_count << 9);
		up(&whole->sem);
	}

	return 0;
out:
	blkdev->disk = NULL;
	blkdev->fnode->cache_data.blkdev_infrast = &default_blkdev_infrast;
	if (blkdev != blkdev->container)
		close_block_device(blkdev->container);
	blkdev->container = NULL;
	loosen_disk(disk);

	return ret;
}

int open_block_device(struct block_device *blkdev, mode_t mode, unsigned flags)
{
	struct disk_device *disk;
	int part_num;
	int ret;

	/**
	 * 在块设备哈希表中查找设备所属的磁盘
	 */
	disk = blkdev_container_lookup(blkdev->devno, &part_num);
	if (!disk)
		return -ENXIO;

	down(&blkdev->sem);
	/**
	 * 第一次打开
	 */
	if (!blkdev->open_count) {
		if (real_open_device(blkdev, mode, flags, disk, part_num))
			goto out;
	} else {
		/** 
		 * 设备已经打开了
		 */
		loosen_disk(disk);
		/**
		 * 不是分区
		 */
		if (blkdev->container == blkdev) {
			/**
			 * 调用块设备的open方法。
			 */
			if (blkdev->disk->fops->open) {
				ret = blkdev->disk->fops->open(blkdev, mode);
				if (ret)
					goto out;
			}
		}
	}

	if (blkdev->container != blkdev) {
		down(&blkdev->container->sem);
		blkdev->container->open_partitions++;
		up(&blkdev->container->sem);
	} else {
		/**
		 * 还没有扫描过分区，扫描分区信息
		 */
		if (!blkdev->partition_uptodate)
			rescan_partitions(blkdev->disk, blkdev);
	}

	/**
	 * 无论如何，都增加打开计数
	 */
	blkdev->open_count++;
	up(&blkdev->sem);

	return 0;

out:
	up(&blkdev->sem);

	return ret;
}

int close_block_device(struct block_device *blkdev)
{
	struct file_node *fnode = blkdev->fnode;
	struct disk_device *disk = blkdev->disk;
	int ret = 0;

	down(&blkdev->sem);

	blkdev->open_count--;
	/**
	 * 彻底关闭文件
	 */
	if (!blkdev->open_count) {
		/**
		 * 回写磁盘
		 */
		blkdev_sync(blkdev);
		/**
		 * 使页面缓存全部失效
		 */
		blkdev_invalidate_pgcache(blkdev);
		truncate_inode_pages(blkdev->fnode->cache_space, 0);
	}

	if (blkdev->container == blkdev) {
		/**
		 * 块设备是磁盘，调用磁盘的回调
		 */
		if (disk->fops->release)
			ret = disk->fops->release(fnode, NULL);
	} else {
		/**
		 * 递减磁盘的分区数量
		 */
		down(&blkdev->container->sem);
		blkdev->container->open_partitions--;
		up(&blkdev->container->sem);
	}

	if (!blkdev->open_count) {
		loosen_disk(disk);
		if (blkdev->container != blkdev)
			blkdev->partition = NULL;
		blkdev->disk = NULL;
		/**
		 * 这将会扔掉上层发出来的磁盘IO请求
		 */
		blkdev->fnode->cache_data.blkdev_infrast = &default_blkdev_infrast;
		if (blkdev != blkdev->container)
			close_block_device(blkdev->container);
		blkdev->container = NULL;
	}

	up(&blkdev->sem);
	loosen_block_device(blkdev);

	return ret;
}

/**
 * 通过devfs文件名，打开设备文件
 * 如/dev/vda/part1
 * 这是本模块提供给vfs层的**重要**接口
 */
struct block_device *blkdev_open_exclude(const char *path, int flags, void *holder)
{
	struct block_device *blkdev;
	mode_t mode = FMODE_READ;
	int error = 0;

	/**
	 * 加载VFS层的描述符
	 */
	blkdev = blkdev_load_desc(path);
	if (IS_ERR(blkdev))
		return blkdev;

	if (!(flags & MFLAG_RDONLY))
		mode |= FMODE_WRITE;
	/**
	 * 调用驱动层，打开设备
	 */
	error = open_block_device(blkdev, mode, 0);
	if (error) {
		loosen_block_device(blkdev);
		return ERR_PTR(error);
	}

	/**
	 * 判断读写权限
	 */
	error = -EACCES;
	if (!(flags & MFLAG_RDONLY) && blkdev_read_only(blkdev))
		goto close;
	/**
	 * 获得排它权限
	 */
	error = blkdev_exclude(blkdev, holder);
	if (error)
		goto close;

	return blkdev;
	
close:
	close_block_device(blkdev);
	return ERR_PTR(error);
}

void blkdev_close_exclude(struct block_device *blkdev)
{
	blkdev_exclude_invalidate(blkdev);
	close_block_device(blkdev);
}

static int blkdev_readpage(struct file * file, struct page_frame * page)
{
	return submit_read_page_blocks(page, blkdev_map_block);
}

static int blkdev_writepage(struct page_frame *page, struct writeback_control *wbc)
{
	return blkbuf_write_page(page, blkdev_map_block, wbc);
}

static int blkdev_prepare_write(struct file *file, struct page_frame *page, unsigned from, unsigned to)
{
	return blkbuf_prepare_write(page, from, to, blkdev_map_block);
}

static int blkdev_commit_write(struct file *file,
	struct page_frame *page, unsigned from, unsigned to)
{
	return blkbuf_commit_write(file->cache_space->fnode, page, from, to);
}

/**
 * 使块设备的页面缓存失效
 */
void blkdev_invalidate_pgcache(struct block_device *bdev)
{
	invalidate_page_cache(bdev->fnode->cache_space);
}

static ssize_t
blkdev_direct_IO(int rw, struct async_io_desc *aio, const struct io_segment *iov,
			loff_t offset, unsigned long nr_segs)
{
	struct file *file = aio->file;
	struct file_node *fnode = file->cache_space->fnode;

	return __blockdev_direct_IO(rw, aio, fnode, fnode_to_blk(fnode),
		iov, offset, nr_segs, blkdev_map_blocks, NULL, DIO_NO_LOCKING);
}

struct cache_space_ops def_blk_cachespace_ops = {
	.readpage	= blkdev_readpage,
	.writepage	= blkdev_writepage,
	.sync_page	= blkbuf_sync_page,
	.prepare_write	= blkdev_prepare_write,
	.commit_write	= blkdev_commit_write,
	.writepages	= writepages_journal,
	.direct_IO	= blkdev_direct_IO,
};

/**
 * 获得块设备描述符bdev的地址.该函数接收索引结点对象的地址
 */
static struct block_device *blkdev_get_desc(struct file_node *fnode)
{
	struct block_device *blkdev;

	smp_lock(&blkdev_lock);
	blkdev = fnode->blkdev;
	/**
	 * 文件节点已经与设备描述符绑定了
	 */
	if (blkdev && hold_file_node(blkdev->fnode)) {
		smp_unlock(&blkdev_lock);
		return blkdev;
	}

	smp_unlock(&blkdev_lock);
	/**
	 * 根据设备号查找描述符
	 */
	blkdev = __find_hold_block_device(fnode->devno);
	if (blkdev) {
		smp_lock(&blkdev_lock);
		/**
		 * 将旧的描述符
		 */
		if (fnode->blkdev && (fnode->blkdev != blkdev)) {
			loosen_block_device(blkdev);
			blkdev = fnode->blkdev;
			hold_file_node(blkdev->fnode);
		} else {
			fnode->cache_space = blkdev->fnode->cache_space;
			/**
			 * 将索引结点插入到由bd_inodes确立的块设备描述符的已打开索引结点链表中
			 */
			list_insert_front(&fnode->device, &blkdev->inodes);
		}
		smp_unlock(&blkdev_lock);
	}

	return blkdev;
}

/**
 * 设置块设备的排它访问
 */
int blkdev_exclude(struct block_device *blkdev, void *holder)
{
	int ret;

	smp_lock(&blkdev_lock);

	if (blkdev->holder == holder)
		ret = 0;
	else if (blkdev->holder != NULL)
		ret = -EBUSY;
	else if (blkdev->container == blkdev)
		ret = 0;
	else if (blkdev->container->holder == &blkdev_lock)
		ret = 0;
	else if (blkdev->container->holder != NULL)
		ret = -EBUSY;
	else
		ret = 0;

	if (ret == 0) {
		blkdev->container->hold_count++;
		blkdev->container->holder = &blkdev_lock;
		blkdev->hold_count++;
		blkdev->holder = holder;
	}

	smp_unlock(&blkdev_lock);

	return ret;
}

/**
 * 使块设备的排它设置失效
 */
void blkdev_exclude_invalidate(struct block_device *blkdev)
{
	smp_lock(&blkdev_lock);

	blkdev->container->hold_count--;
	if (!blkdev->container->hold_count)
		blkdev->container->holder = NULL;
	blkdev->hold_count--;
	if (!blkdev->hold_count)
		blkdev->holder = NULL;

	smp_unlock(&blkdev_lock);
}

/**
 * 块设备文件的缺省操作-open
 */
int blkdev_open(struct file_node *fnode, struct file *filp)
{
	struct block_device *blkdev;
	int ret;

	filp->flags |= O_LARGEFILE;

	/**
	 * 从文件节点中获得块设备描述符
	 * 如果没有就分配一个
	 */
	blkdev = blkdev_get_desc(fnode);
	if (!blkdev)
		return -ENXIO;

	/**
	 * 打开块设备，如果失败会释放引用计数
	 */
	ret = open_block_device(blkdev, filp->f_mode, filp->flags);
	if (ret) {
		loosen_block_device(blkdev);
		return ret;
	}
	filp->cache_space = blkdev->fnode->cache_space;

	/**
	 * 不是独占打开，直接返回
	 */
	if (!(filp->flags & O_EXCL))
		return 0;

	/**
	 * 否则是独占打开，获取独占权限
	 */
	ret = blkdev_exclude(blkdev, filp);
	if (ret)
		/**
		 * 其他人已经打开设备了，释放引用并返回错误
		 */
		close_block_device(blkdev);

	return ret;
}

/**
 * 缺省的块设备文件操作方法-release
 */
static int blkdev_close(struct file_node *fnode, struct file *filp)
{
	struct block_device *blkdev = fnode_to_blk(filp->cache_space->fnode);

	if (blkdev->holder == filp)
		blkdev_exclude_invalidate(blkdev);

	return close_block_device(blkdev);
}

static loff_t blkdev_llseek(struct file *file, loff_t offset, int origin)
{
	struct file_node *fnode = file->cache_space->fnode;
	loff_t size;
	loff_t ret;

	down(&fnode->sem);

	size = fnode_size(fnode);
	switch (origin) {
	case SEEK_END:
		offset += size;
		break;
	case SEEK_CUR:
		offset += file->pos;
	}

	ret = -EINVAL;
	if (offset >= 0 && offset <= size) {
		file->pos = offset;
		ret = offset;
	}

	up(&fnode->sem);

	return ret;
}

static ssize_t blkdev_file_write(struct file *file, const char __user *buf,
				   size_t count, loff_t *ppos)
{
	struct io_segment iov = { .base = (void __user *)buf, .len = count };

	return __generic_file_write(file, &iov, 1, ppos);
}

/**
 * 缺省的块设备文件操作-aio_write
 */
static ssize_t blkdev_file_aio_write(struct async_io_desc *aio, const char __user *buf,
				   size_t count, loff_t pos)
{
	struct io_segment io_seg = { .base = (void __user *)buf,
					.len = count };
		
	return __generic_file_aio_write(aio, &io_seg, 1, &aio->pos);
}

static int blkdev_fsync(struct file *filp, struct filenode_cache *fnode_cache, int datasync)
{
	return blkdev_sync(fnode_to_blk(filp->cache_space->fnode));
}

static int blk_ioctl(struct file_node *fnode, struct file *file, unsigned cmd,
			unsigned long arg)
{
	return blkdev_ioctl(file->cache_space->fnode, file, cmd, arg);
}

/**
 * 打开文件时，如果文件是块设备文件
 * 则sys_open函数会设置文件对象的回调函数为本对象。
 */
struct file_ops def_blk_fops = {
	.open		= blkdev_open,
	.release	= blkdev_close,
	.llseek		= blkdev_llseek,
	.read		= generic_file_read,
	.write		= blkdev_file_write,
  	.aio_read	= generic_file_aio_read,
  	.aio_write	= blkdev_file_aio_write, 
	.mmap		= generic_file_mmap,
	.fsync		= blkdev_fsync,
	.ioctl		= blk_ioctl,
	.readv		= generic_file_readv,
	.writev		= __generic_file_write,
	.sendfile	= generic_file_sendfile,
};

static struct file_node *blkdev_alloc_fnode(struct super_block *sb)
{
	struct blkdev_file_node *blk_fnode;
	struct block_device *blkdev;

	blk_fnode = beehive_alloc(blkdev_fnode_allotter, PAF_KERNEL | __PAF_ZERO);
	if (!blk_fnode)
		return NULL;
 
	blkdev = &blk_fnode->blkdev;
	sema_init(&blkdev->sem, 1);
	sema_init(&blkdev->mount_sem, 1);
	list_init(&blkdev->inodes);
	list_init(&blkdev->list);

	fnode_init(&blk_fnode->fnode);

	return &blk_fnode->fnode;
}

static void blkdev_free_fnode(struct file_node *file_node)
{
	struct blkdev_file_node *blknode = fnode_to_blknode(file_node);

	beehive_free(blkdev_fnode_allotter, blknode);
}

static void bdev_clean_fnode(struct file_node *fnode)
{
	struct block_device *blkdev = &fnode_to_blknode(fnode)->blkdev;

	smp_lock(&blkdev_lock);
	while (!list_is_empty(&blkdev->inodes)) {
		struct file_node *node;

		node = list_first_container(&blkdev->inodes, struct file_node, device);
		__blkdev_detach(node);
	}
	list_del_init(&blkdev->list);
	smp_unlock(&blkdev_lock);
}

static struct super_block_ops blkdev_superblock_ops = {
	.statfs = simple_statfs,
	.alloc = blkdev_alloc_fnode,
	.free = blkdev_free_fnode,
	.release = generic_delete_fnode,
	.clean_fnode = bdev_clean_fnode,
};

static struct super_block *
blkdev_load_filesystem(struct file_system_type *fs_type, int flags,
	const char *dev_name, void *data)
{
	return load_isolate_filesystem(fs_type, "blkdev-fs",
		&blkdev_superblock_ops, BLKFS_MAGIC);
}

static struct file_system_type blkdev_filesystem = {
	.name		= "blkdev-fs",
	.load_filesystem = blkdev_load_filesystem,
	.unload_filesystem	= unload_isolate_filesystem,
};

void __init init_blkdev(void)
{
	int err;

	blkdev_fnode_allotter = beehive_create("blkdev_fnode",
		sizeof(struct blkdev_file_node), 0,
		BEEHIVE_HWCACHE_ALIGN | BEEHIVE_RECLAIM_ABLE | BEEHIVE_PANIC,
		NULL);

	err = register_filesystem(&blkdev_filesystem);
	if (err)
		panic("Cannot register block device pseudo file system");

	blkfs_mount = do_internal_mount(blkdev_filesystem.name, 0, blkdev_filesystem.name, NULL);
	err = PTR_ERR(blkfs_mount);
	if (IS_ERR(blkfs_mount))
		panic("Cannot create block device pseudo file system");

	blkfs_superblock = blkfs_mount->super_block;
}
