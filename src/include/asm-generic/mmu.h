#ifndef _ASM_GENERIC_PGTABLE_H
#define _ASM_GENERIC_PGTABLE_H

#ifndef __ASSEMBLY__
#ifdef CONFIG_MMU
#define pt_l1_promote(addr, end)						\
({	unsigned long __boundary = ((addr) + PGDIR_SIZE) & PGDIR_MASK;	\
	(__boundary - 1 < (end) - 1)? __boundary: (end);		\
})

#ifndef pt_l2_promote
#define pt_l2_promote(addr, end)						\
({	unsigned long __boundary = ((addr) + PUD_SIZE) & PUD_MASK;	\
	(__boundary - 1 < (end) - 1)? __boundary: (end);		\
})
#endif

#ifndef pt_l3_promote
#define pt_l3_promote(addr, end)						\
({	unsigned long __boundary = ((addr) + PT_L3_SIZE) & PT_L3_MASK;	\
	(__boundary - 1 < (end) - 1)? __boundary: (end);		\
})
#endif

#endif /* CONFIG_MMU */
#endif /* !__ASSEMBLY__ */
#endif /* _ASM_GENERIC_PGTABLE_H */
