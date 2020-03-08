#ifndef __DIM_SUM_IDR_H
#define __DIM_SUM_IDR_H

#include <dim-sum/mutex.h>
#include <dim-sum/smp_lock.h>

#define MAX_ID_SHIFT (sizeof(int)*8 - 1)
#define MAX_ID_BIT (1U << MAX_ID_SHIFT)
#define MAX_ID_MASK (MAX_ID_BIT - 1)

struct idr {
	int next_idr;
	int max_idr;
	struct smp_lock	  lock;
};


#define IDR_INIT(name, max)						\
{								\
	.next_idr	= 0,					\
	.max_idr		= max,					\
	.lock		= SMP_LOCK_UNLOCKED((name).lock),			\
}
#define DEFINE_IDR(name, max)	struct idr name = IDR_INIT(name, max)

void *idr_find(struct idr *idp, int id);
int idr_pre_get(struct idr *idp, unsigned paf_mask);
int idr_get_new(struct idr *idp, void *ptr, int *id);
int idr_get_new_above(struct idr *idp, void *ptr, int starting_id, int *id);
void idr_remove(struct idr *idp, int id);
void idr_init(struct idr *idp, int max);

/**
 * 编号描述符
 */
struct idr_space {
	/**
	 * 保护本结构的锁
	 */
	struct mutex  mutex;
	/**
	 * 未占用的位图编号数量
	 */
	u32		  unused_bits;
	/**
	 * 位图及其长度
	 */
	u32		  bitmap_len;
	unsigned long	  *bits;	
};

#define NUMSPACE_INITIALIZER(name)	\
{										\
	.unused_bits = 0,						\
	.bitmap_len = 0,							\
	.bits = NULL,							\
	.mutex = MUTEX_INITIALIZER(name.mutex),	\
}

int get_idr_number(struct idr_space *s);
void put_idr_number(struct idr_space *s, int number);

#endif /* __DIM_SUM_IDR_H */
