/*
 * Generate definitions needed by the preprocessor.
 * This code generates raw asm output which is post-processed
 * to extract and format the required data.
 */

#define __GENERATING_BOUNDS_H
/* Include headers that define the enum constants of interest */
#include <dim-sum/types.h>
#include <dim-sum/kbuild.h>
#include <dim-sum/page_area.h>

void __main(void);

void __main(void)
{ 
	/* The enum constants to put into include/generated/bounds.h */
	DEFINE(__PG_AREA_COUNT, PG_AREA_COUNT);
	/* End of constants */
}
