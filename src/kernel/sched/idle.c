#include <linux/compiler.h>
#include <dim-sum/kernel.h>
//#include <dim-sum/sched.h>

//#include <dim-sum/adapter.h>
#include <dim-sum/idle.h>

#include <dim-sum/sched.h>

#include <asm/asm-offsets.h>

void (*powersave)(void) = NULL;
static void default_powersave(void)
{
	cpu_do_idle();
}

static void default_idle(void)
{
	disable_irq();
	if (!need_resched())
	{
		if (powersave != NULL)
		{
			powersave();
		} else {
			default_powersave();
		}
	}
	enable_irq();
}

void cpu_idle (void)
{
	while (1) {
		void (*idle)(void);

		//这里的读屏障是需要的，因为其他核可能触发本CPU的调度
		rmb();
		idle = NULL;

		if (!idle)
			idle = default_idle;

		preempt_disable();
		while (!need_resched())
		{
			idle();
		}
		preempt_enable();
		schedule();
	}
}
