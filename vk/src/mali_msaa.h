/* SPDX-License-Identifier: MIT */
/* MSAA (4x) and sampleRateShading are experimental until verified on hardware: everything user-visible is gated on
 * MALI_EXPERIMENTAL_MSAA=1 (sample-count advertisement, image creation with samples 4, the sampleRateShading feature). */
#ifndef MALI_MSAA_H
#define MALI_MSAA_H

#include <stdbool.h>
#include <stdlib.h>

static inline bool
mali_experimental_msaa(void)
{
   const char *e = getenv("MALI_EXPERIMENTAL_MSAA");
   return e && e[0] == '1';
}

#endif
