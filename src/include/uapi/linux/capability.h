#ifndef _UAPI_LINUX_CAPABILITY_H
#define _UAPI_LINUX_CAPABILITY_H

#include <linux/compiler.h>

typedef struct __user_cap_header_struct {
	__u32 version;
	int pid;
} __user *cap_user_header_t;

typedef struct __user_cap_data_struct {
        __u32 effective;
        __u32 permitted;
        __u32 inheritable;
} __user *cap_user_data_t;

#endif /* _UAPI_LINUX_CAPABILITY_H */