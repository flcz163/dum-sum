#ifndef _LINUX_CIRC_BUF_H
#define _LINUX_CIRC_BUF_H

#ifdef __KLIBC__
#include <stdio.h>
#endif

int sh_showmem_cmd(int argc, char **args);
extern void dump_all_zones_info(int detail);

extern int net_ping_cmd(int argc, char *argv[]);
extern int net_tftp_cmd(int argc, char *argv[]);
extern int net_ip_cmd(int argc, char *argv[]);
extern int net_tcptest_cmd(int argc, char **args);
extern int net_caep_test_cmd(int argc, char **args);
extern int ls_main(int argc, char *argv[]);
extern int cd_main(int argc, char *argv[]);
extern int pwd_main(int argc, char *argv[]);
extern int mkdir_main(int argc, char *argv[]);
extern int mv_main(int argc, char *argv[]);
extern int rm_main(int argc, char *argv[]);
extern int creat_main(int argc, char *argv[]);
extern int xby_test_cmd(int argc, char *argv[]);
int cat_main(int argc, char *argv[]);
int mknod_main(int argc, char *argv[]);


extern int lwip_file_open(const char *pathname, int flags, mode_t mode);
extern int lwip_file_unlink(const char *a);
extern ssize_t lwip_file_write(int a, const void *b, size_t c);
extern ssize_t lwip_file_read(int a, void *b, size_t c);

#endif /* _LINUX_CIRC_BUF_H */
