#include <dim-sum/accurate_counter.h>
#include <dim-sum/beehive.h>
#include <dim-sum/delay.h>
#include <dim-sum/percpu.h>
#include <dim-sum/printk.h>
#include <dim-sum/sched.h>
#include <dim-sum/syscall.h>
#include <dim-sum/timex.h>
#include <dim-sum/wait.h>

#include <dim-sum/smp_lock.h>
#include <dim-sum/smp_rwlock.h>
#include <dim-sum/smp_bit_lock.h>
#include <dim-sum/smp.h>

void dim_sum_test(void);

struct foo_struct {
	int foo;
};
void xby_test(int);

extern void flush_cache_all(void);

#define __SC_DECL(t, a)	t a
#define __MAP0(m,...)
#define __MAP1(m,t,a,...) m(t,a)
#define __MAP2(m,t,a,...) m(t,a), __MAP1(m,__VA_ARGS__)
#define __MAP3(m,t,a,...) m(t,a), __MAP2(m,__VA_ARGS__)
#define __MAP4(m,t,a,...) m(t,a), __MAP3(m,__VA_ARGS__)
#define __MAP5(m,t,a,...) m(t,a), __MAP4(m,__VA_ARGS__)
#define __MAP6(m,t,a,...) m(t,a), __MAP5(m,__VA_ARGS__)
#define __MAP(n,...) __MAP##n(__VA_ARGS__)

#define RELATIVEJUMP_SIZE   (8 * sizeof(unsigned int))
#define DEFINE_ORIG_FUNC(rt, name, x, ...)					\
	static unsigned int e9_##name[RELATIVEJUMP_SIZE];				\
	static unsigned int inst_##name[RELATIVEJUMP_SIZE];				\
	static rt new_##name(__MAP(x, __SC_DECL, __VA_ARGS__));			\
	static rt (*orig_##name)(__MAP(x, __SC_DECL, __VA_ARGS__))

#define __flush_cache(c, n)        flush_cache_all()
typedef unsigned long long u64;
#define JUMP_INIT(func) do {												\
			unsigned long long addr = (unsigned long long)&new_##func;		\
			/* stp x29, x30, [sp,#-16]! */				\
			e9_##func[0] = 0xa9bf7bfdu;					\
			/* mov x29, #0x0 */							\
			e9_##func[1] = 0xd280001du | ((addr & 0xffff) << 5);		\
			/* movk    x29, #0x0, lsl #16 */				\
			e9_##func[2] = 0xf2a0001du | (((addr & 0xffff0000) >> 16) << 5);        \
			/* movk    x29, #0x0, lsl #32 */				\
			e9_##func[3] = 0xf2c0001du | (((addr & 0xffff00000000) >> 32) << 5);    \
			/* movk    x29, #0x0, lsl #48 */				\
			e9_##func[4] = 0xf2e0001du | (((addr & 0xffff000000000000) >> 48) << 5);   \
			/* blr x29 */									\
			e9_##func[5] = 0xd63f03a0u;						\
			/* ldp x29, x30, [sp],#16 */					\
			e9_##func[6] = 0xa8c17bfdu;						\
			/* ret */										\
			e9_##func[7] = 0xd65f03c0u;						\
		} while (0)

#define JUMP_INSTALL(func) do {						\
				memcpy(inst_##func, orig_##func, RELATIVEJUMP_SIZE);	\
				memcpy(orig_##func, e9_##func, RELATIVEJUMP_SIZE);    \
				__flush_cache(orig_##func, RELATIVEJUMP_SIZE);		\
			} while (0)

#define JUMP_REMOVE(func)						\
			memcpy(orig_##func, inst_##func, RELATIVEJUMP_SIZE);	\
			__flush_cache(orig_##func, RELATIVEJUMP_SIZE);

DEFINE_ORIG_FUNC(int, xby_test_jump, 1, int, a);

static  __attribute__((__noinline__)) int xby_test_jump(int a)
{
	printk("xby-debug in xby_test, val is %d\n", a);
	return a;
}

static  __attribute__((__noinline__)) int new_xby_test_jump(int a)
{
	printk("xby-debug in new_xby_test, val is %d\n", a);
	return a + 1;
}

void xby_test(int fun)
{
	if (fun == 7)
	{
		int i;
		static void *mem[10000];
		static unsigned long ptr[10000];

		for (i = 0; i < 10000; i++)
			ptr[i] = alloc_page_memory(PAF_DMA);

		for (i = 0; i < 10000; i++)
			free_page_memory(ptr[i]);

		for (i = 0; i < 10000; i++)
			mem[i] = kmalloc(4000, PAF_DMA);

		for (i = 0; i < 10000; i++)
			kfree(mem[i]);

		for (i = 0; i < 10000; i++)
			mem[i] = kmalloc(400, PAF_DMA);

		for (i = 0; i < 10000; i++)
			kfree(mem[i]);

		printk("memory test finished.\n");
	} 
	else if (fun == 8)
	{
		*(unsigned long *)(0) = 0;
	}
	else if (fun == 9)
	{
		preempt_disable();
		msleep(1);
	}
	else if (fun == 10)
	{
		//int b = return_bool();
		//printk("b is %d.\n", b);
		extern void fs_test(void);
		fs_test();
	}
	else if (fun == 11)
	{
		sys_sync();
	}
	else if (fun == 12)
	{
		int ret;
		//int (*fun)(int);

#if 1
		ret = xby_test_jump(1);
		printk("xby-debug in main, ret is %d\n", ret);

		orig_xby_test_jump = xby_test_jump;
		printk("orig_xby_test is %p\n", orig_xby_test_jump);
		JUMP_INIT(xby_test_jump);
		JUMP_INSTALL(xby_test_jump);
msleep(1);
		ret = xby_test_jump(1);
		printk("xby-debug in main, ret is %d\n", ret);
#endif
		//fun = 0x123456789abc4321;
		//ret = fun(100);
		//printk("xby-debug in main, ret is %d\n", ret);
#if 0
		//ffffffc0000a01dc
		asm volatile("//	__xchg4\n"
		"	stp		x29, x30, [sp, #-16]!\n"
		"	mov		x29, #0x01dc\n"
		"	movk    x29, #0x000a, lsl #16\n"
		"	movk    x29, #0xffc0, lsl #32\n"
		"	movk    x29, #0xffff, lsl #48\n"
		"	blr		x29\n"
		"	ldp		x29, x30, [sp], #16\n"
		"#1:	b		1b\n"
			:
			:
			: "memory");
		printk("xby-debug in main, ret is %d\n", 2);

         asm volatile("//    __xchg4\n"
         "   stp     x29, x30, [sp, #-16]!\n"
         "   mov     x29, #0\n"
         "   movk    x29, #0, lsl #16\n"
         "   movk    x29, #0, lsl #32\n"
         "   movk    x29, #0, lsl #48\n"
         "   blr     x29\n"
         "   ldp     x29, x30, [sp], #16\n"
         "#1:    b       1b\n"
             : 
             : 
             : "memory");
#endif
	}
}
void dim_sum_test(void)
{
#if 0
	static struct smp_lock static_lock = SMP_LOCK_UNLOCKED(static_lock); 
	static struct smp_rwlock static_rwlock = SMP_RWLOCK_UNLOCKED(static_rwlock); 

	int i;
	unsigned long *addr = (unsigned long *)&dim_sum_test;
			cycles_t now, b = get_cycles();
			udelay(100);
			now = get_cycles();
			printk("xby_debug, test for udelay(100, cycles is %lu, %lu, diff is %lu, my_test is %p, my_test + 1 is %p.\n", b, now, now - b, addr, addr + 1);
	//while (1);

	{
		unsigned long flags;
		smp_lock_init(&static_lock);
		smp_lock(&static_lock);
		smp_unlock(&static_lock);
		if (smp_trylock(&static_lock))
			smp_unlock(&static_lock);
		smp_lock_irqsave(&static_lock, flags);
		smp_unlock_irqrestore(&static_lock, flags);
		if (smp_trylock_irqsave(&static_lock, flags))
			smp_unlock_irqrestore(&static_lock, flags);
	}

	{
		unsigned long flags;
		smp_rwlock_init(&static_rwlock);
		smp_read_lock(&static_rwlock);
		smp_read_unlock(&static_rwlock);
		smp_write_lock(&static_rwlock);
		smp_write_unlock(&static_rwlock);
		if (smp_tryread(&static_rwlock))
			smp_read_unlock(&static_rwlock);
		if (smp_trywrite(&static_rwlock))
			smp_write_unlock(&static_rwlock);
		smp_read_lock_irqsave(&static_rwlock, flags);
		smp_read_unlock_irqrestore(&static_rwlock, flags);
		smp_write_lock_irqsave(&static_rwlock, flags);
		smp_write_unlock_irqrestore(&static_rwlock, flags);
	}

	{
		unsigned long bit_lock = 0;
		smp_bit_lock(0, &bit_lock);
		smp_bit_unlock(0, &bit_lock);
		smp_bit_trylock(0, &bit_lock);
		smp_bit_unlock(0, &bit_lock);
	}

	{
		static struct accurate_counter static_counter = ACCURATE_COUNTER_INIT(0);
		__maybe_unused unsigned long c = 0;
		c = accurate_read(&static_counter);
		accurate_inc(&static_counter);
		accurate_dec(&static_counter);
		accurate_add(2, &static_counter);
		accurate_sub(2, &static_counter);
		accurate_xchg(&static_counter, 1);
	}

	{
		struct foo_struct *pcpu = alloc_percpu(struct foo_struct);
		struct foo_struct *tmp = hold_percpu_ptr(pcpu);
		tmp->foo = 0;
		loosen_percpu_ptr(tmp);
	}

	{
		static DECLARE_BITMAP(bits, MAX_CPUS) = CPU_BITS_ALL;
		struct cpumask *const mask = to_cpumask(bits);
		cpumask_clear_cpu(0, mask);
		arch_raise_ipi(mask, IPI_CALL_FUNC);
	}
	msleep(100);
	{
		for (i = 0; i <= 10; i++)
			WARN_ONCE(1, "this is my test.\n");
	}
	{
		struct wait_queue wait;
		init_waitqueue(&wait);
		cond_wait(wait, 1 > 0);
		cond_wait_interruptible(wait, 1 > 0);
		cond_wait_timeout(wait, 1 > 0, 10);
		cond_wait_interruptible_timeout(wait, 1 > 0, 10);
	}
	{
		unsigned long bit = 0;
		wait_on_bit_timeout(&bit, 0, TASK_INTERRUPTIBLE, 10);
		wait_on_bit_io(&bit, 0, TASK_INTERRUPTIBLE);
		wait_on_bit(&bit, 0, TASK_INTERRUPTIBLE);
	}
#endif
}

