#ifndef __ASM_GENERIC_CURRENT_H
#define __ASM_GENERIC_CURRENT_H

#include <dim-sum/process.h>

#define get_current() (current_proc_info()->task)
#define current get_current()

#endif /* __ASM_GENERIC_CURRENT_H */
