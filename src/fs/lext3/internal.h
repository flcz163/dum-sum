#define DIO_CREDITS (EXT3_RESERVE_TRANS_BLOCKS + 32)

struct indirect_block_desc {
	__le32 *child_pos;
	__le32 child_block_num;
	struct blkbuf_desc *blkbuf;
};

unsigned long lext3_journal_blocks_truncate(struct file_node *fnode);
int lext3_fnode_is_fast_symlink(struct file_node *fnode);
struct journal_handle *lext3_start_trunc_journal(struct file_node *fnode);
int lext3_find_block(struct journal_handle *handle, struct file_node *fnode,
	sector_t iblock, struct blkbuf_desc *result,
	int create, int extend_disksize);
int lext3_get_datablock(struct file_node *fnode, sector_t block,
	struct blkbuf_desc *result, int create);
int lext3_load_fnode(struct file_node *fnode,
	struct lext3_fnode_loc *loc, int in_mem);
int lext3_get_datablock_direct(struct file_node *fnode, sector_t block,
	unsigned long max_blocks, struct blkbuf_desc *bh_result, int create);
int lext3_block_to_path(struct file_node *fnode,
	long blocks, int offsets[4], int *boundary);
struct indirect_block_desc *
lext3_read_branch(struct file_node *fnode, int depth, int *offsets,
	struct indirect_block_desc chain[4], int *err);
int lext3_find_near_block(struct file_node *fnode, long block,
	struct indirect_block_desc chain[4], struct indirect_block_desc *partial,
	unsigned long *goal);

enum lext3_journal_type {
	JOURNAL_TYPE_NOJOURNAL,
	JOURNAL_TYPE_FULL,
	JOURNAL_TYPE_ORDERD,
	JOURNAL_TYPE_WRITEBACK,
};

enum lext3_journal_type lext3_get_journal_type(struct file_node *fnode);
static inline struct journal_handle *lext3_get_journal_handle(void)
{
	return journal_current_handle();
}

int lext3_journal_dirty_data(struct journal_handle *handle,
	struct blkbuf_desc *blkbuf);
int lext3_dir_is_empty(struct file_node *fnode);

#define LEXT3_DIR_PAD			4
#define LEXT3_DIR_ROUND			(LEXT3_DIR_PAD - 1)
static inline int lext3_dir_item_size(int name_len)
{
	int res;

	res = name_len + sizeof(struct lext3_dir_item);
	res = (res + LEXT3_DIR_ROUND) & ~LEXT3_DIR_ROUND;

	return res;
}

static inline struct lext3_dir_item *
next_dir_item(struct lext3_dir_item *item)
{
	struct lext3_dir_item *ret;
	int item_len;

	item_len = le16_to_cpu(item->rec_len);
	ret = (struct lext3_dir_item *)((char *)item + item_len);

	return ret;
}

static inline struct lext3_dir_item *
first_dir_item(struct blkbuf_desc *blkbuf)
{
	return (struct lext3_dir_item *)blkbuf->block_data;
}

static inline bool dir_item_in_blkbuf(struct lext3_dir_item *dir_item,
	struct blkbuf_desc *blkbuf, struct super_block *super)
{
	void *start = (void *)blkbuf->block_data;
	void *end = (void *)(blkbuf->block_data + super->block_size);
	void *item = (void *)dir_item;

	return (item >= start) && (item < end);
}

#define S_SHIFT 12
static unsigned char lext3_type_by_mode[S_IFMT >> S_SHIFT] = {
	[S_IFREG >> S_SHIFT]	= LEXT3_FT_REG_FILE,
	[S_IFDIR >> S_SHIFT]	= LEXT3_FT_DIR,
	[S_IFCHR >> S_SHIFT]	= LEXT3_FT_CHRDEV,
	[S_IFBLK >> S_SHIFT]	= LEXT3_FT_BLKDEV,
	[S_IFIFO >> S_SHIFT]	= LEXT3_FT_FIFO,
	[S_IFSOCK >> S_SHIFT]	= LEXT3_FT_SOCK,
	[S_IFLNK >> S_SHIFT]	= LEXT3_FT_SYMLINK,
};

static inline void set_fnode_dir_index(struct file_node *fnode)
{
	struct super_block *super = fnode->super;

	if (!LEXT3_HAS_COMPAT_FEATURE(super, LEXT3_FEATURE_COMPAT_DIR_INDEX))
		fnode_to_lext3(fnode)->flags &= ~LEXT3_INDEX_FL;
}

static inline void diritem_set_filetype(struct super_block *super,
	struct lext3_dir_item *dir_item, umode_t mode)
{
	if (LEXT3_HAS_INCOMPAT_FEATURE(super, LEXT3_FEATURE_INCOMPAT_FILETYPE))
		dir_item->file_type = lext3_type_by_mode[(mode & S_IFMT)>>S_SHIFT];
}

static inline unsigned long
block_to_group(unsigned long block, struct super_block *super, int *poffset)
{
	struct lext3_superblock_phy *super_phy;
	int offset;

	super_phy = super_to_lext3(super)->phy_super;
	offset = block - le32_to_cpu(super_phy->first_data_block);

	*poffset = offset * LEXT3_BLOCKS_PER_GROUP(super);

	return offset / LEXT3_BLOCKS_PER_GROUP(super);
}

static inline unsigned long
fnode_num_to_group(unsigned long fnode_num, struct super_block *super,
	int *poffset)
{
	struct lext3_superblock_phy *super_phy;

	super_phy = super_to_lext3(super)->phy_super;
	*poffset = (fnode_num - 1) % super_phy->fnodes_per_group;

	return (fnode_num - 1) / super_phy->fnodes_per_group;
}

static inline unsigned long get_super_free_fnodes(struct super_block *super)
{
	struct lext3_superblock *lext3_super = super_to_lext3(super);

	return approximate_counter_read_positive(&lext3_super->free_fnode_count);
}