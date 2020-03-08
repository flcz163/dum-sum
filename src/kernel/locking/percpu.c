#include <dim-sum/beehive.h>
#include <dim-sum/percpu.h>

/**
 * per-cpu变量在所有CPU上面的偏移值
 */
unsigned long per_cpu_var_offsets[MAX_CPUS];

void *__alloc_percpu(size_t size, size_t align)
{
	struct dynamic_percpu *pdata;
	int i;

	pdata = kmalloc(sizeof (*pdata), PAF_KERNEL | __PAF_ZERO);
	if (!pdata)
		return NULL;

	for (i = 0; i < MAX_CPUS; i++) {
		if (!cpu_possible(i))
			continue;
		pdata->ptrs[i] = kmalloc(size, PAF_KERNEL | __PAF_ZERO);

		if (!pdata->ptrs[i])
			goto oom;
	}

	return (void *)(~(unsigned long)pdata);

oom:
	while (--i >= 0) {
		if (!cpu_possible(i))
			continue;
		kfree(pdata->ptrs[i]);
	}
	kfree(pdata);

	return NULL;
}

void free_percpu(const void *ptr)
{
	struct dynamic_percpu *pdata;
	int i;

	pdata = (struct dynamic_percpu *)(~(unsigned long)ptr);
	for (i = 0; i < MAX_CPUS; i++) {
		if (!cpu_possible(i))
			continue;
		kfree(pdata->ptrs[i]);
	}
	kfree(pdata);
}
