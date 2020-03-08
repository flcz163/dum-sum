#include <dim-sum/cpu.h>
#include <dim-sum/cpumask.h>
#include <dim-sum/smp.h>

int smp_call_for_all(void (*func) (void *info), void *info, int wait)
{
	return 0;
}

void __init launch_slave(void)
{
	unsigned int cpu;
	int i;

	for (i = 1; i < nr_existent_cpus; i++) {
		mark_cpu_possible(i, true);
	}
	
	for_each_possible_cpu(cpu) {
		mark_cpu_present(cpu, true);
	}
	
	for_each_present_cpu(cpu) {
		if (!cpu_online(cpu))
			cpu_launch(cpu);
	}
}

