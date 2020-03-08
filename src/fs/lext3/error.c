#include <dim-sum/fs.h>
#include <dim-sum/lext3_fs.h>
#include <dim-sum/stacktrace.h>
#include <dim-sum/writeback.h>

#include "internal.h"

const char *
lext3_error_msg(struct super_block *super, int errno, char msg_buf[16])
{
	char *msg = NULL;

	switch (errno) {
	case -EIO:
		msg = "IO failure";
		break;
	case -ENOMEM:
		msg = "Out of memory";
		break;
	case -EROFS:
		if (!super || super_to_lext3(super)->journal->flags & JSTATE_ABORT)
			msg = "Journal has aborted";
		else
			msg = "Readonly filesystem";
		break;
	default:
		if (msg_buf) {
			if (snprintf(msg_buf, 16, "error %d", -errno) >= 0)
				msg = msg_buf;
		}
		break;
	}

	return msg;
}

static void enconter_error(struct super_block *super)
{
	struct lext3_superblock_phy *super_phy;
	struct lext3_superblock *lext3_super;

	lext3_super = super_to_lext3(super);
	super_phy = super_to_lext3(super)->phy_super;
	lext3_super->mount_state |= LEXT3_ERROR_FS;
	super_phy->state |= cpu_to_le16(LEXT3_ERROR_FS);

	if (super->mount_flags & MFLAG_RDONLY)
		return;

	if (lext3_test_opt (super, LEXT3_MOUNT_ERRORS_RO)) {
		printk (KERN_CRIT "Remounting filesystem read-only\n");
		super->mount_flags |= MFLAG_RDONLY;
	} else {
		struct journal *journal = lext3_super->journal;

		lext3_super->mount_opt |= LEXT3_MOUNT_ABORT;
		if (journal)
			journal_abort(journal, -EIO);
	}

	if (lext3_test_opt(super, LEXT3_MOUNT_ERRORS_PANIC))
		panic("LEXT3 (device %s): panic forced after error\n",
			super->blkdev_name);

	lext3_commit_super(super, super_phy, 1);
}

void lext3_std_error(struct super_block *super, int errno)
{
	char msg_buf[16];
	const char *msg;

	if (!errno)
		return;

	msg = lext3_error_msg(super, errno, msg_buf);
	printk (KERN_CRIT "EXT3-fs error (device %s): %s\n",
		super->blkdev_name, msg);
	dump_stack();

	enconter_error(super);
}

void lext3_enconter_error(struct super_block *super, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	printk(KERN_CRIT "EXT3-fs error (device %s): ",super->blkdev_name);
	printk(fmt, args);
	printk("\n");
	va_end(args);

	enconter_error(super);
}

void lext3_abort_filesystem(struct super_block *super, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	printk(KERN_CRIT "LEXT3 error (device %s)",super->blkdev_name);
	printk(fmt, args);
	printk("\n");
	va_end(args);
	dump_stack();

	if (lext3_test_opt(super, LEXT3_MOUNT_ERRORS_PANIC))
		panic("EXT3-fs panic from previous error\n");

	if (super->mount_flags & MFLAG_RDONLY)
		return;

	printk(KERN_CRIT "Remounting filesystem read-only\n");
	super_to_lext3(super)->mount_state |= LEXT3_ERROR_FS;
	super->mount_flags |= MFLAG_RDONLY;
	super_to_lext3(super)->mount_opt |= LEXT3_MOUNT_ABORT;
	journal_abort(super_to_lext3(super)->journal, -EIO);
}
