#ifndef _PGTABLE_NOPUD_H
#define _PGTABLE_NOPUD_H

#ifndef __ASSEMBLY__

#define __PAGETABLE_PUD_FOLDED

/*
 * Having the pud type consist of a pt_l1 gets the size right, and allows
 * us to conceptually access the pt_l1 entry that this pud is folded into
 * without casting.
 */
typedef struct { pt_l1_t pt_l1; } pt_l2_t;

#define PUD_SHIFT	PT_L1_SHIFT
#define PTRS_PER_PT_L2	1
#define PUD_SIZE  	(1UL << PUD_SHIFT)
#define PUD_MASK  	(~(PUD_SIZE-1))

/*
 * The "pgd_xxx()" functions here are trivial for a folded two-level
 * setup: the pud is never bad, and a pud always exists (as it's folded
 * into the pt_l1 entry)
 */
static inline int pt_l1_is_empty(pt_l1_t pt_l1)		{ return 0; }
static inline int pt_l1_is_invalid(pt_l1_t pt_l1)		{ return 0; }
static inline int pgd_present(pt_l1_t pt_l1)	{ return 1; }
static inline void pgd_clear(pt_l1_t *pt_l1)	{ }
#define pud_ERROR(pud)				(pgd_ERROR((pud).pt_l1))

/*
 * (puds are folded into pgds so this doesn't get actually called,
 * but the define is needed for a generic inline function.)
 */
#define set_pgd(pgdptr, pgdval)			set_pt_l2((pt_l2_t *)(pgdptr), (pt_l2_t) { pgdval })

#define pt_l2_val(x)				(pt_l1_val((x).pt_l1))
#define pt_l2(x)				((pt_l2_t) { __pt_l1(x) } )

#define pgd_page(pt_l1)				(pud_page((pt_l2_t){ pt_l1 }))
#define pgd_page_vaddr(pt_l1)			(pt_l2_page_vaddr((pt_l2_t){ pt_l1 }))

/*
 * allocating and freeing a pud is trivial: the 1-entry pud is
 * inside the pt_l1, so has no extra memory associated with it.
 */
#define pud_alloc_one(mm, address)		NULL
#define pud_free(mm, x)				do { } while (0)
#define __pud_free_tlb(tlb, x, a)		do { } while (0)

#undef  pt_l2_promote
#define pt_l2_promote(addr, end)			(end)

/*******************分割线************************/
static inline pt_l2_t * pt_l2_ptr(pt_l1_t * pt_l1, unsigned long address)
{
	return (pt_l2_t *)pt_l1;
}

static inline void attach_to_pt_l1(pt_l1_t * pt_l1, pt_l2_t * pt_l2)
{
}

#endif /* __ASSEMBLY__ */
#endif /* _PGTABLE_NOPUD_H */
