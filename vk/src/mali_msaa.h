/* SPDX-License-Identifier: MIT */
/* 4x MSAA draws and sampleRateShading are experimental until verified on hardware: gated on MALI_EXPERIMENTAL_MSAA=1 (the sampleRateShading feature,
 * 4x pipeline creation). The 4x sample-count advertisement (limits, image format properties) follows the same gate, so nothing is advertised that pipeline creation would reject. */
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
