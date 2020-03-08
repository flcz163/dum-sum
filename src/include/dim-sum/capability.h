#ifndef _DIM_SUM_CAPABILITY_H
#define _DIM_SUM_CAPABILITY_H

#include <dim-sum/types.h>
#include <linux/compiler.h>
#include <uapi/linux/capability.h>

/**
 * 忽略对文件和组的拥有者进行改变的限制
 */
#define CAP_CHOWN            0
/**
 * 一般是忽略对文件拥有者的权限检查
 */
#define CAP_FOWNER           3
/**
 * 忽略对文件setid和setgid标志设置的限制
 */
#define CAP_FSETID           4

/**
 * 允许一般的系统管理
 */
#define CAP_SYS_ADMIN        21

#endif /* _DIM_SUM_CAPABILITY_H */
