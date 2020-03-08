

/**********************************************************
 *                       Includers                        *
 **********************************************************/
#include <dim-sum/irq.h>
#include <dim-sum/sched.h>
#include <asm/ptrace.h>

#include <sh_arch_regs_kapi.h>

/**********************************************************
 *                         Macro                          *
 **********************************************************/

/**********************************************************
 *                  Extern Declareation                   *
 **********************************************************/

/**********************************************************
 *                    Global Variables                    *
 **********************************************************/

/**********************************************************
 *                    Static Variables                    *
 **********************************************************/

/**********************************************************
 *                       Implements                       *
 **********************************************************/

static inline unsigned long get_user_reg(struct task_desc *task, int offset)
{
	//return task->user_regs->uregs[offset];
	return 0;
}

unsigned long sh_arch_ker_get_ureg(struct task_desc *task, long off)
{
	if (off & 3 || off >= sizeof(struct exception_spot))
	{
		return 0;
	}

	return get_user_reg(task, off >> 2);
}

