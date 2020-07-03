
#ifndef _SH_READLINE_H_
#define _SH_READLINE_H_

/**********************************************************
 *                       Includers                        *
 **********************************************************/
#include "sh_utils.h"

/**********************************************************
 *                         Macro                          *
 **********************************************************/
#ifndef CTRL
#define CTRL(c) (c - '@')
#endif

#define RL_BUF_SIZE			256	/* �����ַ�����󳤶� */

#define INPUT_CHAR_NOECHO	0	/* ����������ַ� */
#define INPUT_CHAR_ECHO		1	/* ����������ַ� */

#define RET_READ_CHAR_DONE	0	/* ��ȡ�ַ����� */
#define RET_READ_CHAR_CONT	1	/* ������ȡ�ַ� */

/**********************************************************
 *                  Extern Declareation                   *
 **********************************************************/
extern void readline_handler_install(void);							/* ����shell��ȡ�ַ����ӿ� */
extern void readline_handler_remove(void);							/* �Ƴ�shell��ȡ�ַ����ӿ� */
extern int readline_echo(char *inputbuf, int maxsize, char *prompt);	/* ��ȡ�ַ������벢���� */
extern int readline_noecho(char *inputbuf, int maxsize, char *prompt);/* ��ȡ�ַ������벻���� */
extern int sh_read(char *buf, size_t size);								/* �������ն˶�ȡ�ַ����ַ��� */
extern void sh_write(char *buf, size_t size);								/* ������ն�����ַ����ַ��� */
extern void sh_clean_input(char *input, int *cursor, int *len);		/* ������������ַ��� */
extern char sh_get_last_char(void);									/* ��ȡshell���������ַ� */
extern void sh_set_last_char(char c);									/* ����shell���������ַ� */
extern void sh_trim_str(char *str);										/* ɾ���ַ���ǰ��ո� */
extern int sh_show_more(void);										/* �Ƿ������ʾ�ַ��� */
extern int sh_show_all(int num, int max_num);							/* �Ƿ���ʾ�����ַ��� */

#endif

