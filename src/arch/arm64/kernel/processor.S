#include <dim-sum/linkage.h>

#include <asm/assembler.h>
#include <asm/asm-offsets.h>

/**
 * 默认的idle节能调用:WFI
 */
ENTRY(cpu_do_idle)
	dsb	sy				// WFI may enter a low-power mode
	wfi
	ret
ENDPROC(cpu_do_idle)

/**
 * 线程切换时，保存线程的寄存器并恢复新线程的寄存器
 */
ENTRY(__switch_cpu_context)
	add	x8, x0, #THREAD_CPU_CONTEXT
	mov	x9, sp
	stp	x19, x20, [x8], #16		/* store callee-saved registers */
	stp	x21, x22, [x8], #16
	stp	x23, x24, [x8], #16
	stp	x25, x26, [x8], #16
	stp	x27, x28, [x8], #16
	stp	x29, x9, [x8], #16
	str	lr, [x8]
	add	x8, x1, #THREAD_CPU_CONTEXT
	ldp	x19, x20, [x8], #16		/* restore callee-saved registers */
	ldp	x21, x22, [x8], #16
	ldp	x23, x24, [x8], #16
	ldp	x25, x26, [x8], #16
	ldp	x27, x28, [x8], #16
	ldp	x29, x9, [x8], #16
	ldr	lr, [x8]
	mov	sp, x9
	ret
ENDPROC(__switch_cpu_context)