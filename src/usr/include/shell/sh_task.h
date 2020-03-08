
#ifndef _SH_TASK_H_
#define _SH_TASK_H_

#include <dim-sum/sched.h>

/**********************************************************
 *                       Includers                        *
 **********************************************************/

/**********************************************************
 *                         Macro                          *
 **********************************************************/
#define UNKNOWN_STATE (-1)
/**********************************************************
 *                  Extern Declareation                   *
 **********************************************************/
extern void _initialize_task_cmds(void);
extern int is_valid_task(unsigned long task_id);
extern int task_is_suspend(unsigned long task_id);
extern char *get_task_name(unsigned long task_id);

#endif


