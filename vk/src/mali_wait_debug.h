/* SPDX-License-Identifier: MIT */
#ifndef MALI_WAIT_DEBUG_H
#define MALI_WAIT_DEBUG_H

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static pthread_once_t mali_wait_debug_once = PTHREAD_ONCE_INIT;
static int mali_wait_debug_enabled;
static void mali_wait_debug_init(void)
{
   const char *s = getenv("MALI_WAIT_DBG");
   mali_wait_debug_enabled = s && !strcmp(s, "1");
}
static inline int mali_wait_debug(void)
{
   pthread_once(&mali_wait_debug_once, mali_wait_debug_init);
   return mali_wait_debug_enabled;
}
static inline uint64_t mali_wait_now(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static inline void mali_wait_log(const char *what, const void *sync, const char *state,
                                 uint64_t ms, uint64_t value)
{
   char comm[16] = "?";
   prctl(PR_GET_NAME, comm, 0, 0, 0);
   fprintf(stderr, "mali_wait: %s %p %s %" PRIu64 " ms comm=%s tid=%ld value=%" PRIu64 "\n",
           what, sync, state, ms, comm, (long)syscall(SYS_gettid), value);
}
#endif
