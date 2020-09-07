#ifndef _DIM_SUM_CPU_H
#define _DIM_SUM_CPU_H

#include <dim-sum/init.h>
#include <dim-sum/mutex.h>

struct notifier_data;
/**
 * 主核在启动从核时，传递给从核的参数
 */
struct slave_cpu_data {
	/**
	 * 从CPU堆栈，必须是第一个字段
	 */
	void *stack;
	/**
	 * 从CPU的ID
	 */
	int cpu;
};

#define MAX_CPUS		CONFIG_MAX_CPUS

/**
 * 实际存在 的CPU个数
 * 必须小于MAX_CPUS
 * 或者说dim-sum只管理这些CPU
 */
extern int nr_existent_cpus;

/**
 * 注册及取消注册
 * CPU热插拨回调函数
 */
extern int register_cpu_notifier(struct notifier_data *data);
extern void unregister_cpu_notifier(struct notifier_data *data);

extern struct mutex mutex_cpu_hotplug;
#define lock_cpu_hotplug()	mutex_lock(&mutex_cpu_hotplug)
#define unlock_cpu_hotplug()	mutex_unlock(&mutex_cpu_hotplug)

/**
 * 启动CPU开始工作
 */
int cpu_launch(unsigned int cpu);
/**
 * 停止CPU工作
 */
int cpu_offline(unsigned int cpu);

/**
 * 初始化主核及从核的C入口函数
 */
asmlinkage void __init start_master(void);
asmlinkage void start_slave(struct slave_cpu_data *);

#endif /* _DIM_SUM_CPU_H */
