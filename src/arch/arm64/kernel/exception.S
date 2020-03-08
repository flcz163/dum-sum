
#include <dim-sum/linkage.h>
#include <asm/asm-offsets.h>
#include <asm/assembler.h>

/**
 * 异常向量表
 */
	.align	11
ENTRY(exception_vectors)
	ventry	el1_no_imp		// Synchronous EL1t
	ventry	el1_no_imp		// IRQ EL1t
	ventry	el1_no_imp		// FIQ EL1t
	ventry	el1_no_imp		// Error EL1t

	ventry	el1_sync			// Synchronous EL1h
	ventry	el1_irq			// IRQ EL1h
	ventry	el1_no_imp		// FIQ EL1h
	ventry	el1_no_imp		// Error EL1h

	ventry	el0_no_imp		// Synchronous 64-bit EL0
	ventry	el0_irq			// IRQ 64-bit EL0
	ventry	el0_no_imp		// FIQ 64-bit EL0
	ventry	el0_no_imp		// Error 64-bit EL0

	ventry	el0_no_imp		// Synchronous 32-bit EL0
	ventry	el0_no_imp		// IRQ 32-bit EL0
	ventry	el0_no_imp		// FIQ 32-bit EL0
	ventry	el0_no_imp		// Error 32-bit EL0
END(exception_vectors)

/**
 * 中断时，保存寄存器现场到堆栈中
 */
	.macro	save_regs, el, regsize = 64
	sub	sp, sp, #S_FRAME_SIZE
	.if	\regsize == 32
	mov	w0, w0				// zero upper 32 bits of x0
	.endif
	stp	x0, x1, [sp, #16 * 0]
	stp	x2, x3, [sp, #16 * 1]
	stp	x4, x5, [sp, #16 * 2]
	stp	x6, x7, [sp, #16 * 3]
	stp	x8, x9, [sp, #16 * 4]
	stp	x10, x11, [sp, #16 * 5]
	stp	x12, x13, [sp, #16 * 6]
	stp	x14, x15, [sp, #16 * 7]
	stp	x16, x17, [sp, #16 * 8]
	stp	x18, x19, [sp, #16 * 9]
	stp	x20, x21, [sp, #16 * 10]
	stp	x22, x23, [sp, #16 * 11]
	stp	x24, x25, [sp, #16 * 12]
	stp	x26, x27, [sp, #16 * 13]
	stp	x28, x29, [sp, #16 * 14]

	.if	\el == 0
	mrs	x21, sp_el0
	get_thread_info tsk			// Ensure MDSCR_EL1.SS is clear,
	ldr	x19, [tsk, #TI_FLAGS]		// since we can unmask debug
	disable_step_tsk x19, x20		// exceptions when scheduling.
	.else
	add	x21, sp, #S_FRAME_SIZE
	.endif
	mrs	x22, elr_el1
	mrs	x23, spsr_el1
	stp	lr, x21, [sp, #S_LR]
	stp	x22, x23, [sp, #S_PC]

	/**
	 * 将系统调用号置为-1
	 * 如果是svc调用，那么随后会重新设置其值
	 */
	.if	\el == 0
	mvn	x21, xzr
	str	x21, [sp, #S_SYSCALLNO]
	.endif

	/*
	 * 调用本宏后，下面三个寄存器的值为:
	 *
	 * x21 - aborted SP
	 * x22 - aborted PC
	 * x23 - aborted PSTATE
	*/
	.endm

/**
 * 恢复寄存器
 */
	.macro	restore_regs, el, ret = 0
	ldp	x21, x22, [sp, #S_PC]		// load ELR, SPSR
	.if	\el == 0
	ldr	x23, [sp, #S_SP]		// load return stack pointer
	msr	sp_el0, x23
	.endif
	msr	elr_el1, x21			// set up the return data
	msr	spsr_el1, x22
	.if	\ret
	ldr	x1, [sp, #S_X1]			// preserve x0 (syscall return)
	.else
	ldp	x0, x1, [sp, #16 * 0]
	.endif
	ldp	x2, x3, [sp, #16 * 1]
	ldp	x4, x5, [sp, #16 * 2]
	ldp	x6, x7, [sp, #16 * 3]
	ldp	x8, x9, [sp, #16 * 4]
	ldp	x10, x11, [sp, #16 * 5]
	ldp	x12, x13, [sp, #16 * 6]
	ldp	x14, x15, [sp, #16 * 7]
	ldp	x16, x17, [sp, #16 * 8]
	ldp	x18, x19, [sp, #16 * 9]
	ldp	x20, x21, [sp, #16 * 10]
	ldp	x22, x23, [sp, #16 * 11]
	ldp	x24, x25, [sp, #16 * 12]
	ldp	x26, x27, [sp, #16 * 13]
	ldp	x28, x29, [sp, #16 * 14]
	ldr	lr, [sp, #S_LR]
	add	sp, sp, #S_FRAME_SIZE		// restore sp
	eret					// return to kernel
	.endm

	.macro	get_thread_info, rd
	mov	\rd, sp
	and	\rd, \rd, #~(PROCESS_STACK_SIZE - 1)	// top of stack
	.endm

	tsk	.req	x28		// current process_desc

.align	6

ENTRY(el1_irq)
	save_regs 1
	enable_dbg

/**
 * 调用handle_arch_irq指向的中断处理函数
 * 如gic_handle_irq
 */
	adrp	x1, handle_arch_irq
	ldr	x1, [x1, #:lo12:handle_arch_irq]
	mov	x0, sp
	blr	x1
/**
 * 这里直接恢复现场了
 * 那么抢占处理在哪里?
 * 答案是:放到C代码结束处处理了。
 */
1:
	restore_regs 1
ENDPROC(el1_irq)

ENTRY(el0_irq)
	save_regs 0
	enable_dbg

	adrp	x1, handle_arch_irq
	ldr	x1, [x1, #:lo12:handle_arch_irq]
	mov	x0, sp
	blr	x1
1:
	restore_regs 0
ENDPROC(el0_irq)

ENTRY(el0_no_imp)
1:	b 1b
ENDPROC(el0_no_imp)

ENTRY(el1_no_imp)
1:	b 1b
ENDPROC(el1_no_imp)
	
el1_sync:
	mrs	x1, esr_el1			// read the syndrome register
	mrs	x0, far_el1

/**
 * 在内核态中，出现了sync异常，只能挂掉了:(
 */
	b hung
ENDPROC(el1_no_imp)
