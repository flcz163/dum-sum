
#ifndef _SH_COMPLETION_H_
#define _SH_COMPLETION_H_

/**********************************************************
 *                       Includers                        *
 **********************************************************/

/**********************************************************
 *                         Macro                          *
 **********************************************************/
/* �����ַ�������ӿڴ����� */
#define HANDLE_COMPLETION_FAILED	-1
#define HANDLE_COMPLETION_DONE		0

/* ���岹���ַ������� */
#define UNKNOWN_COMPLETION			-1	/* δ���岹���ַ������� */
#define CMD_NAME_COMPLETION			0	/* shell�������Ʋ����ַ��� */
#define CMD_FILENAME_ARG_COMPLETION	1	/* shell�����ļ��������ַ��� */
#define CMD_LOCATION_ARG_COMPLETION	2	/* shell�����ַ�����ַ��� */

#define MAX_SHOW_COMPLETIONS_NUM	60	/* �����ʾ�����ַ�����Ŀ��ÿ��5�У�����12�� */

#define MAX_COMPLETION_STR_LEN		256	/* ������ַ������� */

/* 
 * ����shell�����������ִ�к�������
 * ����1 -- ��������ַ���
 * ����2 -- shell������������ַ��������ַ
 */
typedef int (sh_cmd_arg_completer_t)(char *, char **[]);

/**********************************************************
 *                  Extern Declareation                   *
 **********************************************************/
/* shell�����ַ������봦��ӿ� */
extern void sh_completion_handler(char *input, int *cursor, int *len);
/* shell�����ļ�����Ŀ¼���������봦��ӿ� */
extern int sh_filename_completer(char *input, char **completions[]);
/* shell����Ŀ¼���������봦��ӿ� */
extern int sh_directory_completer(char *input, char **completions[]);
/* shell�����ַ�������봦��ӿ� */
extern int sh_location_completer(char *input, char **completions[]);
/* shell�����������մ���ӿ� */
extern int sh_noop_completer(char *input, char **completions[]);
/* shell���������������մ���ӿ� */
extern int sh_command_completer(char *input, char **completions[]);

#endif

