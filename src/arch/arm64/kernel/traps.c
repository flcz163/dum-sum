#include <dim-sum/kallsyms.h>
#include <dim-sum/kernel.h>
#include <dim-sum/sched.h>
#include <dim-sum/stacktrace.h>

#include <asm/exception.h>

static void dump_backtrace_entry(unsigned long where, unsigned long stack)
{
	print_ip_sym(where);
#if 0
	if (in_exception_text(where))
		dump_mem("", "Exception stack", stack,
			 stack + sizeof(struct pt_regs));
#endif
}

/*
 * AArch64 PCS assigns the frame pointer to x29.
 *
 * A simple function prologue looks like this:
 * 	sub	sp, sp, #0x10
 *   	stp	x29, x30, [sp]
 *	mov	x29, sp
 *
 * A simple function epilogue looks like this:
 *	mov	sp, x29
 *	ldp	x29, x30, [sp]
 *	add	sp, sp, #0x10
 */
int notrace unwind_frame(struct stackframe *frame)
{
	unsigned long high, low;
	unsigned long fp = frame->fp;

	low  = frame->sp;
	high = ALIGN(low, PROCESS_STACK_SIZE);

	if (fp < low || fp > high - 0x18 || fp & 0xf)
		return -EINVAL;

	frame->sp = fp + 0x10;
	frame->fp = *(unsigned long *)(fp);
	frame->pc = *(unsigned long *)(fp + 8);

	return 0;
}

static void dump_backtrace(struct exception_spot *regs, struct task_desc *tsk)
{
	struct stackframe frame;
	register unsigned long sp asm ("sp"); 

	pr_debug("%s(regs = %p tsk = %p)\n", __func__, regs, tsk);

	if (!tsk)
		tsk = current;

	if (regs) {
		frame.fp = regs->regs[29];
		frame.sp = regs->sp;
		frame.pc = regs->pc;
	} else if (tsk == current) {
		frame.fp = (unsigned long)__builtin_frame_address(0);
		frame.sp = sp;
		frame.pc = (unsigned long)dump_backtrace;
	} else {
		/*
		 * task blocked in __switch_to
		 */
		frame.fp = thread_saved_fp(tsk);
		frame.sp = thread_saved_sp(tsk);
		frame.pc = thread_saved_pc(tsk);
	}

	pr_emerg("Call trace:\n");
	while (1) {
		unsigned long where = frame.pc;
		int ret;

		ret = unwind_frame(&frame);
		if (ret < 0)
			break;
		dump_backtrace_entry(where, frame.sp);
	}
}

void dump_task_stack(struct task_desc *tsk, unsigned long *sp)
{
	dump_backtrace(NULL, tsk);
	barrier();
}
