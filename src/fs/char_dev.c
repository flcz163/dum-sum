#include <dim-sum/beehive.h>
#include <dim-sum/cdev.h>
#include <dim-sum/err.h>
#include <dim-sum/errno.h>
#include <dim-sum/fs.h>
#include <dim-sum/idr.h>
#include <dim-sum/mutex.h>
#include <dim-sum/smp_rwlock.h>

/**
 * 保护对文件节点的字符设备字段的访问
 */
static struct smp_lock chrdev_lock = 
			SMP_LOCK_UNLOCKED(chrdev_lock);
/**
 * 保护对字符设备驱动的互斥访问
 */
static struct mutex chrdev_mutex =
	MUTEX_INITIALIZER(chrdev_mutex);
/**
 * 用于分配字符设备的主设备号
 */
static struct idr_space chrdev_numspace =
	NUMSPACE_INITIALIZER(chrdev_numspace);
/**
 * 字符设备哈希表
 */
static struct device_container chrdev_container;

static struct device *hold_chrdev(struct char_device *chrdev)
{
	struct device *device;

	device = hold_device(&chrdev->device);

	return device;
}

void loosen_chrdev(struct char_device *chrdev)
{
	loosen_device(&chrdev->device);
}

void chrdev_detach(struct file_node *file_node)
{
	smp_lock(&chrdev_lock);
	list_del_init(&file_node->device);
	file_node->chrdev = NULL;
	smp_unlock(&chrdev_lock);
}

/**
 * 把字符设备放到哈希表中
 */
int putin_chrdev_container(struct char_device *chrdev,
	devno_t dev_num, unsigned count)
{
	chrdev->device.dev_num = dev_num;
	
	return putin_device_container(&chrdev_container, &chrdev->device, chrdev);
}

/**
 * 从哈希表中移除一个字符设备。
 */
void takeout_chrdev_container(struct char_device *chrdev)
{
	takeout_device_container(&chrdev_container, &chrdev->device);
	loosen_device(&chrdev->device);
}

/**
 * 初始化cdev描述符。当cdev描述符是嵌入在其他结构中时，这是有用的方法。
 */
void cdev_init(struct char_device *cdev, struct file_ops *fops)
{
	memset(cdev, 0, sizeof *cdev);
	list_init(&cdev->fnode_list);

	cdev->ops = fops;
}

int alloc_chrdev_region(devno_t *dev, unsigned baseminor, unsigned count,
			const char *name)
{
	int major = get_idr_number(&chrdev_numspace);

	if (major >= MAXMAJOR) {
		*dev = INVDEVNO;
		return -E2BIG;
	} else {
		*dev = MKDEV(major, baseminor);
		return 0;
	}
}

int register_chrdev_region(devno_t from, unsigned count, const char *name)
{
	return 0;
}

void unregister_chrdev_region(devno_t from, unsigned count)
{
}

/**
 * 每次打开字符设备时，均回调此方法
 * 	file_node:文件节点指针
 * 	filp:文件描述符指针
 */
int chrdev_open(struct file_node *file_node, struct file *filp)
{
	struct char_device *chrdev = file_node->chrdev;
	struct char_device *standby = NULL;
	int ret = 0;

	smp_lock(&chrdev_lock);
	/**
	 * 检查文件节点指向的字符设备是否存在
	 * 如果存在，表示字符设备已经被打开，增加引用计数即可
	 */
	if (!chrdev) {
		struct device *dev;
		int minor;

		smp_unlock(&chrdev_lock);
		/**
		 * 根据设备编号，在设备哈希表中搜索设备
		 */
		dev = device_container_lookup(&chrdev_container,
				file_node->devno, &minor);
		/**
		 * 错误的设备号，可能是驱动加载不成功
		 * 直接返回错误
		 */
		if (!dev)
			return -ENXIO;

		standby = container_of(dev, struct char_device, device);
		/**
		 * 在锁的保护下，看看文件节点是否仍然没有打开字符设备
		 * 其他人可能在同步打开文件
		 */
		smp_lock(&chrdev_lock);
		chrdev = file_node->chrdev;
		if (!chrdev) {
			/**
			 * 将设备与文件节点绑定起来
			 */
			file_node->chrdev = chrdev = standby;
			/**
			 * 并设置次设备索引号
			 */
			file_node->minor = minor;
			/**
			 * 将文件节点对象加入到字符设备的节点链表中
			 */
			list_insert_front(&file_node->device, &chrdev->fnode_list);
		} else {
			hold_chrdev(chrdev);
			loosen_chrdev(standby);
		}
	} else 
		hold_chrdev(chrdev);
	
	smp_unlock(&chrdev_lock);

	/**
	 * 初始化文件操作指针
	 */
	filp->file_ops = chrdev->ops;
	if (!filp->file_ops) {
		loosen_chrdev(chrdev);
		return -ENXIO;
	}

	/**
	 * 驱动定义了open方法，就执行它。
	 */
	if (filp->file_ops->open) {
		mutex_lock(&chrdev_mutex);
		ret = filp->file_ops->open(file_node, filp);
		mutex_unlock(&chrdev_mutex);

		if (ret)
			loosen_chrdev(chrdev);
	}

	return ret;
}

struct file_ops def_chrdev_fops = {
	.open = chrdev_open,
};

void __init init_chrdev_early(void)
{
	device_container_init(&chrdev_container);
}
