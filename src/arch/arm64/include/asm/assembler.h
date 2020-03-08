#ifndef __ASSEMBLY__
#error "Only include this from assembly code"
#endif

#ifndef __ASM_ASSEMBLER_H
#define __ASM_ASSEMBLER_H

#include <asm/ptrace.h>
#include <asm/process.h>

/**
 * 堆栈压栈/出栈
 */
	.macro	push, xreg1, xreg2
	stp	\xreg1, \xreg2, [sp, #-16]!
	.endm

	.macro	pop, xreg1, xreg2
	ldp	\xreg1, \xreg2, [sp], #16
	.endm

/**
 * 开关中断
 */
	.macro	disable_irq
	msr	daifset, #2
	.endm

	.macro	enable_irq
	msr	daifclr, #2
	.endm

/**
 * 禁止中断，并保存原来的标志到寄存器
 */
	.macro	save_and_disable_irqs, olddaif
	mrs	\olddaif, daif
	disable_irq
	.endm

/**
 * 恢复中断寄存器
 */
	.macro	restore_irqs, olddaif
	msr	daif, \olddaif
	.endm

/**
 * 打开/关闭调试异常
 */	.macro	disable_dbg
	msr	daifset, #8
	.endm

	.macro	enable_dbg
	msr	daifclr, #8
	.endm

/**
 * 禁止单步调试线程
 */
	.macro	disable_step_tsk, flgs, tmp
	tbz	\flgs, #__TIF_SINGLESTEP, 9990f
	mrs	\tmp, mdscr_el1
	bic	\tmp, \tmp, #1
	msr	mdscr_el1, \tmp
	isb	/* Synchronise with enable_dbg */
9990:
	.endm

/**
 * 打开单步调试
 */
	.macro	enable_step_tsk, flgs, tmp
	tbz	\flgs, #__TIF_SINGLESTEP, 9990f
	disable_dbg
	mrs	\tmp, mdscr_el1
	orr	\tmp, \tmp, #1
	msr	mdscr_el1, \tmp
9990:
	.endm

/**
 * 同时打开中断和调试
 */
	.macro	enable_dbg_and_irq
	msr	daifclr, #(8 | 2)
	.endm

/**
 * SMP数据屏障
 */
	.macro	smp_dmb, opt
	dmb	\opt
	.endm

#define USER(l, x...)				\
9999:	x;					\
	.section __ex_table,"a";		\
	.align	3;				\
	.quad	9999b,l;			\
	.previous

/**
 * 寄存器别名
 */
lr	.req	x30		// link register

/**
 * 中断向时表入口
 */
	 .macro	ventry	label
	.align	7
	b	\label
	.endm

/**
 * 根据大小端配置，确定是否生成代码
 */
#ifdef CONFIG_CPU_BIG_ENDIAN
#define CPU_BE(code...) code
#else
#define CPU_BE(code...)
#endif

#ifdef CONFIG_CPU_BIG_ENDIAN
#define CPU_LE(code...)
#else
#define CPU_LE(code...) code
#endif

#ifndef CONFIG_CPU_BIG_ENDIAN
	.macro	regs_to_64, rd, lbits, hbits
#else
	.macro	regs_to_64, rd, hbits, lbits
#endif
	orr	\rd, \lbits, \hbits, lsl #32
	.endm

/**
 * PC相对寻址操作
 * 被操作的地址与当前PC相差不超过4G
 */
	/**
	 * 将地址值加载到寄存器中
	 */
	.macro	adr_l, dst, sym, tmp=
	.ifb	\tmp
	adrp	\dst, \sym
	add	\dst, \dst, :lo12:\sym
	.else
	adrp	\tmp, \sym
	add	\dst, \tmp, :lo12:\sym
	.endif
	.endm

	/**
	 * 将地址的指向的内容加载到寄存器
	 */
	.macro	ldr_l, dst, sym, tmp=
	.ifb	\tmp
	adrp	\dst, \sym
	ldr	\dst, [\dst, :lo12:\sym]
	.else
	adrp	\tmp, \sym
	ldr	\dst, [\tmp, :lo12:\sym]
	.endif
	.endm

	/*
	 * 将寄存器内容存储到地址中
	 */
	.macro	str_l, src, sym, tmp
	adrp	\tmp, \sym
	str	\src, [\tmp, :lo12:\sym]
	.endm

#endif	/* __ASM_ASSEMBLER_H */
