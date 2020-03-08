#include <dim-sum/kernel.h>
#include <dim-sum/sched.h>
#include <dim-sum/stacktrace.h>

static void __dump_stack(void)
{
	dump_task_stack(NULL, NULL);
}

asmlinkage __visible void dump_stack(void)
{
	__dump_stack();
}

