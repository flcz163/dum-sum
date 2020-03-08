#ifndef _DIM_SUM_IOREMAP_H
#define _DIM_SUM_IOREMAP_H

#include <dim-sum/init.h>

void vunmap(const void *addr);
void iounmap(void __iomem *addr);

#endif /* _DIM_SUM_IOREMAP_H */