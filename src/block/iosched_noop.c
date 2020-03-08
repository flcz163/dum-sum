#include <dim-sum/beehive.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/init.h>

static int noop_get_merge_pos(struct blk_request_queue *queue,
	struct blk_request **req, struct block_io_desc *bio)
{
	struct double_list *entry;
	struct blk_request *request;
	int ret;

	/**
	 * 快速路径，一般可以和上一个请求合并
	 */
	if ((ret = iosched_try_last_merge(queue, bio)) != IOSCHED_NO_MERGE) {
		*req = queue->preferred_merge;
		return ret;
	}

	/**
	 * 遍历队列中的所有请求
	 */
	list_for_each_prev(entry, &queue->requests) {
		request = TO_BLK_REQUEST(entry);

		/**
		 * 遇到IO屏障，或者已经开始的请求
		 * 不能合并，也不能继续向前搜索了
		 */
		if (request->flags & (BLKREQ_BARRIER | BLKREQ_STARTED))
			break;

		/**
		 * 不是数据传送请求，继续找下一个请求
		 */
		if (!(request->flags & BLKREQ_DATA))
			continue;

		/**
		 * 尝试与当前请求合并
		 */
		if ((ret = iosched_try_merge(request, bio))) {
			*req = request;
			/**
			 * 记录最后一次合并的请求
			 * 加快下次合并速度
			 */
			queue->preferred_merge = request;
			return ret;
		}
	}

	/**
	 * 不能合并，必须重新开始一个新请求
	 */
	return IOSCHED_NO_MERGE;
}

static void noop_merge(struct blk_request_queue *queue,
	struct blk_request *req, struct blk_request *next)
{
	list_del_init(&next->list);
}

static void noop_add_request(struct blk_request_queue *queue,
	struct blk_request *request, int where)
{
	if (where == IOSCHED_INSERT_HEAD)
		list_insert_front(&request->list, &queue->requests);
	else
		list_insert_behind(&request->list, &queue->requests);

	/**
	 * 讨厌的屏障，前面的所有请求都不能参与合并
	 */
	if (request->flags & BLKREQ_BARRIER)
		queue->preferred_merge = NULL;
	else if (!queue->preferred_merge)
		queue->preferred_merge = request;
}

static struct blk_request *
noop_get_first_request(struct blk_request_queue *queue)
{
	if (!list_is_empty(&queue->requests))
		return BLK_FIRST_REQUEST(&queue->requests);

	return NULL;
}

static struct ioscheduler_type iosched_noop = {
	.ops = {
		.get_merge_pos		= noop_get_merge_pos,
		.merge		= noop_merge,
		.add_request		= noop_add_request,
		.get_first_request		= noop_get_first_request,
	},
	.name = "noop",
};

int __init init_iosched_noop(void)
{
	return register_ioscheduler(&iosched_noop);
}

__maybe_unused void iosched_noop_exit(void)
{
	unregister_ioscheduler(&iosched_noop);
}
