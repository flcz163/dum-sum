#ifndef __DIM_SUM_IOSCHED_H
#define __DIM_SUM_IOSCHED_H

struct ioscheduler;
struct blk_request_queue;
struct blk_request;
struct block_io_desc;
	
/**
 * get_merge_pos函数的返回值
 * 该函数检查新的BIO请求是否可以并入已经存在的请求中。
 */
enum iosched_merge_pos {
	/**
	 * 已经存在的请求中不能包含BIO结构。
	 */
	IOSCHED_NO_MERGE,
	/**
	 * BIO结构可以作为末尾的BIO而插入到某个请求中。这时，可能还检查是否与下一个请求合并。
	 */
	IOSCHED_FRONT_MERGE,
	/**
	 * BIO结构可以作为某个请求的第一个BIO被插入。这时需要检查是否能够与前一个请求合并。
	 */
	IOSCHED_BACK_MERGE,
};

/* 电梯算法的回调函数 */
struct ioscheduler_ops
{
	/**
	 * 查找可以和bio进行合并的请求
	 * 返回如ELEVATOR_NO_MERGE
	 */
	int (*get_merge_pos) (struct blk_request_queue *,
			struct blk_request **, struct block_io_desc *);
	/**
	 * 合并磁盘请求
	 */
	void (*merge) (struct blk_request_queue *,
			struct blk_request *, struct blk_request *);
	/**
	 * 磁盘请求被合并后调用。
	 * 可以做一些善后工作
	 */
	void (*merge_post) (struct blk_request_queue *, struct blk_request *);
	/**
	 * 获得队列中下一个磁盘请求
	 */
	struct blk_request *(*get_first_request) (struct blk_request_queue *);

	/**
	 * 往调度器中添加请求时调用
	 */
	void (*add_request) (struct blk_request_queue *,
			struct blk_request *, int);
	/**
	 * 如果调用器自己管理队列，则调用此回调
	 * 从调度器中移除请求时调用
	 */
	void (*remove_request) (struct blk_request_queue *, struct blk_request *);
	/**
	 * 判断队列是否为空
	 */
	int (*is_empty) (struct blk_request_queue *);

	/* 请求被完成时调用 */
	void (*release_request) (struct blk_request_queue *, struct blk_request *);
	/**
	 * 查找前一个请求
	 */
	struct blk_request *(*get_front_request) (struct blk_request_queue *, struct blk_request *);
	/**
	 * 查找后一个请求
	 */
	struct blk_request *(*get_behind_request) (struct blk_request_queue *, struct blk_request *);
	/**
	 * 被某些算法用于为请求分配存储空间
	 */
	int (*init_request) (struct blk_request_queue *, struct blk_request *, int);
	/**
	 * 被某些算法用于为请求释放存储空间
	 */
	void (*uninit_request) (struct blk_request_queue *, struct blk_request *);
	/**
	 * 初始化函数，为算法分配特定的内存
	 */
	int (*init) (struct blk_request_queue *, struct ioscheduler *);
	/**
	 * 释放函数，释放特定的内存
	 */
	void (*uninit) (struct ioscheduler *);
	/**
	 * 新请求是否可以入队
	 */
	int (*may_queue) (struct blk_request_queue *, bool);
};

#define IOSCHED_MAX_NAME	(16)
/**
 * IO调度算法描述符
 */
struct ioscheduler_type
{
	/* 算法名称*/
	char name[IOSCHED_MAX_NAME];
	/**
	 * 通过此字段加入到全局链表中
	 */
	struct double_list list;
	/**
	 * 电梯算法的回调函数
	 */
	struct ioscheduler_ops ops;
};

/**
 * 磁盘IO调度队列
 */
struct ioscheduler
{
	/**
	 * 调度器操作回调
	 */
	struct ioscheduler_ops *ops;
	/**
	 * 调度队列私有数据
	 * 如最后期限调度算法是deadline_data
	 */
	void *elevator_data;
	/**
	 * 算法类型
	 */
	struct ioscheduler_type *type;
};

extern void iosched_request_release(struct blk_request_queue *, struct blk_request *);
extern void iosched_uninit_request(struct blk_request_queue *, struct blk_request *);
extern void iosched_exit(struct ioscheduler *);
extern int iosched_is_empty(struct blk_request_queue *);
extern int iosched_get_merge_pos(struct blk_request_queue *, struct blk_request **, struct block_io_desc *);
extern struct blk_request *iosched_get_front_request(struct blk_request_queue *, struct blk_request *);
extern struct blk_request *iosched_get_behind_request(struct blk_request_queue *, struct blk_request *);
extern void iosched_merge_requests(struct blk_request_queue *, struct blk_request *,
			       struct blk_request *);
extern void iosched_merge_post(struct blk_request_queue *, struct blk_request *);
extern struct blk_request *iosched_get_first_request(struct blk_request_queue *queue);
extern void iosched_remove_request(struct blk_request_queue *queue,
	struct blk_request *request);

enum {
	IOSCHED_MAY_QUEUE,
	IOSCHED_NO_QUEUE,
	IOSCHED_MUST_QUEUE,
};
extern int iosched_may_queue(struct blk_request_queue *, bool);
extern int iosched_init_request(struct blk_request_queue *, struct blk_request *, int);
enum {
	/**
	 * 将请求添加到队列头部
	 */
	IOSCHED_INSERT_HEAD,
	/**
	 * 将请求添加到队列尾部
	 */
	IOSCHED_INSERT_TAIL,
	/**
	 * 按顺序将请求添加到队列合适的位置
	 */
	IOSCHED_INSERT_ORDERED,
};
extern void iosched_add_request(struct blk_request_queue *, struct blk_request *, int);
/**
 * 一般地，本函数用于中断处理函数将请求从请求队列中删除。
 */
extern void iosched_remove_request(struct blk_request_queue *queue,
	struct blk_request *request);
extern int iosched_try_last_merge(struct blk_request_queue *, struct block_io_desc *);
extern int iosched_can_merge(struct blk_request *, struct block_io_desc *);
extern int iosched_try_merge(struct blk_request *, struct block_io_desc *);

extern int register_ioscheduler(struct ioscheduler_type *);
extern void unregister_ioscheduler(struct ioscheduler_type *);
int register_iosched_queue(struct blk_request_queue *q);
void unregister_iosched_queue(struct blk_request_queue *q);
extern int iosched_init(struct blk_request_queue *, char *);

__maybe_unused void iosched_noop_exit(void);
void init_iosched(void);
#endif /* __DIM_SUM_IOSCHED_H */
