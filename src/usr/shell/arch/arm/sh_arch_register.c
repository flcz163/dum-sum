

/**********************************************************
 *                       Includers                        *
 **********************************************************/
#include <sh_kapi.h>
 
#include "sh_arch_register.h"
#include "sh_utils.h"

/**********************************************************
 *                         Macro                          *
 **********************************************************/
static struct regset arm_regset_map[]=
{
	/*瀵勫瓨鍣ㄦ槧灏勮〃*/
	{ "r0", 0, 4, 1, 1, 0},
	{ "r1", 4, 4, 1, 1, 4},
	{ "r2", 8, 4, 1, 1, 8},
	{ "r3", 12, 4, 1, 1, 12},
	{ "r4", 16, 4, 1, 1, 16},
	{ "r5", 20, 4, 1, 1, 20},
	{ "r6", 24, 4, 1, 1, 24},
	{ "r7", 28, 4, 1, 1, 28},
	{ "r8", 32, 4, 1, 1, 32},
	{ "r9", 36, 4, 1, 1, 36},
	{ "r10", 40, 4, 1, 1, 40},
	{ "fp", 44, 4, 1, 1, 44},
	{ "ip", 48, 4, 1, 1, 48},
	{ "sp", 52, 4, 1, 1, 52},
	{ "lr", 56, 4, 1, 1, 56},
	{ "pc", 60, 4, 1, 1, 60},
	{ "f0", 64, 12, 0, 0, -1},
	{ "f1", 76, 12, 0, 0, -1},
	{ "f2", 88, 12, 0, 0, -1},
	{ "f3", 100, 12, 0, 0, -1},
	{ "f4", 112, 12, 0, 0, -1},
	{ "f5", 124, 12, 0, 0, -1},
	{ "f6", 136, 12, 0, 0, -1},
	{ "f7", 148, 12, 0, 0, -1},
	{ "fps", 160, 4, 0, 0, -1},
	{ "cpsr", 164, 4, 1, 1, 64}
};

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

int arch_get_reg_no(const char *reg_name)
{
	int regno;

	for (regno = 0; regno < ARCH_REGS_NUM; regno++)
	{
		if (strcmp(arm_regset_map[regno].name, reg_name) == 0)
		{
			return regno;
		}
	}

	return -1;
}

const char *arch_get_reg_name(int regno)
{
	if (regno > ARCH_REGS_NUM)
	{
		return NULL;
	}
	else
	{
		return arm_regset_map[regno].name;
	}
}

unsigned long arch_get_register(int task_id, int regno)
{
	unsigned long reg_value = 0;

	reg_value = sh_ker_get_ureg(task_id, arm_regset_map[regno].ptrace_offset);

	return reg_value;
}


