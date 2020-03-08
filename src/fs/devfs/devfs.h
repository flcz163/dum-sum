struct devfs_file_node {
	/**
	 * 引用计数
	 */
	struct accurate_counter ref_count;
	/**
	 * 文件节点基本信息
	 */
	umode_t mode;
	struct timespec access_time;
	struct timespec data_modify_time;
	struct timespec meta_modify_time;
	unsigned int node_num;
	uid_t uid;
	gid_t gid;
	/**
	 * 指向VFS层文件节点缓存
	 */
	struct filenode_cache *fnode_cache;
	union {
		struct {
			struct smp_rwlock lock;
			struct double_list children;
			/**
			 * 目录节点已经失效
			 */
			bool inactive;
		} dir;
		struct {
			unsigned int length;
			char *linkname;
		} symlink;
		devno_t dev;
	};
	struct double_list child;
	struct devfs_file_node *parent;
	bool may_delete;
	unsigned short name_len;
	char name[0];
};

#define MODE_DIR (S_IFDIR | S_IWUSR | S_IRUGO | S_IXUGO)
