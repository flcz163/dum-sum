#ifndef _ASM_GENERIC_BUG_H
#define _ASM_GENERIC_BUG_H

#include <linux/compiler.h>

#ifndef __ASSEMBLY__
#include <dim-sum/kernel.h>

#ifndef HAVE_ARCH_BUG
#define BUG() do { \
	printk("BUG: failure at %s:%d/%s()!\n", __FILE__, __LINE__, __func__); \
	panic("BUG!"); \
} while (0)
#endif

#define ASSERT(assert)						\
do {									\
	if (!(assert)) {						\
		printk (KERN_EMERG					\
			"Assertion failure in %s() at %s:%d: \"%s\"\n",	\
			__FUNCTION__, __FILE__, __LINE__, # assert);	\
		BUG();							\
	}								\
} while (0)

#ifndef HAVE_ARCH_BUG_ON
#define BUG_ON(condition) do { if (unlikely(condition)) BUG(); } while(0)
#endif

extern void print_warning(const char *file, const int line);
#define __WARN()		print_warning(__FILE__, __LINE__)

#ifndef WARN_ON
#define WARN_ON(condition, format...) ({						\
	int __ret_warn_on = !!(condition);				\
	if (unlikely(__ret_warn_on))					\
		__WARN();						\
	unlikely(__ret_warn_on);					\
})
#endif

#ifndef WARN
#define WARN(format...) ({						\
		__WARN();					\
})
#endif

#define WARN_ONCE(condition, format...)	({			\
	static bool __section(.data.unlikely) __warned = false;		\
	int __ret_warn_once = !!(condition);			\
	if (unlikely(__ret_warn_once))				\
		if (WARN_ON(!__warned, format) != 0) 			\
			__warned = true;			\
	unlikely(__ret_warn_once);				\
})

#endif /* __ASSEMBLY__ */

#endif
