#ifndef __DIM_SUM_SMP_SEQ_LOCK_H
#define __DIM_SUM_SMP_SEQ_LOCK_H

#include <dim-sum/smp_lock.h>
#include <dim-sum/preempt.h>

/**
 * 顺序锁描述符。
 */
struct smp_seq_lock {
	/**
	 * 顺序计数器。每个读者需要在读数据前后两次读顺序计数器。
	 * 只有在这个值没有变化时，才说明读取到的数据是有效的。
	 */
	unsigned sequence;
	/**
	 * 保护结构的自旋锁。
	 */
	struct smp_lock lock;
};

#define SMP_SEQ_LOCK_UNLOCKED(lockname)			\
	{						\
		.sequence = 0,	\
		.lock = SMP_LOCK_UNLOCKED(lockname)	\
	}

static inline void smp_seq_init(struct smp_seq_lock *lock)
{
	lock->sequence = 0;
	smp_lock_init(&lock->lock);
}

/**
 * 为写获得顺序锁。
 */
void smp_seq_write_lock(struct smp_seq_lock *lock);

/**
 * 释放写顺序锁
 */
void smp_seq_write_unlock(struct smp_seq_lock *lock);

int smp_seq_write_trylock(struct smp_seq_lock *lock);

/**
 * 它返回当前顺序号。
 */
static inline unsigned smp_seq_read_begin(const struct smp_seq_lock *lock)
{
	unsigned ret;

	ret = lock->sequence;
	smp_rmb();

	return ret;
}

/**
 * 判断是否有写者改变了顺序锁
 */
static inline int smp_seq_read_retry(const struct smp_seq_lock *lock, unsigned iv)
{
	smp_rmb();
	/**
	 * iv为奇数，说明在读者调用读锁后，有写者更新了数据结构。
	 * 写者开始后，iv一定是奇数。直到写者解锁才会变成偶数。
	 * lock->sequence ^ iv是判断read_seqbegin的值是否发生了变化。
	 */
	return (iv & 1) | (lock->sequence ^ iv);
}

#endif /* __DIM_SUM_SMP_SEQ_LOCK_H */
