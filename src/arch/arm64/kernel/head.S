
#include <dim-sum/linkage.h>
#include <asm/assembler.h>
#include <asm/cache.h>
#include <asm/memory.h>
#include <asm/page.h>
#include <asm/asm-offsets.h>

/**
 * x9    ->  FDT
 * x10  ->  临时
 * x11  ->  虚拟地址与物理地址之间的差值
 * x12  ->  idmap_pg_dir(TTBR0)
 * x13  ->  global_pg_dir(TTBR1)
 */
#define VA_OFFSET	(kernel_text_start - TEXT_OFFSET)

#if (TEXT_OFFSET & 0xfff) != 0
#error TEXT_OFFSET must be at least 4KB aligned
#elif (KERNEL_VA_START & 0x1fffff) != 0
#error KERNEL_VA_START must be at least 2MB aligned
#elif TEXT_OFFSET > 0x1fffff
#error TEXT_OFFSET must be less than 2MB
#endif

.section	".head.text","ax"

	b	fake_start
/**
 * 供boot使用的参数块
 *	1、保留
 *	2、内核代码段偏移(小端)，0x80000
 *	3、内核镜像大小(小端)
 *	4、内核标志
 *	5、保留
 *	6、保留
 *	7、Magic
 *	8、Magic
 *	9、Magic
 *	10、Magic
 *	11、保留
 */
	.long	0
	.quad	_kernel_offset_le
	.quad	_kernel_size_le
	.quad	_kernel_flags_le
	.quad	0
	.quad	0
	.quad	0
	.byte	0x41
	.byte	0x52
	.byte	0x4d
	.byte	0x64
	.word	0

ENTRY(fake_start)
	b real_start
ENDPROC(fake_start)

.align	16

/**
 * 在第三级页面中创建2M大小的块映射项
 */
	.macro	create_block_map, tbl, flags, phys, start, end
	lsr	\phys, \phys, #MMU_BLOCK_SHIFT
	lsr	\start, \start, #MMU_BLOCK_SHIFT
	and	\start, \start, #PTRS_PER_PT_L4 - 1
	orr	\phys, \flags, \phys, lsl #MMU_BLOCK_SHIFT
	lsr	\end, \end, #MMU_BLOCK_SHIFT
	and	\end, \end, #PTRS_PER_PT_L4 - 1
9999:	str	\phys, [\tbl, \start, lsl #3]
	add	\start, \start, #1
	add	\phys, \phys, #MMU_BLOCK_SIZE
	cmp	\start, \end
	b.ls	9999b
	.endm

__create_temporary_page_tables:
	adrp	x12, idmap_pg_dir
	adrp	x13, global_pg_dir
	mov	x14, lr

	/**
	 * 对临时页表进行inval操作
	 */
	mov	x0, x12
	add	x1, x13, #GLOBAL_DIR_SIZE
	bl	__inval_cache_range

	/**
	 * 清除临时页表
	 */
	mov	x0, x12
	add	x6, x13, #GLOBAL_DIR_SIZE
1:	stp	xzr, xzr, [x0], #16
	stp	xzr, xzr, [x0], #16
	stp	xzr, xzr, [x0], #16
	stp	xzr, xzr, [x0], #16
	cmp	x0, x6
	b.lo	1b

	ldr	x7, = EARLY_MMUFLAGS

	/**
	 * identity space.
	 */
	mov	x0, x12
	adrp	x3, __idmap_text_start


#define EXTRA_SHIFT	(PT_L1_SHIFT + PAGE_SHIFT - 3)
#define EXTRA_PTRS	(1 << (48 - EXTRA_SHIFT))

#if VA_BITS != EXTRA_SHIFT
#error "Mismatch between VA_BITS and page size/number of translation levels"
#endif

	adrp	x5, __idmap_text_end
	clz	x5, x5
	cmp	x5, TCR_T0SZ(VA_BITS)
	b.ge	1f

	adr_l	x6, idmap_t0sz
	str	x5, [x6]
	dmb	sy
	dc	ivac, x6

	lsr	x5, x3, EXTRA_SHIFT
	and	x5, x5, EXTRA_PTRS - 1
	add	x6, x0, #PAGE_SIZE
	orr	x6, x6, #PT_L3_TYPE_TABLE
	str	x6, [x0, x5, lsl #3]
	add	x0, x0, #PAGE_SIZE
	
1:
	lsr	x5, x3, PT_L1_SHIFT
	and	x5, x5, PTRS_PER_PT_L1 - 1
	add	x6, x0, #PAGE_SIZE
	orr	x6, x6, #PT_L3_TYPE_TABLE
	str	x6, [x0, x5, lsl #3]
	add	x0, x0, #PAGE_SIZE
	mov	x5, x3
	adr_l	x6, __idmap_text_end	/* __idmap_text_end的物理地址 */
	create_block_map x0, x7, x3, x5, x6

	/**
	 * 映射内核镜像对应的物理地址
	 */
	mov	x0, x13
	mov	x5, #KERNEL_VA_START
	lsr	x3, x5, PT_L1_SHIFT
	and	x3, x3, PTRS_PER_PT_L1 - 1
	add	x6, x0, #PAGE_SIZE
	orr	x6, x6, #PT_L3_TYPE_TABLE
	str	x6, [x0, x3, lsl #3]
	add	x0, x0, #PAGE_SIZE
	
	mov x6, #1
	lsl x6, x6, #30
	sub x6, x6, #1
	mov	x3, x11
	create_block_map x0, x7, x3, x5, x6

	/**
	 * 映射FDT表，必须与镜像地址相距不超过512M
	 */
	mov	x3, x9	/* 物理地址 */
	and	x3, x3, #~((1 << 21) - 1)	/* 2M对齐 */
	mov	x6, #KERNEL_VA_START
	sub	x5, x3, x11
	tst	x5, #~((1 << 29) - 1)
	csel	x9, xzr, x10, ne
	b.ne	1f
	add	x5, x5, x6 /* 虚拟地址 */
	add	x6, x5, #1 << 21
	sub	x6, x6, #1
	create_block_map x0, x7, x3, x5, x6
1:
	mov	x0, x12
	add	x1, x13, #GLOBAL_DIR_SIZE
	dmb	sy
	bl	__inval_cache_range

	mov	lr, x14
	ret
ENDPROC(__create_temporary_page_tables)

	.macro	tcr_set_idmap_t0sz, valreg, tmpreg
		ldr_l	\tmpreg, idmap_t0sz
		bfi	\valreg, \tmpreg, #TCR_T0SZ_OFFSET, #TCR_TxSZ_WIDTH
	.endm

	.macro	reset_pmuserenr_el0, tmpreg
		mrs	\tmpreg, id_aa64dfr0_el1	// Check ID_AA64DFR0_EL1 PMUVer
		sbfx	\tmpreg, \tmpreg, #8, #4
		cmp	\tmpreg, #1			// Skip if no PMU present
		b.lt	9000f
		msr	pmuserenr_el0, xzr		// Disable PMU access from EL0
	9000:
	.endm

/**
 * 为CPU执行MMU方面的准备工作
 *
 * 结束后，x0返回SCTLR_EL1的值
 *
 */
ENTRY(__prepare_cpu_mmu)
	ic	iallu				/* I+BTB cache invalidate */
	tlbi	vmalle1is			/* invalidate I + D TLBs */
	dsb	ish

	mov	x0, #3 << 20
	msr	cpacr_el1, x0			/* Enable FP/ASIMD */
	mov	x0, #1 << 12			/* Reset mdscr_el1 and disable */
	msr	mdscr_el1, x0			/* access to the DCC from EL0 */
	reset_pmuserenr_el0 x0	/* Disable PMU access from EL0 */
	/*
	 * Memory region attributes for LPAE:
	 *
	 *   n = AttrIndx[2:0]
	 *			n	MAIR
	 *   DEVICE_nGnRnE	000	00000000
	 *   DEVICE_nGnRE	001	00000100
	 *   DEVICE_GRE		010	00001100
	 *   NORMAL_NC		011	01000100
	 *   NORMAL			100	11111111
	 */
	ldr	x5, =MAIR(0x00, MT_DEVICE_nGnRnE) | \
		     MAIR(0x04, MT_DEVICE_nGnRE) | \
		     MAIR(0x0c, MT_DEVICE_GRE) | \
		     MAIR(0x44, MT_NORMAL_NC) | \
		     MAIR(0xff, MT_NORMAL)
	msr	mair_el1, x5
	/*
	 * Prepare SCTLR
	 */
	adr	x5, __crval
	ldp	w5, w6, [x5]
	mrs	x0, sctlr_el1
	bic	x0, x0, x5
	orr	x0, x0, x6
	/**
	 * 准备TCR 和TTBR的值
	 */
	ldr	x10, =TCR_TxSZ(VA_BITS) | TCR_CACHE_FLAGS | TCR_SMP_FLAGS | \
			TCR_TG_FLAGS | TCR_ASID16 | TCR_TBI0
	tcr_set_idmap_t0sz	x10, x9

	/*
	 * 从ID_AA64MMFR0_EL1中读取PARange位
	 * 并将IPS位设置到TCR_EL1.
	 */
	mrs	x9, ID_AA64MMFR0_EL1
	bfi	x10, x9, #32, #3
	msr	tcr_el1, x10
	ret					// return to head.S
ENDPROC(__prepare_cpu_mmu)

ENTRY(real_start)
	/**
	 * 将x0~x3保存到boot_params中
	 * 这三个参数是boot传递给内核的值
	 */
	adr_l	x9, boot_params
	stp	x0, x1, [x9]
	stp	x2, x3, [x9, #16]

	/**
	 * 对boot_params中32个字节(x0~x3)执行inval操作
	 */
	adr_l x0, boot_params
	add	x1, x0, #0x20			// 4 x 8 bytes
	/**
	 * 在MMU未打开时，需要先调用此句
	 * 才能执行inval操作
	 */
	dmb	sy
	bl	__inval_cache_range

	adrp	x11, VA_OFFSET
	bl	__create_temporary_page_tables		/* x12=TTBR0, x13=TTBR1 */
	
	/**
	 * 前面已经准备好页表。
	 * 准备调用CPU设置代码，打开MMU.
	 */
	bl	__prepare_cpu_mmu
	/**
	 * 打开MMU以后，直接跳转到这里
	 */
	ldr	x14, =__prepare_jump_to_master
	b	__turn_on_cpu_mmu
ENDPROC(real_start)

	.section ".text","ax"

	.pushsection	.data..cacheline_aligned
	.align	L1_CACHE_SHIFT
	.ltorg
	
	.set	initial_sp, master_idle_stack + THREAD_START_SP

__prepare_jump_to_master:
	adr_l	x6, __bss_start
	adr_l	x7, __bss_stop

/**
 * 清空BSS段的内容
 */
1:	cmp	x6, x7
	b.hs	2f
	str	xzr, [x6], #8
	b	1b
/**
 * 准备启动线程的堆栈
 * 并跳转到C函数入口处
 */
2:
	adr_l	sp, initial_sp, x4
	str_l	x9, device_tree_phys, x5
	str_l	x11, phys_addr_origin, x6
	mov	x29, #0

/**
 * 是不是有点小激动??
 */
	b	start_master
ENDPROC(__prepare_jump_to_master)

/**
 * 打开当前CPU的MMU.
 *
 *  x0  = 用于打开MMU的SCTLR_EL1 值
 *  x14 =  当打开MMU以后，跳转到此地址(虚拟地址)
 *
 */
	.section	".idmap.text", "ax"
__turn_on_cpu_mmu:
	ldr	x5, = exception_vectors
	msr	vbar_el1, x5
	msr	ttbr0_el1, x12			/* idmap_pg_dir -> TTBR0 */
	msr	ttbr1_el1, x13			/* global_pg_dir -> TTBR1 */
	isb
	msr	sctlr_el1, x0
	isb
	/* __prepare_jump_to_master or __prepare_jump_to_slave*/
	br	x14
ENDPROC(__turn_on_cpu_mmu)

ENTRY(slave_cpu_entry)
	adrp	x12, idmap_pg_dir
	adrp	x13, global_pg_dir
	bl	__prepare_cpu_mmu

	ldr	x9, = slave_cpu_data
	ldr	x14, = __prepare_jump_to_slave
	b	__turn_on_cpu_mmu
ENDPROC(slave_cpu_entry)

ENTRY(__prepare_jump_to_slave)
	ldr	x0, [x9]			/* slave_cpu_data.stack */
	mov	sp, x0
	mov x0, x9
	mov	x29, #0
	b	start_slave
ENDPROC(__prepare_jump_to_slave)

	.section ".text.init", #alloc, #execinstr

	/*
	 * We set the desired value explicitly, including those of the
	 * reserved bits. The values of bits EE & E0E were set early in
	 * el2_setup, which are left untouched below.
	 *
	 *                 n n            T
	 *       U E      WT T UD     US IHBS
	 *       CE0      XWHW CZ     ME TEEA S
	 * .... .IEE .... NEAI TE.I ..AD DEN0 ACAM
	 * 0011 0... 1101 ..0. ..0. 10.. .0.. .... < hardware reserved
	 * .... .1.. .... 01.1 11.1 ..01 0.01 1101 < software settings
	 */
	.type	__crval, #object
__crval:
	.word	0xfcffffff			// clear
	.word	0x34d5d91d			// set
