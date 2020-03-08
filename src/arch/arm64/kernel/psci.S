
#include <dim-sum/linkage.h>

#include <asm/assembler.h>

/**
 * int __invoke_psci_fn_hvc(u64 function_id, u64 arg0, u64 arg1, u64 arg2)
 * 启动从核时需要调用
 */
ENTRY(__invoke_psci_fn_hvc)
	hvc	#0
	ret
ENDPROC(__invoke_psci_fn_hvc)