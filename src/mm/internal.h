/**
 * 已经分配的永久性内存地址
 */
unsigned long boot_mem_allocated(void);
/**
 * 释放boot内存给伙伴系统
 */
unsigned long free_all_bootmem(void);
void __init init_page_allotter(void);
void __init init_beehive_early(void);
void __init init_beehive_allotter(void);
void init_sparse_memory(void);
