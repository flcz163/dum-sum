#include <dim-sum/capability.h>
#include <dim-sum/errno.h>
#include <dim-sum/fs.h>
#include <dim-sum/mm.h>
#include <dim-sum/sched.h>

/**
 * 检查是否允许设置文件属性
 */
int fnode_check_attr(struct file_node *fnode, struct fnode_attr *attr)
{
	int ret = -EPERM;
	unsigned int valid = attr->valid;

	if (valid & ATTR_FORCE)
		return 0;

	if ((valid & ATTR_UID) &&
	    (current->fsuid != fnode->uid ||
	     attr->uid != fnode->uid) && !capable(CAP_CHOWN))
		return ret;

	if ((valid & ATTR_GID) &&
	    (current->fsuid != fnode->uid ||
	    (!in_group_p(attr->gid) && attr->gid != fnode->gid)) &&
	    !capable(CAP_CHOWN))
		return ret;

	if (valid & ATTR_MODE) {
		if ((current->fsuid != fnode->uid) && !capable(CAP_FOWNER))
			return ret;

		if (!in_group_p((valid & ATTR_GID) ? attr->gid :
				fnode->gid) && !capable(CAP_FSETID))
			attr->ia_mode &= ~S_ISGID;
	}

	if (valid & (ATTR_MTIME_SET | ATTR_ATIME_SET)) {
		if (current->fsuid != fnode->uid && !capable(CAP_FOWNER))
			return ret;
	}

	return 0;
}

int fnode_setattr(struct file_node *fnode, struct fnode_attr * attr)
{
	unsigned int valid = attr->valid;
	int ret = 0;

	if (valid & ATTR_SIZE) {
		if (attr->size != fnode_size(fnode)) {
			ret = vmtruncate(fnode, attr->size);
			if (ret || (valid == ATTR_SIZE))
				goto out;
		} else
			valid |= ATTR_MTIME|ATTR_CTIME;
	}

	if (valid & ATTR_UID)
		fnode->uid = attr->uid;
	if (valid & ATTR_GID)
		fnode->gid = attr->gid;
	if (valid & ATTR_ATIME)
		fnode->access_time = timespec_trunc(attr->ia_atime,
			fnode->super->gran_ns);
	if (valid & ATTR_MTIME)
		fnode->data_modify_time = timespec_trunc(attr->ia_mtime,
			fnode->super->gran_ns);
	if (valid & ATTR_CTIME)
		fnode->meta_modify_time = timespec_trunc(attr->ia_ctime,
			fnode->super->gran_ns);
	if (valid & ATTR_MODE) {
		umode_t mode = attr->ia_mode;

		if (!in_group_p(fnode->gid) && !capable(CAP_FSETID))
			mode &= ~S_ISGID;
		fnode->mode = mode;
	}

	mark_fnode_dirty(fnode);
out:
	return ret;
}
