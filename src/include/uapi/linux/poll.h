#ifndef _UAPI_LINUX_POLL_H
#define _UAPI_LINUX_POLL_H

#include <dim-sum/string.h>

typedef unsigned int nfds_t;

#undef FD_ZERO
#undef FD_SET
#undef FD_CLR
#undef FD_ISSET

static inline void FD_ZERO(fd_set *__fdsetp)
{
       memset(__fdsetp, 0, sizeof(fd_set));
}
static inline void FD_SET(int __fd, fd_set *__fdsetp)
{
       __fdsetp->fds_bits[__fd/BITS_PER_LONG] |=
               (1UL << (__fd % BITS_PER_LONG));
}
static inline void FD_CLR(int __fd, fd_set *__fdsetp)
{
       __fdsetp->fds_bits[__fd/BITS_PER_LONG] &=
               ~(1UL << (__fd % BITS_PER_LONG));
}
static inline int FD_ISSET(int __fd, fd_set *__fdsetp)
{
       return (__fdsetp->fds_bits[__fd/BITS_PER_LONG] >>
               (__fd % BITS_PER_LONG)) & 1;
}

#endif /* _UAPI_LINUX_POLL_H */
