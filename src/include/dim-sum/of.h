#ifndef __DIM_SUM_OF_H
#define __DIM_SUM_OF_H

#include <dim-sum/types.h>
#include <dim-sum/errno.h>

typedef u32 phandle;

struct device_node {
	const char *name;
	const char *type;
	phandle phandle;
	const char *full_name;
};

static inline const char *of_node_full_name(const struct device_node *np)
{
	return np ? np->full_name : "<no-node>";
}

#endif /* __DIM_SUM_OF_H */
