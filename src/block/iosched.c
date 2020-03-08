#include <dim-sum/beehive.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/blkio.h>

static struct smp_lock ioscheduler_list_lock =
	SMP_LOCK_UNLOCKED(ioscheduler_list_lock);
static struct double_list ioscheduler_list =
	LIST_HEAD_INITIALIZER(ioscheduler_list);


/**
 * 在队列中查找，找到请求的合并位置
 */
int iosched_get_merge_pos(struct blk_request_queue *queue, struct blk_request **req, struct block_io_desc *bio)
{
	struct ioscheduler *sched = queue->scheduler;

	if (sched->ops->get_merge_pos)
		return sched->ops->get_merge_pos(queue, req, bio);

	return IOSCHED_NO_MERGE;
}

/**
 * 当一个IO请求完成时调用
 */
void iosched_request_release(struct blk_request_queue *queue,
		struct blk_request *request)
{
	struct ioscheduler *sched = queue->scheduler;

	/**
	 * 已经启动传输的读写请求
	 */
	if ((request->flags & BLKREQ_DATA) && (request->flags & BLKREQ_STARTED))
		queue->running_count--;

	/**
	 * 调用调度器的回调
	 */
	if (sched->ops->release_request)
		sched->ops->release_request(queue, request);
}

/**
 * 对请求进行反初始化
 */
void iosched_uninit_request(struct blk_request_queue *queue, struct blk_request *request)
{
	struct ioscheduler *sched = queue->scheduler;

	if (sched->ops->uninit_request)
		sched->ops->uninit_request(queue, request);

	list_del_init(&request->list);
}

/**
 * 向队列中添加一个请求
 */
void iosched_add_request(struct blk_request_queue *queue,
			struct blk_request *request, int where)
{
	request->queue = queue;

	/**
	 * 正在切换调度器
	 */
	if (queue->state & BLKQUEUE_DRAINING) {
		/**
		 * 为了避免饥饿，此时不再接收新请求
		 * 将请求放到延迟链表缓存起来
		 */
		list_insert_behind(&request->list, &queue->delayed_list);
		return;
	}

	/**
	 * IO屏障不能参与排序，应当放到后面
	 */
	if ((request->flags & BLKREQ_BARRIER) && (where == IOSCHED_INSERT_ORDERED))
		where = IOSCHED_INSERT_TAIL;

	/**
	 * 对noop算法来说，回调函数是noop_add_request，直接插入链表即可
	 */
	queue->scheduler->ops->add_request(queue, request, where);

	/**
	 * 队列与设备绑定了
	 */
	if (queue->state & BLKQUEUE_ATTACHED) {
		int nrq = queue->request_pools[READ].request_count
				+ queue->request_pools[WRITE].request_count
				- queue->running_count;

		/**
		 * 设备忙，强制将请求推送到设备
		 */
		if (nrq >= queue->busy_thresh)
			__blk_generic_push_queue(queue);
	}
}

/**
 * 从队列中移除一个请求
 */
void iosched_remove_request(struct blk_request_queue *queue,
	struct blk_request *request)
{
	/**
	 * 请求不在队列中
	 */
	BUG_ON(list_is_empty(&request->list));
	/**
	 * 从队列链表中摘除请求
	 */
	list_del_init(&request->list);

	/**
	 * 请求队列需要管理自己的内存池
	 */
	if (request->queue) {
		struct ioscheduler *sched = queue->scheduler;

		/**
		 * 被驱动接收的IO请求
		 */
		if ((request->flags & BLKREQ_DATA) && (request->flags & BLKREQ_STARTED))
			queue->running_count++;

		/**
		 * 不可能再参与合并了
		 */
		if (request == queue->preferred_merge)
			queue->preferred_merge = NULL;

		/**
		 * 调用调度器的回调，进行移除操作
		 */
		if (sched->ops->remove_request)
			sched->ops->remove_request(queue, request);
	}
}

/**
 * 检查请求队列中是否存在待处理请求。
 */
int iosched_is_empty(struct blk_request_queue *queue)
{
	struct ioscheduler *sched = queue->scheduler;

	if (sched->ops->is_empty)
		return sched->ops->is_empty(queue);

	return list_is_empty(&queue->requests);
}

/**
 * 合并两个请求
 */
void iosched_merge_requests(struct blk_request_queue *queue, struct blk_request *request,
			     struct blk_request *next)
{
	struct ioscheduler *sched = queue->scheduler;

	/**
	 * 新请求被合并后，可能已经不是独立的请求
	 */
	if (queue->preferred_merge == next)
		queue->preferred_merge = NULL;

	if (sched->ops->merge)
		sched->ops->merge(queue, request, next);
}

/**
 * 合并请求后，做一些收尾工作
 */
void iosched_merge_post(struct blk_request_queue *queue, struct blk_request *request)
{
	struct ioscheduler *sched = queue->scheduler;

	if (sched->ops->merge_post)
		sched->ops->merge_post(queue, request);
}

struct blk_request *iosched_get_front_request(struct blk_request_queue *queue, struct blk_request *request)
{
	struct double_list *prev;

	struct ioscheduler *sched = queue->scheduler;

	if (sched->ops->get_front_request)
		return sched->ops->get_front_request(queue, request);

	prev = request->list.prev;
	if (!list_is_empty(&request->list) && prev != &queue->requests)
		return TO_BLK_REQUEST(prev);

	return NULL;
}

struct blk_request *iosched_get_behind_request(struct blk_request_queue *queue, struct blk_request *request)
{
	struct double_list *next;

	struct ioscheduler *sched = queue->scheduler;

	if (sched->ops->get_behind_request)
		return sched->ops->get_behind_request(queue, request);

	next = request->list.next;
	if (!list_is_empty(&request->list) && next != &queue->requests)
		return TO_BLK_REQUEST(next);

	return NULL;
}

int iosched_may_queue(struct blk_request_queue *queue, bool write)
{
	struct ioscheduler *sched = queue->scheduler;

	if (sched->ops->may_queue)
		return sched->ops->may_queue(queue, write);

	return IOSCHED_MAY_QUEUE;
}

int iosched_init_request(struct blk_request_queue *queue,
	struct blk_request *request, int alloc_flags)
{
	struct ioscheduler *sched = queue->scheduler;

	request->queue = queue;
	request->sched_data = NULL;

	if (sched->ops->init_request)
		return sched->ops->init_request(queue, request, alloc_flags);

	return 0;
}

/**
 * 返回请求队列中下一个要处理的请求。
 * 它依然将请求保存在队列中，但是为其做了活动标记。该标记是防止IO调度器将其与其他请求合并。
 */
struct blk_request *iosched_get_first_request(struct blk_request_queue *queue)
{
	struct ioscheduler *sched = queue->scheduler;
	struct blk_request *request;

	request = sched->ops->get_first_request(queue);
	/**
	 * 从请求队列中取出一个请求进行处理
	 */
	if (request != NULL) {
		request->flags |= BLKREQ_STARTED;

		/**
		 * 当前请求是队列中的请求边界(IO屏障) 
		 */
		if (request == queue->preferred_merge)
			queue->preferred_merge = NULL;
	}

	return request;
}

/**
 * 判断BIO是否可以与请求合并
 */
int iosched_can_merge(struct blk_request *request, struct block_io_desc *bio)
{
	if (request->flags & (BLKREQ_NOMERGE | BLKREQ_STARTED | BLKREQ_BARRIER))
		return 0;

	if (!(request->flags & BLKREQ_DATA))
		return 0;

	if (bio_is_write(bio) != blk_request_is_write(request))
		return 0;

	if (request->disk != bio->bi_bdev->disk)
		return 0;

	if (request->waiting || request->special)
		return 0;

	return 1;
}

/**
 * 试图将请求与BIO合并
 */
int iosched_try_merge(struct blk_request *request, struct block_io_desc *bio)
{
	int ret = IOSCHED_NO_MERGE;

	if (iosched_can_merge(request, bio)) {
		if (request->start_sector + request->sector_count == bio->start_sector)
			ret = IOSCHED_BACK_MERGE;
		else if (request->start_sector - bio_sectors(bio) == bio->start_sector)
			ret = IOSCHED_FRONT_MERGE;
	}

	return ret;
}

/**
 * 试图将队列中最后一次的请求合并
 */
inline int iosched_try_last_merge(struct blk_request_queue *queue, struct block_io_desc *bio)
{
	if (queue->preferred_merge)
		return iosched_try_merge(queue->preferred_merge, bio);

	return IOSCHED_NO_MERGE;
}

int register_iosched_queue(struct blk_request_queue *queue)
{
	hold_object(&queue->object);

	return 0;
}

void unregister_iosched_queue(struct blk_request_queue *queue)
{
	if (queue)
		loosen_object(&queue->object);
}

/**
 * 默认的IO调度器名称
 */
static char default_ioscheduler_name[16];

static struct ioscheduler_type *__lookup_ioscheduler(const char *name)
{
	struct ioscheduler_type *sched = NULL;
	struct double_list *entry;

	list_for_each(entry, &ioscheduler_list) {
		struct ioscheduler_type *tmp;

		tmp = list_container(entry, struct ioscheduler_type, list);
		if (!strcmp(tmp->name, name)) {
			sched = tmp;
			break;
		}
	}

	return sched;
}

static struct ioscheduler_type *lookup_ioscheduler(const char *name)
{
	struct ioscheduler_type *sched;

	smp_lock_irq(&ioscheduler_list_lock);
	sched = __lookup_ioscheduler(name);
	smp_unlock_irq(&ioscheduler_list_lock);

	return sched;
}

int register_ioscheduler(struct ioscheduler_type *sched)
{
	smp_lock_irq(&ioscheduler_list_lock);
	if (__lookup_ioscheduler(sched->name))
		BUG();
	list_insert_behind(&sched->list, &ioscheduler_list);
	smp_unlock_irq(&ioscheduler_list_lock);

	printk(KERN_INFO "io scheduler %s registered", sched->name);
	if (!strcmp(sched->name, default_ioscheduler_name))
		printk("(default)");
	printk("\n");

	return 0;
}

void unregister_ioscheduler(struct ioscheduler_type *sched)
{
	smp_lock_irq(&ioscheduler_list_lock);
	list_del_init(&sched->list);
	smp_unlock_irq(&ioscheduler_list_lock);
}

/**
 * 初始化请求队列的调度器
 */
int iosched_init(struct blk_request_queue *queue, char *name)
{
	struct ioscheduler_type *type = NULL;
	struct ioscheduler *sched;
	int ret = 0;

	if (!name)
		name = default_ioscheduler_name;

	type = lookup_ioscheduler(name);
	if (!type)
		return -EINVAL;

	/**
	 * 为队列分配调度器对象
	 */
	sched = kmalloc(sizeof(struct ioscheduler), PAF_KERNEL | __PAF_ZERO);
	if (!sched)
		return -ENOMEM;

	/**
	 * 初始化调度器
	 */
	sched->ops = &type->ops;
	sched->type = type;
	if (sched->ops->init)
		ret = sched->ops->init(queue, sched);
	if (ret)
		kfree(sched);
	else {
		queue->scheduler = sched;
		list_init(&queue->requests);
		queue->preferred_merge = NULL;
	}

	return ret;
}

void init_iosched(void)
{
	strcpy(default_ioscheduler_name, "noop");
}

void iosched_exit(struct ioscheduler *sched)
{
	if (sched->ops->uninit)
		sched->ops->uninit(sched);
	sched->ops = NULL;
	sched->type = NULL;
	kfree(sched);
}
