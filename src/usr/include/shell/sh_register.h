
#ifndef _SH_REGISTER_H_
#define _SH_REGISTER_H_
/**********************************************************
 *                       Includers                        *
 **********************************************************/

/**********************************************************
 *                         Macro                          *
 **********************************************************/
struct regset
{
	const char *name;	/* �Ĵ���������			  */
	int offset;			/* �üĴ����ڻ����е�ƫ���� */
	int size;			/* �üĴ����Ĵ�С 		  */
	int readable;		/*�üĴ����Ƿ�ɶ�*/
	int writeable;		/*�üĴ����Ƿ��д*/
	int ptrace_offset;	/*��ȡ�Ĵ�������ptraceʱ�����ƫ����*/
};

/**********************************************************
 *                  Extern Declareation                   *
 **********************************************************/
extern void _initialize_register_cmds(void);
extern unsigned long sh_get_register(int task_id, int regno);
extern int sh_get_reg_no(const char *reg_name);
extern const char *sh_get_reg_name(int regno);

#endif

