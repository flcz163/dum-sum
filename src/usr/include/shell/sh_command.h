
#ifndef _SH_COMMAND_H_
#define _SH_COMMAND_H_

/**********************************************************
 *                       Includers                        *
 **********************************************************/
#include "sh_completion.h"

/**********************************************************
 *                         Macro                          *
 **********************************************************/
#define MAX_CMD_LEN		256			/* �������� */
#define MAX_CMD_NUM		256			/* ���������� */
#define MAX_CMD_NAME	128			/* ��������� */
#define MAX_CMD_HELP	128			/* ����������Ϣ */
#define MAX_CMD_DESC	1024		/* �����������Ϣ */
	
/* ����shell����ִ�к�������ֵ */
#define RET_ERROR		-1			/* ִ��shell������� */
#define RET_OK			0			/* ִ��shell����ɹ� */

/* ���������ʾshell������Ŀ */
#define MAX_SHOW_SHELL_CMDS_NUM		20

/* 
 * ����shell����ִ�к������� 
 * ����1 -- ��������
 * ����2 -- �����ַ�������
 */
typedef int (sh_cmd_func_t)(int, char *[]);

/* ����shell����ṹ�� */
struct sh_cmd_t
{
	/* ��һ��shell����ָ�� */
	struct sh_cmd_t			*next;		

	/* shell�������� */
	char					*name;		

	/* shell����ִ�к���ָ�� */
	sh_cmd_func_t			*func;		

	/* shell���������Ϣ */
	char					*help;

	/* shell����ʹ��˵�� */
	char					*usage;

	/* shell����������Ϣ */
	char					*desc;		

	/* shell����������뺯��ָ�� */
	sh_cmd_arg_completer_t	*completer; 

	/* shell������� */
	struct sh_cmd_t			*alias;	
};

/* ����shell����ִ���̲߳����ṹ�� */
struct sh_cmd_run_t
{
	struct sh_cmd_t	*sh_cmd;	/* shell����ṹ�� */
	int				argc;		/* �������� */
	char			**argv;		/* �����ַ������� */
};

/**********************************************************
 *                  Extern Declareation                   *
 **********************************************************/
extern struct sh_cmd_t *register_shell_command(char *name, sh_cmd_func_t *func,
														char *help, char *usage, char *desc, 
														sh_cmd_arg_completer_t *completer); /* ע��shell���� */
extern struct sh_cmd_t *register_alias_command(char *alias_name, char *old_name);	/* ע��������� */
extern int unregister_shell_command(char *name);						/* ע��shell���� */
extern void initialize_shell_cmds(void);									/* ��ʼ��shell���� */
extern int sh_cmd_loop(void);												/* shell�����¼�ѭ�� */
extern struct sh_cmd_t *get_sh_cmd_list(void);								/* ��ȡshell�����б� */
extern int get_sh_cmd_count(void);										/* ��ȡshell������Ŀ */
extern void sh_show_prompt(void);										/* ��ӡshell��������ʾ�� */
extern int parse_cmd_args(char *cmd, char **argv[]);						/* ����shell�����������shell������ */
extern struct sh_cmd_t *lookup_shell_cmd(char *name);					/* ����shell�������Ʋ�������ṹ�� */
extern void sh_show_cmd_usage(char *cmd_name);						/* ��ӡshell����ʹ��˵�� */

#endif

