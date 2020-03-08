#ifndef __DIM_SUM_JOURNAL_H
#define __DIM_SUM_JOURNAL_H

#include <dim-sum/beehive.h>
#include <dim-sum/block_buf.h>
#include <dim-sum/semaphore.h>
#include <dim-sum/smp_bit_lock.h>
#include <dim-sum/smp_lock.h>
#include <dim-sum/sched.h>
#include <dim-sum/wait.h>

typedef unsigned int trans_id_t;
struct blkbuf_desc;
struct transaction;
struct block_device;
struct file_node;
struct task_desc;
struct timer;
struct double_list;
struct beehive_allotter;
struct journal;

enum journal_state {
	/**
	 * 停止日志，关闭日志线程
	 */
	__JSTATE_UNMOUNT,
	/**
	 * 由于错误而终止
	 */
	__JSTATE_ABORT,
	/**
	 * 外部程序已经应答了日志中的错误，因此可以继续
	 */
	__JSTATE_ACK_ERR,
	/**
	 * 日志超级块已经写入到磁盘
	 */
	__JSTATE_FLUSHED,
	/**
	 * 已经被加载并恢复
	 */
	__JSTATE_LOADED,
	/**
	 * 磁盘支持IO屏障，这是加载文件系统时用户手工指定的标志
	 * 如果硬件不支持，那么就会去掉此标志
	 */
	__JSTATE_BARRIER,
};

#define JSTATE_UNMOUNT		(1UL << __JSTATE_UNMOUNT)
#define JSTATE_ABORT		(1UL << __JSTATE_ABORT)
#define JSTATE_ACK_ERR		(1UL << __JSTATE_ACK_ERR)
#define JSTATE_FLUSHED		(1UL << __JSTATE_FLUSHED)
#define JSTATE_LOADED		(1UL << __JSTATE_LOADED)
#define JSTATE_BARRIER		(1UL << __JSTATE_BARRIER)

enum transaction_state {
	/*
	 * 事务正在运行，可以接收新的原子操作。
	 */
	TRANS_RUNNING,
	/**
	 * 等待提交状态，不接收新的原子操作。
	 */
	TRANS_PREPARE_COMMIT,
	/**
	 * 将数据块回写到磁盘
	 */
	TRANS_SYNCDATA,
	/**
	 * 正在将事务的元数据提交到日志中。
	 */
	TRANS_COMMIT_METADATA,
	/**
	 * 事务已经完整的提交到日志中，并打上提交标记
	 */
	TRANS_FINISHED,
};

enum transaction_queue_type {
	/**
	 * 没有受到日志的管理
	 */
	TRANS_QUEUE_NONE,
	/**
	 * 缓冲区被保留，待日志访问
	 */
	TRANS_QUEUE_RESERVED,
	/**
	 * ORDERED模式下，需要监控的数据块
	 * 必须等待这些块写入后，才能写入元数据块
	 */
	TRANS_QUEUE_DIRTY_DATA,
	/**
	 * 在日志提交期间，被锁住以待IO
	 */
	TRANS_QUEUE_LOCKED_DATA,
	/**
	 * 缓冲区位于元数据队列
	 * 这些脏的元数据会被写入到日志
	 */
	TRANS_QUEUE_METADATA,
	/**
	 * 正准备写入日志的元数据块
	 * 这是保留其原始块缓冲区
	 */
	TRANS_QUEUE_META_ORIG,
	/**
	 * 正准备写入日志的元数据块
	 * 这是保存其转义后可以直接写入日志的缓冲区
	 */
	TRANS_QUEUE_META_LOG,
	/**
	 * 此队列包含几类块
	 *	1、手动撤销的块
	 *	2、已经提交日志的元数据块
	 *	3、从旧事务的检查点移动过来的块
	 * 一旦本事务成功成功提交，就可以不再关注的块
	 */
	TRANS_QUEUE_FORGET,
	/**
	 * 日志控制块队列，如撤销块、描述符块
	 */
	TRANS_QUEUE_CTRLDATA,
	/**
	 * 链表类型数
	 */
	TRANS_QUEUE_TYPES,
};

#define JFS_FEATURE_INCOMPAT_REVOKE	0x00000001

#define JFS_HAS_COMPAT_FEATURE(j,mask)					\
	((j)->format_version >= 2 &&					\
	 ((j)->super_block->feature_compat & cpu_to_be32((mask))))
#define JFS_HAS_RO_COMPAT_FEATURE(j,mask)				\
	((j)->format_version >= 2 &&					\
	 ((j)->super_block->feature_ro_compat & cpu_to_be32((mask))))
#define JFS_HAS_INCOMPAT_FEATURE(j,mask)				\
	((j)->format_version >= 2 &&					\
	 ((j)->super_block->feature_incompat & cpu_to_be32((mask))))

enum journal_block_type {
	/**
	 * 日志描述符块，包含后续块在文件系统中的块号
	 */
	JFS_DESCRIPTOR_BLOCK = 1,
	/**
	 * 提交块 
	 */
	JFS_COMMIT_BLOCK = 2,
	/**
	 * 日志超级块
	 */
	JFS_SUPER_BLOCK_V1 = 3,
	JFS_SUPER_BLOCK_V2 = 4,
	/**
	 * 撤销块
	 */
	JFS_REVOKE_BLOCK = 5,
};

#define JFS_MAGIC_NUMBER 0xc03b3998U
#define JFS_FEATURE_INCOMPAT_REVOKE	0x00000001
#define JFS_KNOWN_COMPAT_FEATURES	0
#define JFS_KNOWN_ROCOMPAT_FEATURES	0
#define JFS_KNOWN_INCOMPAT_FEATURES	JFS_FEATURE_INCOMPAT_REVOKE

enum journal_tag_flag {
	/**
	 * 磁盘上的日志数据数据被转义
	 * 回放的时候需要转回去
	 */
	__JTAG_FLAG_ESCAPE,
	/**
	 * 与上一个标签的UUID相同
	 */
	__JTAG_FLAG_SAME_UUID,
	/**
	 * 结束符，表示该标签是描述符块中最后一个标签
	 */
	__JTAG_FLAG_LAST_TAG,
};

/**
 * 日志描述符块里面，元数据标签的标志
 */
#define JTAG_FLAG_ESCAPE		(1UL << __JTAG_FLAG_ESCAPE)
#define JTAG_FLAG_SAME_UUID	(1UL << __JTAG_FLAG_SAME_UUID)
#define JTAG_FLAG_LAST_TAG		(1UL << __JTAG_FLAG_LAST_TAG)

/*
 * 默认的最大提交周期，5秒
 */
#define JBD_DEFAULT_MAX_COMMIT_AGE 5

/**
 * 默认的撤销表哈希桶数量
 */
#define JOURNAL_REVOKE_BUCKETS 256

/**
 * 与日志相关的，块缓冲区状态位
 */
enum journal_state_bits {
	/**
	 * 块缓冲区是否受到日志管理
	 */
	BS_JOURNALED = BS_PRIVATESTART,
	/**
	 * 表示相应的块正在写入日志
	 */
	BS_WRITE_JOURNAL,
	/**
	 * 由于被截断的原因，文件块被释放
	 */
	BS_FREED,
	/**
	 * 由于错误等原因，元数据块被撤销
	 */
	BS_REVOKED,
	/**
	 * 撤销标志 是否有效
	 */
	BS_REVOKED_VALID,
	/**
	 * 脏块，等待提交到日志，但是还不能写入文件系统
	 */
	BS_JOURNAL_DIRTY,
	/**
	 * 日志状态锁标志位
	 */
	BS_JLOCK_STATE,
	/**
	 * 日志信息锁标志位
	 * 保护日志的私有数据、块缓冲
	 */
	BS_JLOCK_INFO,
	/**
	 * 当元数据块还没有写入日志时，如果下一个事务想要写入元数据块
	 * 那么它必须等待上一个事务，在这个位上面等待
	 */
	BS_WAIT_LOGGED,
};

static inline int blkbuf_is_journaled(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_JOURNALED, &(blkbuf)->state);
}
static inline void blkbuf_set_journaled(struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_JOURNALED, &(blkbuf)->state);
}
static inline void blkbuf_clear_journaled(struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_JOURNALED, &(blkbuf)->state);
}

static inline int blkbuf_is_write_journal(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_WRITE_JOURNAL, &(blkbuf)->state);
}
static inline void blkbuf_set_write_journal(struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_WRITE_JOURNAL, &(blkbuf)->state);
}
static inline void blkbuf_clear_write_journal(struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_WRITE_JOURNAL, &(blkbuf)->state);
}

static inline int blkbuf_is_journal_dirty(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_JOURNAL_DIRTY, &(blkbuf)->state);
}
static inline void blkbuf_set_journal_dirty(struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_JOURNAL_DIRTY, &(blkbuf)->state);
}
static inline void blkbuf_clear_journal_dirty(struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_JOURNAL_DIRTY, &(blkbuf)->state);
}
static inline int blkbuf_test_set_journal_dirty(struct blkbuf_desc *blkbuf)
{
	return atomic_test_and_set_bit(BS_JOURNAL_DIRTY, &(blkbuf)->state);
}
static inline int blkbuf_test_clear_journal_dirty(struct blkbuf_desc *blkbuf)
{
	return atomic_test_and_clear_bit(BS_JOURNAL_DIRTY, &(blkbuf)->state);
}

static inline int blkbuf_is_revoked(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_REVOKED, &(blkbuf)->state);
}
static inline void blkbuf_set_revoked(struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_REVOKED, &(blkbuf)->state);
}
static inline void blkbuf_clear_revoked(struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_REVOKED, &(blkbuf)->state);
}
static inline int blkbuf_test_set_revoked(struct blkbuf_desc *blkbuf)
{
	return atomic_test_and_set_bit(BS_REVOKED, &(blkbuf)->state);
}
static inline int blkbuf_test_clear_revoked(struct blkbuf_desc *blkbuf)
{
	return atomic_test_and_clear_bit(BS_REVOKED, &(blkbuf)->state);
}

static inline int blkbuf_is_revokevalid(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_REVOKED_VALID, &(blkbuf)->state);
}
static inline void blkbuf_set_revokevalid(struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_REVOKED_VALID, &(blkbuf)->state);
}
static inline void clear_buffer_revokevalid(struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_REVOKED_VALID, &(blkbuf)->state);
}
static inline int blkbuf_test_set_revokevalid(struct blkbuf_desc *blkbuf)
{
	return atomic_test_and_set_bit(BS_REVOKED_VALID, &(blkbuf)->state);
}
static inline int blkbuf_test_clear_revokevalid(struct blkbuf_desc *blkbuf)
{
	return atomic_test_and_clear_bit(BS_REVOKED_VALID, &(blkbuf)->state);
}

static inline int blkbuf_is_freed(const struct blkbuf_desc *blkbuf)
{
	return test_bit(BS_FREED, &(blkbuf)->state);
}
static inline void blkbuf_set_freed(struct blkbuf_desc *blkbuf)
{
	atomic_set_bit(BS_FREED, &(blkbuf)->state);
}
static inline void blkbuf_clear_freed(struct blkbuf_desc *blkbuf)
{
	atomic_clear_bit(BS_FREED, &(blkbuf)->state);
}

static inline int blkbuf_trylock_state(struct blkbuf_desc *blkbuf)
{
	return smp_bit_trylock(BS_JLOCK_STATE, &blkbuf->state);
}
static inline void blkbuf_lock_state(struct blkbuf_desc *blkbuf)
{
	smp_bit_lock(BS_JLOCK_STATE, &blkbuf->state);
}
static inline void blkbuf_unlock_state(struct blkbuf_desc *blkbuf)
{
	smp_bit_unlock(BS_JLOCK_STATE, &blkbuf->state);
}
static inline int blkbuf_is_locked_state(struct blkbuf_desc *blkbuf)
{
	return smp_bit_is_locked(BS_JLOCK_STATE, &blkbuf->state);
}

/**
 * 日志操作句柄
 */
struct journal_handle 
{
	/**
	 * 引用计数
	 */
	int ref_count;
	/**
	 * 本原子操作属于哪个事务
	 */
	struct transaction *transaction;
	/**
	 * 线程可用的额度
	 * 也就是还可以用多少个磁盘块来保存日志
	 */
	int block_credits;
	/**
	 * 处理完毕后，立即提交并等待事务结束
	 */
	unsigned int sync;
	/**
	 * 遇到严重故障，必须停止日志
	 */
	unsigned int aborted;
	/**
	 * 错误号
	 */
	int error;
};

/**
 * 一个完整的事务，可以包含多个原子操作。
 */
struct transaction
{
	/**
	 * 保护原子操作的锁
	 */
	struct smp_lock lock;
	/**
	 * 本事务的ID
	 */
	trans_id_t trans_id;
	/**
	 * 事务当前的状态
	 */
	enum transaction_state state;
	/**
	 * 正在使用本事务的句柄数量
	 * 在提交前，需要等待其值为0，表示没有原子操作向它提交请求。
	 */
	int users;
	/**
	 * 使用本事务的句柄序号，递增不递减
	 */
	int users_sequence;
	/**
	 * 所属的日志
	 */
	struct journal *journal;
	/**
	 * 本事务从日志的哪一个磁盘块开始
	 * 从此磁盘块开始记录日志数据
	 */
	unsigned long start_block_num;
	/**
	 * 保留给日志操作使用的空间额度
	 * 即日志要顺利完成，所需要的块数量
	 */
	int			reserved_credits;
	/**
	 * 本事务中元数据缓冲区的个数
	 */
	int metadata_blocks;
	/**
	 * 事务的超时时间
	 * 当超过此时间时，即使事务中缓冲区较少，也会提交。
	 */
	unsigned long timeout;
	/**
	 * 被事务所保留，但是没有在事务中修改的缓冲区
	 * 在提交事务前被释放
	 */
	struct double_list reserved_list;
	/**
	 * 与当前事务相关的数据缓冲区。
	 * 在ordered模式下，应当首先将其写入磁盘，再写入元数据。
	 */
	struct double_list data_block_list;
	/**
	 * 文件系统还没有回写，因此需要日志系统回写的数据块
	 * 日志系统将其锁定，然后提交到文件系统中
	 */
	struct double_list locked_data_list;
	/**
	 * 所有修改过的元数据缓冲区的链表。
	 * 需要将其提交到日志中
	 */
	struct double_list metadata_list;
	/**
	 * 原始的元数据链表，没有经过转义
	 */
	struct double_list meta_orig_list;
	/**
	 * 当前正在等待IO写入的链表。经过转义可以直接写入日志。
	 * 与原始链表一一对应
	 */
	struct double_list meta_log_list;
	/**
	 * 等待写入日志的控制块链表。
	 * 如描述符块、撤销块
	 */
	struct double_list ctrldata_list;
	/**
	 * 一旦本事务被提交，就可以废弃的缓冲区。
	 * 包含事务中撤销的缓冲块，以及已经提交的元数据块
	 */
	struct double_list forget_list;
	/**
	 * 检查点链表，该链表中所有块写入后，才完成此事务的检查点
	 */
	struct double_list checkpoint_list;
	/**
	 * 通过此字段，将事务链接到日志的检查点事务链表
	 */
	struct double_list list_checkpoint;
};

/**
 * 日志描述符
 */
struct journal
{
	/**
	 * 日志的状态标志
	 * 如JSTATE_UNMOUNT
	 */
	unsigned long flags;
	/**
	 * 日志处理过程中的错误号
	 */
	int errno;
	/**
	 * 日志的超级块缓冲区
	 */
	struct blkbuf_desc	 *super_blkbuf;
	/**
	 * 超级块描述符，指向超级块缓冲区的内容
	 */
	struct journal_super_phy *super_block;
	/**
	 * 超级块支持的版本类型
	 */
	int format_version;
	/**
	 * 保护日志状态相关的变量
	 * 如当前日志编号
	 */
	struct smp_lock state_lock;
	/**
	 * 当前正在运行的事务。
	 * 如果为NULL，需要为新的原子操作创建新的事务。
	 * 这个事务正在接受新的原子操作请求。
	 */
	struct transaction *running_transaction;
	/**
	 * 当前正在提交的事务。
	 */
	struct transaction *committing_transaction;
	/**
	 * 最近*已经*成功提交的事务编号，但是还不一定执行了检查点
	 */
	trans_id_t committed_id;
	/**
	 * 正在提交的事务ID
	 */
	trans_id_t committing_id;
	/**
	 * 禁止并发的建立事务屏障
	 */
	struct semaphore	barrier_sem;
	/**
	 * 等待创建屏障的任务数
	 */
	int barrier_count;
	/**
	 * 需要进行检查点的事务链表头
	 */
	struct double_list checkpoint_transactions;
	/**
	 * 等待队列，其中的任务在等待开始一个新事务
	 */
	struct wait_queue	wait_new_trans;
	/**
	 * 等待队列，其中的任务正在等待日志的磁盘空间
	 */
	struct wait_queue	wait_logspace;
	/**
	 * 等待队列
	 * 线程正在此队列上等待日志被提交
	 * 或者是等待日志线程被正常启动
	 * 由日志线程唤醒队列上的等待线程
	 */
	struct wait_queue	wait_commit_done;
	/**
	 * 目前暂时未用
	 */
	struct wait_queue	wait_checkpoint;
	/**
	 * 等待队列
	 * 日志线程会在此队列上等待提交请求
	 */
	struct wait_queue	wait_commit;
	/**
	 * 等待队列
	 * 日志线程在此队列上等待现有日志句柄处理完毕
	 * 当结束日志句柄操作时唤醒此队列
	 */
	struct wait_queue wait_updates;
	/**
	 * 防止并发的执行检查点操作的互斥锁
	 */
	struct semaphore checkpoint_sem;
	/**
	 *  第一个未使用的日志块
	 */
	unsigned long free_block_num;
	/**
	 * 最后一个仍然在使用的日志块
	 */
	unsigned long inuse_block_first;
	/**
	 * 日志中剩余的空闲块，为0表示已经满了
	 */
	unsigned long free_blocks;
	/**
	 * 格式化时，确定的起始结束块号
	 */
	unsigned long first_block_log;
	unsigned long last_block_log;
	/**
	 * 日志块设备
	 */
	struct block_device *blkdev;
	/**
	 * 块大小
	 */
	int block_size;
	/**
	 * 日志在块设备中偏移量
	 */
	unsigned int block_start;
	/**
	 * 日志在磁盘中的最大容量
	 * 默认是磁盘的最后一个块号
	 * 但是在日志超级块中可能调整
	 */
	unsigned int block_end;
	/**
	 * 与日志绑定的文件系统，其所在的设备
	 */
	struct block_device *fs_blkdev;
	/**
	 * 保护缓冲区链表及缓冲区状态的锁
	 */
	struct smp_lock list_lock;
	/**
	 * 日志中最老的事务编号
	 */
	trans_id_t oldest_trans_id;
	/**
	 * 下一个事务的编号
	 */
	trans_id_t next_trans;
	/**
	 * 日志的UUID，防止误恢复文件系统
	 */
	__u8 uuid[16];
	/**
	 * 日志提交线程
	 */
	struct task_desc *demon_task;
	/**
	 * 一次允许提交的日志元数据缓冲区个数
	 */
	int max_block_in_trans;
	/**
	 * 当开始一个事务以后，间隔多久开始向磁盘提交
	 * 以jiffies为单位
	 */
	unsigned long commit_interval;
	/**
	 * 定期唤醒线程的定时器
	 */
	struct timer *commit_timer;
	/**
	 * 保护撤销块的锁
	 */
	struct smp_lock revoke_lock;
	/**
	 * 正在使用的撤销哈希表
	 */
	struct journal_revoke_table *cur_revoke_table;
	/**
	 * 两个撤销表，一个备用，一个正在用
	 */
	struct journal_revoke_table *revoke_tables[2];
	/**
	 * 临时数组，指向需要存储到日志中的数据块缓冲区描述符
	 */
	struct blkbuf_desc **blkbuf_bulk;
	/**
	 * 一个日志块中的标签数量
	 * 等于日志块大小除以块编号大小
	 * 每一个标签表示元数据块在原始文件系统中的位置
	 */
	int tags_in_block;
	/**
	 * 对LEXT3来说，指向其超级块
	 */
	void *private;
};

/**
 * 管理磁盘块的日志描述符
 * 与磁盘缓冲区描述符对应
 */
struct blkbuf_journal_info {
	/**
	 * 指向所管理的磁盘块
	 */
	struct blkbuf_desc *blkbuf;
	/**
	 * 引用计数
	 */
	int ref_count;
	/**
	 * 当前缓冲区位于日志的哪个链表中。如:
	 *	TRANS_QUEUE_NONE
	 */
	unsigned int which_list;
	/**
	 * 通过这个字段将缓冲区链接到事务的链表中
	 */
	struct double_list list_trans;
	/**
	 * 两种情况需要复制一份块缓冲区数据
	 *	1、数据需要转义
	 * 	2、上一个事务还在使用某个缓冲区，下一个事务则需要复制数据
	 */
	char *bufdata_copy;
	/**
	 * 针对undo操作来说，复制其原始缓冲块
	 * 例如LEXT3中的位图磁盘缓冲区
	 * 这里将其复制一份写到日志中
	 * 分配磁盘空间的时候，需要同时考虑内存中的位，以及此处的位
	 */
	char *undo_copy;
	/**
	 * 指向所属事务
	 * 要么是当前运行事务，要么是正在提交的任务
	 */
	struct transaction *transaction;
	/**
	 * 指向正在运行的事务，此事务希望修改此缓冲区
	 * 但是缓冲区已经在提交事务中
	 */
	struct transaction *next_trans;
	/**
	 * 当前缓冲区处于哪个事务的检查点链表中
	 */
	struct transaction *checkpoint_trans;
	/**
	 * 通过这个字段，将其链接到事务的检查点链表中
	 */
	struct double_list list_checkpoint;
};

/**
 * 撤销表
 */
struct journal_revoke_table
{
	/**
	 * 哈希桶的大小及其order值
	 */
	int hash_size;
	int hash_order;
	/**
	 * 哈希桶指针数组，动态分配
	 */
	struct hash_list_bucket *hash_table;
};

/**
 * 描述符块的头
 */
struct journal_header
{
	/**
	 * 描述符的魔术值，如:
	 *		JFS_MAGIC_NUMBER
	 */
	__be32 magic;
	/**
	 * 描述符块的类型，如:
	 *		JFS_DESCRIPTOR_BLOCK
	 */
	__be32 type;
	/**
	 * 所属事务序号
	 */
	__be32 trans_id;
};

/**
 * 日志超级块描述符
 */
struct journal_super_phy
{
/* 0x0000 */
	/**
	 * 块描述符头
	 */
	struct journal_header header;

/* 0x000C */
	/**
	 * 日志设备的块大小
	 */
	__be32 block_size;
	/**
	 * 日志总块数
	 */
	__be32 block_end;
	/**
	 * 日志块的第一个块号 
	 * 初始为1
	 * 其中0保留给超级块
	 */
	__be32 first_block_log;

/* 0x0018 */
	/**
	 * 最老的事务编号
	 */
	__be32 trans_id;
	/**
	 * 日志开始的块号，为0表示日志为空
	 */
	__be32 inuse_block_first;

/* 0x0020 */
	/**
	 * 错误编号
	 */
	__be32 errno;

/* 0x0024 */
	/**
	 * 功能兼容标志
	 */
	__be32 feature_compat;
	__be32 feature_incompat;
	__be32 feature_ro_compat;

/* 0x0030 */
	/**
	 * UUID
	 */
	__u8 uuid[16];

/* 0x0040 */
	/**
	 * 共享计数，未用
	 */
	__be32 users;

	/**
	 * 未用
	 */
	__be32 dynsuper;

/* 0x0048 */
	/**
	 * 每个事务中最大的日志块，未用
	 */
	__be32 max_transaction;
	/**
	 * 每个事务中最大的数据块，未用
	 */
	__be32 max_trans_data;

/* 0x0050 */
	__u32	padding[44];

/* 0x0100 */
	__u8	user_ids[16*48];
/* 0x0400 */
};

/** 
 * 日志块标签，表示后续日志块在文件系统中的位置
 */
struct journal_block_tag
{
	/**
	 * 数据在文件系统中的磁盘块号
	 */
	__be32		target_block_num;
	/**
	 * 标志，如JTAG_FLAG_ESCAPE
	 */
	__be32		flags;
	/**
	 * 如果没有JFS_FLAG_SAME_UUID标志 ，那么后接16字节的uuid
	 * 其值和日志的UUID一样
	 */
};

/**
 * 撤销块的描述符头
 */
struct journal_revoke_header
{
	/**
	 * 通用描述符头
	 */
	struct journal_header header;
	/**
	 * 占用字节数，含本结构
	 */
	__be32 size;
};


#define journal_oom_retry 1

/**
 * 日志操作句柄分配器
 */
extern struct beehive_allotter *journal_handle_allotter;

static inline int tid_gt(trans_id_t x, trans_id_t y)
{
	int diff = (x - y);

	return (diff > 0);
}

static inline int tid_geq(trans_id_t x, trans_id_t y)
{
	int diff = (x - y);

	return (diff >= 0);
}

extern int	 journal_errno(struct journal *);
extern struct journal * journal_alloc_dev(struct block_device *bdev,
	struct block_device *fs_dev, int start, int len, int bsize);

extern int	   journal_update_format (struct journal *);
extern int journal_set_revoke(struct journal *, unsigned long, trans_id_t);

extern int	 journal_init_revoke(struct journal *, int);
extern int	 init_journal_revoke(void);

extern void * __journal_kmalloc (size_t size, int flags, int retry);
#define jbd_kmalloc(size, flags) \
	__journal_kmalloc((size), (flags), journal_oom_retry)
#define jbd_rep_kmalloc(size, flags) \
	__journal_kmalloc((size), (flags), 1)

extern void journal_destroy(struct journal *);
extern int	   journal_clear_err(struct journal *);
extern void __journal_abort_hard(struct journal *);
extern void __journal_abort_soft(struct journal *, int);
extern int	 journal_recovery_ignore(struct journal *);
extern int	 journal_wipe(struct journal *, int);
extern int	 journal_map_block(struct journal *,
	unsigned long, unsigned long *);

extern void journal_update_superblock	(struct journal *, int);
extern int	 journal_engorge(struct journal *journal);
extern int	 journal_recover_engorge(struct journal *journal);
extern void journal_clear_revoke(struct journal *);
extern int	 journal_has_feature(struct journal *, unsigned long,
	unsigned long, unsigned long);
extern int journal_test_revoke(struct journal *, unsigned long, trans_id_t);
extern struct journal_handle *journal_start(struct journal *, int);
extern void journal_abort(struct journal *, int);
extern void journal_lock_updates(struct journal *);
extern void journal_unlock_updates(struct journal *);
extern int	 journal_flush(struct journal *);

static inline int journal_is_aborted(struct journal *journal)
{
	return journal->flags & JSTATE_ABORT;
}

static inline struct journal_handle *journal_current_handle(void)
{
	return current->journal_info;
}

static inline void
journal_set_current_handle(struct journal_handle *handle)
{
	current->journal_info = handle;
}

static inline int journal_needed_space(struct journal *journal)
{
	int block_count;

	block_count = journal->max_block_in_trans;
	if (journal->committing_transaction)
		block_count += journal->committing_transaction->reserved_credits;

	return block_count;
}

static inline struct blkbuf_journal_info *
blkbuf_to_journal(struct blkbuf_desc *blkbuf)
{
	return (struct blkbuf_journal_info *)blkbuf->private;
}

static inline struct blkbuf_desc *
journal_to_blkbuf(struct blkbuf_journal_info *blkbuf_jinfo)
{
	return blkbuf_jinfo->blkbuf;
}

extern int	 journal_get_write_access(struct journal_handle *,
	struct blkbuf_desc *, int *credits);
extern int	 journal_get_create_access (struct journal_handle *,
	struct blkbuf_desc *);
extern int	 journal_get_undo_access(struct journal_handle *,
 	struct blkbuf_desc *, int *credits);
extern int	journal_checkpoint_finish(struct journal *);
void __journal_takeout_checkpoint(struct blkbuf_journal_info *);
void journal_info_detach(struct blkbuf_desc *blkbuf);
void journal_info_loosen(struct blkbuf_journal_info *blkbuf_jinfo);

static inline int handle_is_aborted(struct journal_handle *handle)
{
	if (handle->aborted)
		return 1;

	return journal_is_aborted(handle->transaction->journal);
}

static inline void journal_abort_handle(struct journal_handle *handle)
{
	handle->aborted = 1;
}

extern int	 journal_dirty_metadata (struct journal_handle *,
	struct blkbuf_desc *);
extern void journal_putback_credits(struct journal_handle *,
 	struct blkbuf_desc *, int credits);
extern int	 journal_forget(struct journal_handle *, struct blkbuf_desc *);
extern int	 journal_dirty_data(struct journal_handle *, struct blkbuf_desc *);
extern int	 journal_extend(struct journal_handle *, int block_count);
int journal_restart(struct journal_handle *handle, int block_count);
extern int	 journal_stop(struct journal_handle *);
extern int	 journal_revoke(struct journal_handle *,
	unsigned long, struct blkbuf_desc *);
extern int	 journal_set_features(struct journal *,
	unsigned long, unsigned long, unsigned long);
extern void __journal_drop_transaction(struct journal *, struct transaction *);

int log_wait_commit(struct journal *journal, trans_id_t tid);
extern int journal_blocks_per_page(struct file_node *file_node);
extern int	 journal_invalidatepage(struct journal *,
	struct page_frame *, unsigned long);
extern int	 journal_release_page(struct journal *, struct page_frame *, int);
extern int	 journal_force_commit(struct journal *);
struct blkbuf_journal_info *
journal_info_hold_alloc(struct blkbuf_desc *blkbuf);
extern void __journal_putin_list(struct blkbuf_journal_info *,
	struct transaction *, int);
static inline void journal_info_lock(struct blkbuf_desc *blkbuf)
{
	smp_bit_lock(BS_JLOCK_INFO, &blkbuf->state);
}

static inline void journal_info_unlock(struct blkbuf_desc *blkbuf)
{
	smp_bit_unlock(BS_JLOCK_INFO, &blkbuf->state);
}

extern void
journal_cancel_revoke(struct journal_handle *, struct blkbuf_journal_info *);

#if 0
#define journal_debug(n, f, a...)						\
	do {								\
		if ((n) <= 7) {			\
			printk ("(%s, %d): %s: ",		\
				__FILE__, __LINE__, __FUNCTION__);	\
		  	printk (f, ## a);				\
		}							\
	} while (0)
#else
#define journal_debug(f, a...)	
#endif

struct blkbuf_journal_info *hold_blkbuf_jinfo(struct blkbuf_desc *blkbuf);
extern void journal_commit_transaction(struct journal *);
extern void __journal_putin_next_list(struct blkbuf_journal_info *);
extern void journal_putin_next_list(struct journal *,
	struct blkbuf_journal_info *);
int __journal_clean_checkpoint_list(struct journal *journal);
void __journal_takeout_checkpoint(struct blkbuf_journal_info *);
void __journal_insert_checkpoint(struct blkbuf_journal_info *,
	struct transaction *);
extern void journal_switch_revoke_table(struct journal *journal);
extern void journal_revoke_write(struct journal *, struct transaction *);
extern struct blkbuf_journal_info * journal_alloc_block(struct journal *);
int journal_next_log_block(struct journal *, unsigned long *);
extern int journal_prepare_metadata_block(struct transaction *,
	struct blkbuf_journal_info *, struct blkbuf_journal_info **,
	int blocknr);
extern void journal_takeout_list(struct journal *,
	struct blkbuf_journal_info *);
extern void __journal_takeout_list(struct blkbuf_journal_info *);
int journal_start_commit(struct journal *journal, trans_id_t *tid);
int journal_force_commit_nested(struct journal *journal);
extern int	 journal_check_used_features(struct journal *,
	unsigned long, unsigned long, unsigned long);
extern void journal_putin_list(struct blkbuf_journal_info *,
	struct transaction *, int);
extern void journal_loosen_blkbuf_bulk(struct blkbuf_desc *[], int);

#endif /* __DIM_SUM_JOURNAL_H */
