#ifndef _DIM_SUM_BUS_H
#define _DIM_SUM_BUS_H

struct device;

extern int bus_add_device(struct device * dev);

void init_bus(void);

#endif /* _DIM_SUM_BUS_H */