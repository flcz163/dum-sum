#ifndef __ASM_EXCEPTION_H
#define __ASM_EXCEPTION_H

#include <asm/ptrace.h>

#ifndef __ASSEMBLY__

struct exception_spot {
	u64 regs[31];
	u64 sp;
	u64 pc;
	u64 pstate;
	u64 orig_x0;
	u64 syscallno;
};

#define __exception	__attribute__((section(".exception.text")))
#define __exception_irq_entry	__exception

asmlinkage void hung(unsigned long addr, unsigned esr);

#endif /* __ASSEMBLY__ */

#endif	/* __ASM_EXCEPTION_H */
