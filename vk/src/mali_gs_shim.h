/* SPDX-License-Identifier: MIT */
/* MALI_ADVERTISE_GS=1: OPT-IN COMPATIBILITY SHIM, NOT CONFORMANCE. Default OFF.
 * The hardware (Valhall/Bifrost) has no geometry stage and this driver has no geometry-shader emulation. DXVK 2.x hard-requires the
 * geometryShader device feature to create a device at all, even for games that never create a GS pipeline. With this env set the driver
 * reports geometryShader = true and the maxGeometry* limits at the Vulkan minimums, but vkCreateGraphicsPipelines with a GEOMETRY stage
 * still returns VK_ERROR_FEATURE_NOT_PRESENT (logged once on stderr). A GS pipeline never works. The CTS geometry shader tests must stay
 * excluded while this is on, and nothing may report conformance with it. vk/GEOMETRY-SHADERS.md (status) and vk/README.md (gap list). */
#ifndef MALI_GS_SHIM_H
#define MALI_GS_SHIM_H

#include <stdbool.h>
#include <stdlib.h>

static inline bool
mali_advertise_gs(void)
{
   const char *e = getenv("MALI_ADVERTISE_GS");
   return e && e[0] == '1';
}

#endif
