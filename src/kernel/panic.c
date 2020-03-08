#include <dim-sum/bug.h>
#include <dim-sum/kernel.h>
#include <dim-sum/printk.h>
#include <dim-sum/sched.h>
#include <dim-sum/smp.h>
#include <dim-sum/stacktrace.h>

#include <asm/exception.h>

asmlinkage void hung(unsigned long addr, unsigned  esr)
{
	panic("system hung, %lx, %x\n", addr, esr);
}

void print_warning(const char *file, const int line)
{
	pr_warn("------------[ cut here ]------------\n");
	pr_warn("WARNING: CPU: %d PID: %d at %s:%d\n",
		smp_processor_id(), current->pid, file, line);

	dump_stack();
}

void panic(const char * fmt, ...)
{
	printk(fmt);
	dump_stack();

	while (1);
}
