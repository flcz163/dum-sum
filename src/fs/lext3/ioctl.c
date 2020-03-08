#include <dim-sum/fs.h>
#include <dim-sum/lext3_fs.h>
#include <dim-sum/writeback.h>

#include "internal.h"

int lext3_ioctl(struct file_node *fnode, struct file *file,
	unsigned int cmd, unsigned long arg)
{
	return -ENOTTY;
}
