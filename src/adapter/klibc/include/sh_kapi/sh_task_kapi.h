
#ifndef _SH_TASK_KAPI_H_
#define _SH_TASK_KAPI_H_

/**********************************************************
 *                       Includers                        *
 **********************************************************/

/**********************************************************
 *                         Macro                          *
 **********************************************************/

/**********************************************************
 *                  Extern Declareation                   *
 **********************************************************/
extern unsigned long  sh_ker_is_valid_task(unsigned long task_id);
extern unsigned long sh_ker_get_init_task_id(void);
extern unsigned long  sh_ker_get_next_task_id(unsigned long  prev_task_id);
extern char *sh_ker_get_task_name(unsigned long  task_id);
extern unsigned long sh_ker_get_task_entry(unsigned long  task_id);
extern unsigned long sh_ker_get_task_prio(unsigned long  task_id);
extern unsigned long  sh_ker_get_task_state(unsigned long  task_id);
extern unsigned long  sh_ker_suspend_task(unsigned long  task_id);
extern unsigned long sh_ker_resume_task(unsigned long  task_id);
extern void sh_ker_dump_task(unsigned long  task_id);
struct task_desc *sh_ker_get_task(unsigned long  task_id);

#endif

