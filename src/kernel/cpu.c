#include <dim-sum/bitops.h>
#include <dim-sum/cpumask.h>
#include <dim-sum/delay.h>
#include <dim-sum/errno.h>
#include <dim-sum/idle.h>
#include <dim-sum/mutex.h>
#include <dim-sum/notifier.h>
#include <dim-sum/percpu.h>
#include <dim-sum/psci.h>
#include <dim-sum/sched.h>
#include <dim-sum/smp.h>
#include <dim-sum/types.h>
#include <dim-sum/cpu.h>

#include <asm/asm-offsets.h>
#include <asm/cacheflush.h>
#include <asm/mmu_context.h>
#include <asm/tlbflush.h>

/**
 * 互斥锁
 * 避免并发的进行CPU热插拨操作
 */
struct mutex mutex_cpu_hotplug = MUTEX_INITIALIZER(mutex_cpu_hotplug);

const DECLARE_BITMAP(cpu_all_bits, MAX_CPUS) = CPU_BITS_ALL;

static DECLARE_BITMAP(cpu_possible_bits, CONFIG_MAX_CPUS) ;
const struct cpumask *const cpu_possible_mask = to_cpumask(cpu_possible_bits);


static DECLARE_BITMAP(cpu_online_bits, CONFIG_MAX_CPUS) ;
const struct cpumask *const cpu_online_mask = to_cpumask(cpu_online_bits);


static DECLARE_BITMAP(cpu_present_bits, CONFIG_MAX_CPUS) ;
const struct cpumask *const cpu_present_mask = to_cpumask(cpu_present_bits);


static DECLARE_BITMAP(cpu_active_bits, CONFIG_MAX_CPUS) ;
const struct cpumask *const cpu_active_mask = to_cpumask(cpu_active_bits);

void mark_cpu_possible(unsigned int cpu, bool possible)
{
	if (possible)
		cpumask_set_cpu(cpu, to_cpumask(cpu_possible_bits));
	else
		cpumask_clear_cpu(cpu, to_cpumask(cpu_possible_bits));
}

void mark_cpu_present(unsigned int cpu, bool present)
{
	if (present)
		cpumask_set_cpu(cpu, to_cpumask(cpu_present_bits));
	else
		cpumask_clear_cpu(cpu, to_cpumask(cpu_present_bits));
}

void mark_cpu_online(unsigned int cpu, bool online)
{
	if (online)
		cpumask_set_cpu(cpu, to_cpumask(cpu_online_bits));
	else
		cpumask_clear_cpu(cpu, to_cpumask(cpu_online_bits));
}

void init_cpu_present(const struct cpumask *src)
{
	cpumask_copy(to_cpumask(cpu_present_bits), src);
}

void init_cpu_possible(const struct cpumask *src)
{
	cpumask_copy(to_cpumask(cpu_possible_bits), src);
}

void init_cpu_online(const struct cpumask *src)
{
	cpumask_copy(to_cpumask(cpu_online_bits), src);
}


struct salve_cpu_data salve_cpu_data;

typedef unsigned long (psci_fn)(unsigned long, unsigned long,
				unsigned long, unsigned long);
asmlinkage psci_fn __invoke_psci_fn_hvc;

#define PSCI_RET_SUCCESS			0
#define PSCI_RET_NOT_SUPPORTED			-1
#define PSCI_RET_INVALID_PARAMS			-2
#define PSCI_RET_DENIED				-3
#define PSCI_RET_ALREADY_ON			-4
#define PSCI_RET_ON_PENDING			-5
#define PSCI_RET_INTERNAL_FAILURE		-6
#define PSCI_RET_NOT_PRESENT			-7
#define PSCI_RET_DISABLED			-8
#define PSCI_RET_INVALID_ADDRESS		-9

static int psci_to_linux_errno(int errno)
{
	switch (errno) {
	case PSCI_RET_SUCCESS:
		return 0;
	case PSCI_RET_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	case PSCI_RET_INVALID_PARAMS:
	case PSCI_RET_INVALID_ADDRESS:
		return -EINVAL;
	case PSCI_RET_DENIED:
		return -EPERM;
	};

	return -EINVAL;
}

#define cpu_logical_map(cpu) (cpu)

static int psci_cpu_on(unsigned long cpuid, unsigned long entry_point)
{
	int err;
	u32 fn;

	fn = 0xc4000003;
	err = __invoke_psci_fn_hvc(fn, cpuid, entry_point, 0);
	return psci_to_linux_errno(err);
}

int psci_launch_cpu(unsigned int cpu)
{
	int err = 0;
	phys_addr_t entry = linear_virt_to_phys(slave_cpu_entry);

	printk("xby_debug in psci_launch_cpu, pa is %p.\n", entry);
	err = psci_cpu_on(cpu_logical_map(cpu), entry);

	return err;
}

static int boot_secondary(unsigned int cpu, struct task_desc *idle)
{
	return psci_launch_cpu(cpu);
}

void arch_raise_ipi(const struct cpumask *mask, enum ipi_cmd_type cmd_type)
{
	__smp_raise_ipi(mask, cmd_type);
}

asmlinkage void start_slave(struct salve_cpu_data *salve_cpu_data)
{
	int cpu = salve_cpu_data->cpu;
	smp_processor_id() = cpu;

	set_this_cpu_offset(per_cpu_offset(smp_processor_id()));

	/**
	 * 使identify映射失效
	 */
	set_ttbr0(linear_virt_to_phys(empty_zero_page));
	flush_tlb_all();

	gic_secondary_init();
	arch_timer_secondary_init();

	mark_cpu_online(cpu, true);

	//complete(&cpu_running);
	
	enable_irq();
	
	cpu_idle();

	BUG();
}

int nr_existent_cpus = 4;

static int __cpu_launch(unsigned int cpu, struct task_desc *idle)
{
	int ret;
	//TODO
	printk("xby_debug in __cpu_launch, %d.\n", cpu);

	salve_cpu_data.stack = task_stack_bottom(idle) + THREAD_START_SP;
	salve_cpu_data.cpu = cpu;
	__flush_dcache_area(&salve_cpu_data, sizeof(salve_cpu_data));

	ret = boot_secondary(cpu, idle);
	if (ret == 0) {
		msleep(10);

		if (!cpu_online(cpu)) {
			pr_crit("CPU%u: failed to come online\n", cpu);
			ret = -EIO;
		}
	} else {
		pr_err("CPU%u: failed to boot: %d\n", cpu, ret);
	}

	salve_cpu_data.stack = NULL;

	return ret;
}

int cpu_launch(unsigned int cpu)
{
	int ret = 0;
	struct task_desc *idle = idle_task(cpu);

	lock_cpu_hotplug();

	if (cpu_online(cpu) || !cpu_present(cpu)) {
		ret = -EINVAL;
		goto unlock;
	}

	ret = __cpu_launch(cpu, idle);
	
unlock:
	unlock_cpu_hotplug();

	return ret;
}

int cpu_offline(unsigned int cpu)
{
	/* TO-DO */
	return 0;
}

int register_cpu_notifier(struct notifier_data *data)
{
	/* TO-DO */
	return 0;
}

void unregister_cpu_notifier(struct notifier_data *data)
{
	/* TO-DO */
}
