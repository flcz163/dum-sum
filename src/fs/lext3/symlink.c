#include <dim-sum/fnode.h>
#include <dim-sum/fs.h>
#include <dim-sum/lext3_fs.h>

static int
lext3_follow_fast_symlink(struct filenode_cache *fnode_cache,
	struct filenode_lookup_s *look)
{
	struct lext3_file_node *lext3_fnode;

	lext3_fnode = fnode_to_lext3(fnode_cache->file_node);
	look_save_symlink_name(look, (char*)lext3_fnode->data_blocks);

	return 0;
}

struct file_node_ops lext3_symlink_fnode_ops = {
	.read_link	= generic_read_link,
	.follow_link	= generic_follow_link,
	.loosen_link	= generic_loosen_link,
};

struct file_node_ops lext3_fast_symlink_fnode_ops = {
	.read_link	= generic_read_link,
	.follow_link	= lext3_follow_fast_symlink,
};

