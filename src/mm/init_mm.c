#include <dim-sum/mm_types.h>

#include <asm/memory.h>

struct memory_map_desc kern_memory_map = {
	.pt_l1		= global_pg_dir,
	.page_table_lock =  SMP_LOCK_UNLOCKED(kern_memory_map.page_table_lock),
};
