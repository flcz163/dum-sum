#include <dim-sum/block_buf.h>
#include <dim-sum/err.h>
#include <dim-sum/fs.h>
#include <dim-sum/lext3_fs.h>

#include "internal.h"

static unsigned char filetype_table[] = {
	DT_UNKNOWN, DT_REG, DT_DIR, DT_CHR, DT_BLK, DT_FIFO, DT_SOCK, DT_LNK
};

/**
 * 校验目录项是否正确，防止硬件问题引起错误
 */
int lext3_verify_dir_item(const char *function, struct file_node *dir,
	struct lext3_dir_item *dir_item, struct blkbuf_desc *blkbuf,
	unsigned long offset)
{
	const char * error_msg = NULL;
 	const int rlen = le16_to_cpu(dir_item->rec_len);

	if (rlen < lext3_dir_item_size(1))
		error_msg = "rec_len is smaller than minimal";
	else if (rlen % 4 != 0)
		error_msg = "rec_len % 4 != 0";
	else if (rlen < lext3_dir_item_size(dir_item->name_len))
		error_msg = "rec_len is too small for name_len";
	else if (((char *) dir_item - blkbuf->block_data) + rlen > dir->super->block_size)
		error_msg = "directory entry across blocks";
	else if (le32_to_cpu(dir_item->file_node_num) >
			le32_to_cpu(super_to_lext3(dir->super)->phy_super->fnode_count))
		error_msg = "file_node out of bounds";

	if (error_msg != NULL)
		lext3_enconter_error (dir->super, "bad entry in directory #%lu: %s - "
			"offset=%lu, file_node=%lu, rec_len=%d, name_len=%d",
			dir->node_num, error_msg, offset,
			(unsigned long)le32_to_cpu(dir_item->file_node_num),
			rlen, dir_item->name_len);

	return error_msg == NULL ? 1 : 0;
}

static int lext3_readdir(struct file *file, void *dirent, filldir_t filldir)
{
	struct blkbuf_desc *blkbuf, *tmp, *ary_blkbuf[16];
	struct lext3_dir_item *dir_item;
	struct super_block *super;
	unsigned long offset, block;
	struct file_node *fnode;
	int i, idx, stored;
	int ra_blocks;
	int error = 0;
	int ret = 0;
	int valid;
	int err;

	fnode = file->fnode_cache->file_node;
	super = fnode->super;

	stored = 0;
	blkbuf = NULL;
	offset = file->pos & (super->block_size - 1);

	while (!error && !stored && file->pos < fnode->file_size) {
		/**
		 * 计算逻辑块位置并读入逻辑块
		 */
		block = (file->pos) >> super->block_size_order;
		blkbuf = lext3_read_metablock(NULL, fnode, block, 0, &err);
		if (!blkbuf) {
			lext3_enconter_error (super,
				"directory #%lu contains a hole at offset %lu",
				fnode->node_num, (unsigned long)file->pos);
			/**
			 * 读入错误，直接移动到下一块
			 */
			file->pos += super->block_size - offset;
			continue;
		}

		if (!offset) {
			/**
			 * 预读16个扇区
			 */
			ra_blocks = 16 >> (super->block_size_order - 9);
			idx = 0;
 			for (i = 0; i < ra_blocks; i++) {
				/**
				 * 读入下一块的物理位置
				 */
				block++;
				tmp = lext3_get_metablock(NULL, fnode, block, 0, &err);
				/**
				 * 数据不是最新的，需要提交请求
				 */
				if (tmp && !blkbuf_is_uptodate(tmp) && !blkbuf_is_locked(tmp)) {
					ary_blkbuf[idx] = tmp;
					idx++;
				}
				else
					loosen_blkbuf (tmp);
			}

			/**
			 * 提交预读请求
			 */
			if (idx) {
				submit_block_requests (READA, idx, ary_blkbuf);
				for (i = 0; i < idx; i++)
					loosen_blkbuf (ary_blkbuf[i]);
			}
		}

revalidate:
		/** 
		 * 自从上次读取调用以来，目录发生了变化
		 * 目录项可能失效，必须从块开始处读取
		 */
		if (file->version != fnode->version) {
			/**
			 * 略过已经读取的部分
			 */
			for (i = 0; i < super->block_size && i < offset; ) {
				dir_item = (struct lext3_dir_item *)(blkbuf->block_data + i);
				/**
				 * 必要的检查，防止死循环
				 */
				if (le16_to_cpu(dir_item->rec_len) < lext3_dir_item_size(1))
					break;
				i += le16_to_cpu(dir_item->rec_len);
			}
			offset = i;
			file->pos = (file->pos & ~(super->block_size - 1)) | offset;
			file->version = fnode->version;
		}

		/**
		 * 读一个完整的数据块
		 */
		while (!error && file->pos < fnode->file_size
		    && offset < super->block_size) {
			dir_item = (struct lext3_dir_item *)(blkbuf->block_data + offset);
			valid = lext3_verify_dir_item("lext3_readdir", fnode,
						dir_item, blkbuf, offset);
			/**
			 * 目录项有问题，跳到下一块
			 */
			if (!valid) {
				file->pos = (file->pos | (super->block_size - 1)) + 1;
				loosen_blkbuf (blkbuf);
				ret = stored;

				goto out;
			}

			/**
			 * 移动到下一个目录项
			 */
			offset += le16_to_cpu(dir_item->rec_len);
			/**
			 * 对于块中第一个目录项来说，有可能是无效的
			 */
			if (le32_to_cpu(dir_item->file_node_num)) {
				unsigned long version = file->version;
				unsigned long dir_type = DT_UNKNOWN;
				unsigned long ft_feature;

				ft_feature = LEXT3_HAS_INCOMPAT_FEATURE(super,
					LEXT3_FEATURE_INCOMPAT_FILETYPE);
				if (ft_feature && (dir_item->file_type >= LLEXT3_FT_MAX))
					dir_type = filetype_table[dir_item->file_type];

				/**
				 * 回调，向调用者返回数据
				 */
				error = filldir(dirent, dir_item->name, dir_item->name_len,
					file->pos, le32_to_cpu(dir_item->file_node_num),
					dir_type);
				if (error)
					break;

				if (version != file->version)
					goto revalidate;

				stored++;
			}
			file->pos += le16_to_cpu(dir_item->rec_len);
		}
		offset = 0;
		loosen_blkbuf (blkbuf);
	}
out:
	return ret;
}

struct file_ops lext3_dir_fileops = {
	.llseek = generic_file_llseek,
	.read = generic_read_dir,
	.readdir = lext3_readdir,
	.ioctl	 = lext3_ioctl,
	.fsync = lext3_sync_file,
};
