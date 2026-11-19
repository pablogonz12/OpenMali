/* SPDX-License-Identifier: MIT */
/* 4x MSAA draws and sampleRateShading are experimental until verified on hardware: gated on MALI_EXPERIMENTAL_MSAA=1 (the sampleRateShading feature,
 * 4x pipeline creation). Sample-count advertisement and 4x image creation are NOT gated: main already had them and dEQP cases rely on them (decision: keep, document the gap in vk/README.md "Known gaps"). */
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
