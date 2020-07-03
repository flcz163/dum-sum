
#ifndef _SH_KEYMAP_H_
#define _SH_KEYMAP_H_

/**********************************************************
 *                       Includers                        *
 **********************************************************/

/**********************************************************
 *                         Macro                          *
 **********************************************************/
/* 
 * ������Ƽ�ִ�к������� 
 * ����1 -- �����ַ���
 * ����2 -- ��ǰ���λ��
 * ����3 -- ��ǰ�����ַ�������
 */
typedef int (sh_key_func_t)(char *, int *, int *);

/* ������Ƽ��ṹ�� */
typedef struct
{
	int 			 sh_key_value;	/* ���Ƽ���ֵ */
	sh_key_func_t	*sh_key_func;	/* ���Ƽ�ִ�к��� */
}sh_key_map_t;
	
/* �����ַ�ֵ�ͼ�ֵ */
#ifndef VK_DELETE
#define VK_DELETE	0x7f		/* Delete��ֵ */
#endif
	
#ifndef VK_ESC
#define VK_ESC		0x1b		/* Esc��ֵ */
#endif

/**********************************************************
 *                  Extern Declareation                   *
 **********************************************************/
extern int esc_key_handler(void);								/* esc��ֵ����ת�������ַ� */
extern sh_key_func_t ctrl_A_key_handler;
extern sh_key_func_t ctrl_B_key_handler;
extern sh_key_func_t ctrl_C_key_handler;
extern sh_key_func_t ctrl_D_key_handler;
extern sh_key_func_t ctrl_E_key_handler;
extern sh_key_func_t ctrl_F_key_handler;
extern sh_key_func_t ctrl_H_key_handler;
extern sh_key_func_t ctrl_I_key_handler;
extern sh_key_func_t ctrl_K_key_handler;
extern sh_key_func_t ctrl_L_key_handler;
extern sh_key_func_t ctrl_N_key_handler;
extern sh_key_func_t ctrl_P_key_handler;
extern sh_key_func_t ctrl_T_key_handler;
extern sh_key_func_t ctrl_U_key_handler;
extern sh_key_func_t ctrl_W_key_handler;
extern sh_key_func_t ctrl_Z_key_handler;
extern sh_key_func_t delete_key_handler;
/* ��ͨ�ַ�����ӿ� */
extern int normal_char_type_handler(char c, char *input, int *cursor, int *len);	
extern sh_key_func_t *lookup_sh_key_func(int sh_key_value);	/* ���ݿ����ַ�ֵ���Ҷ�Ӧ�Ĵ����� */

#endif

