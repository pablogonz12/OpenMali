/* SPDX-License-Identifier: MIT */
/* Force-included (-include) into every Mesa translation unit of the bionic build, so Mesa stays
 * unmodified. Fills the few libc/libsync gaps of the NDK. None of this is reachable from our ICD
 * (display WSI threads, panthor kmod), the code only has to compile and link. */
#ifndef MALI_BIONIC_COMPAT_H
#define MALI_BIONIC_COMPAT_H
#include <errno.h>
#include <poll.h>
#include <pthread.h>

#ifndef PTHREAD_CANCEL_ASYNCHRONOUS
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
static inline int pthread_setcanceltype(int type, int *old) { (void)type; if (old) *old = 0; return 0; }
static inline int pthread_cancel(pthread_t t) { (void)t; return ENOSYS; }
#endif

/* libsync's sync_wait(fd, timeout_ms): 0 on signal, -1/ETIME on timeout. */
static inline int sync_wait(int fd, int timeout)
{
   struct pollfd p = {.fd = fd, .events = POLLIN};
   int r;
   do { r = poll(&p, 1, timeout); } while (r < 0 && errno == EINTR);
   if (r == 0) { errno = ETIME; return -1; }
   return r < 0 ? -1 : 0;
}
#endif
