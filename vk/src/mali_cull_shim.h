/* SPDX-License-Identifier: MIT */
/* MALI_ADVERTISE_CULL=1: OPT-IN COMPATIBILITY SHIM, NOT CONFORMANCE. Default OFF.
 * Valhall/Bifrost have no cull-distance stage: a primitive-level cull (drop the primitive iff ALL its vertices have a value < 0) cannot be
 * done from the vertex shader (one vertex) or the fragment shader (interpolated values). DXVK hard-requires the shaderCullDistance device
 * feature to create a device at all. With this env set the driver reports shaderCullDistance = true, maxCullDistances = 8 and
 * maxCombinedClipAndCullDistances = 8 (the Vulkan minimums), and the compiler IGNORES every gl_CullDistance write (the outputs are demoted
 * to plain temporaries). Primitives that should have been culled are therefore rasterised; D3D11 only uses SV_CullDistance as an
 * optimisation hint, so the visible result is normally identical. The CTS cull-distance tests must stay excluded while this is on, and
 * nothing may report conformance with it. vk/CULL-DISTANCE.md (behaviour) and vk/README.md (gap list). */
#ifndef MALI_CULL_SHIM_H
#define MALI_CULL_SHIM_H

#include <stdbool.h>
#include <stdlib.h>

static inline bool
mali_advertise_cull(void)
{
   const char *e = getenv("MALI_ADVERTISE_CULL");
   return e && e[0] == '1';
}

#endif
