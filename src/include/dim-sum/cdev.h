#ifndef __DIM_SUM_CDEV_H
#define __DIM_SUM_CDEV_H

#include <dim-sum/device.h>
#include <dim-sum/fs.h>
#include <dim-sum/types.h>
#include <dim-sum/dev_container.h>

/**
 * 字符设备驱动程序描述符
 */
struct char_device {
	struct device device;
	/**
	 * 指向设备驱动程序文件操作表的指针
	 */
	struct file_ops *ops;
	/**
	 * 引用此设备的文件结点链表的头
	 */
	struct double_list fnode_list;
};

void cdev_init(struct char_device *, struct file_ops *);

struct char_device *cdev_alloc(void);

void loosen_chrdev(struct char_device *p);

int putin_chrdev_container(struct char_device *, devno_t, unsigned);

void takeout_chrdev_container(struct char_device *);

void chrdev_detach(struct file_node *);

#endif /* __DIM_SUM_CDEV_H */
