/* SPDX-License-Identifier: MIT */
#ifndef MALI_WAIT_DEBUG_H
#define MALI_WAIT_DEBUG_H
#include <stdint.h>
#include <time.h>

struct mali_device;
int mali_wait_debug(void);
void mali_wait_log(const char *what, const void *sync, const char *state,
                   uint64_t ms, uint64_t value);
void mali_wait_count(int change);
void mali_wait_heartbeat(struct mali_device *dev, int submit, int present);
static inline uint64_t mali_wait_now(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
#endif
