#ifndef __DIM_SUM_BUG_H
#define __DIM_SUM_BUG_H

#include <asm/bug.h>
#include <linux/compiler.h>

#define BUILD_BUG_ON_ZERO(e) (sizeof(struct { int:-!!(e); }))
#define BUILD_BUG_ON_NULL(e) ((void *)sizeof(struct { int:-!!(e); }))

#define BUILD_BUG_ON_MSG(cond, msg) compiletime_assert(!(cond), msg)

#ifndef __OPTIMIZE__
#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))
#else
#define BUILD_BUG_ON(condition) \
	BUILD_BUG_ON_MSG(condition, "BUILD_BUG_ON failed: " #condition)
#endif

#define BUILD_BUG() BUILD_BUG_ON_MSG(1, "BUILD_BUG failed")

#endif	/* __DIM_SUM_BUG_H */
