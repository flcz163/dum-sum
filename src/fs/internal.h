extern struct beehive_allotter *file_allotter;
struct filenode_cache *__find_file_indir_alloc(struct file_name *name, struct filenode_cache * base, struct filenode_lookup_s *look);
int __advance_symlink(struct filenode_cache *filenode_cache, struct filenode_lookup_s *look);
extern int load_filenode_cache(char *, unsigned, struct filenode_lookup_s *);
extern int load_filenode_user(char __user *, unsigned, struct filenode_lookup_s *);
void advance_mount_point(struct mount_desc **mnt,
	struct filenode_cache **fnode_cache);