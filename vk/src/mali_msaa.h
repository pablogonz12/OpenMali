/* SPDX-License-Identifier: MIT */
/* 4x MSAA is ON by default (Mesa's GL ladder needs MaxSamples >= 4 for OpenGL 3.0; the hardware "Average" resolve and pipeline acceptance
 * are real, checked by t_vk_info and t_vk_draw msaa_resolve4/msaa_sample_shading on both phones). MALI_EXPERIMENTAL_MSAA=0 opts out to 1x
 * only; MALI_EXPERIMENTAL_MSAA=1 is a no-op kept for old scripts. Sample shading is 1-sample-effect either way. */
#ifndef MALI_MSAA_H
#define MALI_MSAA_H

#include <stdbool.h>
#include <stdlib.h>

static inline bool
mali_experimental_msaa(void)
{
   const char *e = getenv("MALI_EXPERIMENTAL_MSAA");
   return !(e && e[0] == '0');   /* default on; =0 keeps the 1x-only limits and pipeline rules */
}

#endif
