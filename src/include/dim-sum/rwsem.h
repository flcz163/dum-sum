#ifndef __DIM_SUM_RWSEM_H
#define __DIM_SUM_RWSEM_H

#include <dim-sum/accurate_counter.h>
#include <dim-sum/smp_lock.h>

/**
 * 读写信号量描述符
 */
struct rw_semaphore {
	/**
	 * >0 读者数量
	 * =0 无读者写者
	 * -1 一个写者
	 */
	int count;
	/**
	 * 保护此结构的锁
	 */
	struct smp_lock wait_lock;
	/**
	 * 等待信号的的读者或者写者
	 */
	struct double_list wait_list;
};

#define RWSEM_INITIALIZER(name) \
	{				\
		.wait_lock = SMP_LOCK_UNLOCKED((name).wait_lock),		\
		.count = 0,													\
		.wait_list = LIST_HEAD_INITIALIZER((name).wait_list)				\
	}


extern void init_rwsem(struct rw_semaphore *sem);
extern void __down_read(struct rw_semaphore *sem);
extern int __down_read_trylock(struct rw_semaphore *sem);
extern void __down_write(struct rw_semaphore *sem);
extern int __down_write_trylock(struct rw_semaphore *sem);
extern void __up_read(struct rw_semaphore *sem);
extern void __up_write(struct rw_semaphore *sem);
extern void __downgrade_write(struct rw_semaphore *sem);

static inline void down_read(struct rw_semaphore *sem)
{
	__down_read(sem);
}

static inline int down_read_trylock(struct rw_semaphore *sem)
{
	int ret;
	ret = __down_read_trylock(sem);
	return ret;
}

static inline void down_write(struct rw_semaphore *sem)
{
	__down_write(sem);
}

static inline int down_write_trylock(struct rw_semaphore *sem)
{
	int ret;
	ret = __down_write_trylock(sem);
	return ret;
}

static inline void up_read(struct rw_semaphore *sem)
{
	__up_read(sem);
}

static inline void up_write(struct rw_semaphore *sem)
{
	__up_write(sem);
}

static inline void downgrade_write(struct rw_semaphore *sem)
{
	__downgrade_write(sem);
}

#endif /* __DIM_SUM_RWSEM_H */
