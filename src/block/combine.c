#include <dim-sum/blkio.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/scatterlist.h>

/**
 * 将块设备请求收集到分散/聚集链表中
 * 可以将返回的分散表sg传递给dma_map_sg。
 */
int blk_gather_bio(struct blk_request_queue *q,
	struct blk_request *rq, struct scatterlist *sg)
{
	struct block_io_item *items, *prev_bvec;
	struct block_io_desc *bio;
	int segment_count;
	int combine;
	int i;
	struct scatterlist *prev = NULL;

	segment_count = 0;
	combine = q->state & BLKQUEUE_COMBINED;

	prev_bvec = NULL;
	/**
	 * 遍历每一个bio
	 */
	for (bio = rq->bio_head; bio != NULL; bio = bio->bi_next) {
		/**
		 * 遍历BIO中的每一块
		 */
		bio_for_each_item(items, bio, i) {
			int nbytes = items->length;
			int combine_able = 0;

			/**
			 * 判断是否能与前一块合并
			 */
			if (prev_bvec && combine)
				combine_able = (prev->length + nbytes <= q->request_settings.max_size)
					&& (BIOVEC_PHYS_MERGEABLE(prev_bvec, items))
					&& (BIOVEC_SEG_BOUNDARY(q, prev_bvec, items));

			if (combine_able)
				prev->length += nbytes;
			else {
				prev = sg;
				sg_clear_end(sg);
				sg_set_page(sg, items->bv_page, nbytes, items->bv_offset);
				sg = sg_next(sg);
				segment_count++;
			}

			prev_bvec = items;
		}
	}

	if (prev)
		sg_mark_end(prev);

	return segment_count;
}
