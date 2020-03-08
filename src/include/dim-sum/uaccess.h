#ifndef __DIM_SUM_UACCESS_H
#define __DIM_SUM_UACCESS_H

unsigned long
copy_from_user(void *to, const void *from, unsigned long n);
unsigned long
copy_to_user(void *to, const void *from, unsigned long n);

#define __get_user(x, ptr) \
	({			\
		typeof(*(ptr)) val = x;	\
		copy_from_user((void *)&val, ptr, sizeof(val)); \
		x;			\
	})
	
#define __put_user(x, ptr) \
	({			\
		typeof(*(ptr)) val = x;	\
		copy_to_user(ptr, &val, sizeof(val)); \
	})

#define access_ok(type,addr,size) 1

#define put_user(x, ptr)						\
({									\
	__typeof__(*(ptr)) __user *__p = (ptr);				\
	__put_user((x), __p);					\
})

int verify_area(int type, const void * addr, unsigned long size);

/**
 * 获得用户传入的文件名参数。
 * 注意:纯内核态应用不需要额外的复制过程
 */
static inline int clone_user_string(const char * filename, char **result)
{
	//内核态中，不需要考虑文件名复制
	*result = (char *)filename;
	return 0;
}

/**
 * 释放内核文件名参数。
 * 注意:纯内核态应用不需要额外的释放过程
 */
static inline void discard_user_string(const char * name)
{
}

#endif /* __DIM_SUM_UACCESS_H */