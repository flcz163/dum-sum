#include <dim-sum/string.h>
#include <dim-sum/mem.h>
#include <dim-sum/cmd.h>

int sh_showmem_cmd(int argc, char **args)
{
	if (argc > 2) {
		printk("Usage: showmem/showmem\n");
		return -1;
	}

	return 0;
}

