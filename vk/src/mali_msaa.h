/* SPDX-License-Identifier: MIT */
/* 4x MSAA is on by default: Mesa's GL ladder needs MaxSamples >= 4 for OpenGL 3.0 (src/mesa/main/version.c:269-291), and 4x here is a real
 * implementation (hardware "Average" resolve, vk/src/mali_cmd.c:960-1051; pipeline acceptance vk/src/mali_pipeline.c:194), verified by
 * t_vk_draw msaa_resolve4 / msaa_sample_shading and t_vk_info's sample-count checks. MALI_EXPERIMENTAL_MSAA=0 opts out (limits, image format
 * properties and pipeline creation follow this one switch, so nothing is advertised that pipeline creation would reject). Sample shading is
 * 1-sample-effect, which is unchanged by the gate. */
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
