
#ifndef _SH_ARCH_MM_H_
#define _SH_ARCH_MM_H_
/**********************************************************
 *                       Includers                        *
 **********************************************************/

/**********************************************************
 *                         Macro                          *
 **********************************************************/

#if 0
/* ���ڴ�ӿ� */
#define __arch_read_safe(src, dst, err)   \
    __asm__ __volatile__(                 \
        "1:ldr %1, [%2]\n\t"              \
        "2:\n"                            \
        ".section .fixup,\"ax\"\n"        \
        "3:mov %0, %3\n"                  \
        "   b 2b\n"                       \
        ".previous\n"                     \
        ".section __ex_table,\"a\"\n"     \
        "   .align 4\n"                   \
        "   .long 1b,3b\n"                \
        ".previous"                       \
        : "=r"(err),"=r"(dst)             \
        : "r" (src),"i"(-1), "0"(0))

/* д�ڴ�ӿ� */
#define __arch_write_safe(src, dst, err)  \
    __asm__ __volatile__(                 \
        "1:str %2, [%1]\n\t"              \
        "2:\n"                            \
        ".section .fixup,\"ax\"\n"        \
        "3:mov %0, %3\n"                  \
        "   b 2b\n"                       \
        ".previous\n"                     \
        ".section __ex_table,\"a\"\n"     \
        "   .align 4\n"                   \
        "   .long 1b,3b\n"                \
        ".previous"                       \
        : "=r"(err)           \
        : "r"(dst), "r" (src),"i"(-1), "0"(0))
#endif

/**********************************************************
 *                  Extern Declareation                   *
 **********************************************************/

#endif

