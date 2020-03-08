#ifndef _DIM_SUM_LOG2_H
#define _DIM_SUM_LOG2_H

static __attribute_const__ inline bool 
is_power_of_2(unsigned long n)
{
	return (n != 0 && ((n & (n - 1)) == 0));
}

#endif /* _DIM_SUM_LOG2_H */
