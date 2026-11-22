/* SPDX-License-Identifier: MIT */
/* Geometry-shader lane 1 (notes/gs-design.md §4): a pass-through GS is accepted and drawn at zero extra work; every
 * other geometry shader is still refused with a named reason, and VkPhysicalDeviceFeatures::geometryShader stays
 * FALSE (nothing here advertises it: see mali_physical_device.c and t_vk_feat gs_limits).
 *
 * MALI_EXPERIMENTAL_GS=1 is the development gate the gs_* tests run under, in the spirit of MALI_EXPERIMENTAL_MSAA
 * (mali_msaa.h): it lets *pipeline creation* accept a pass-through GS while the advertised bit stays false, so no
 * application can find it (a driver that refuses at init stays refused; DXVK never asks for a GS stage with the bit
 * false). Off by default, never set by the driver, never in a fleet profile. */
#ifndef MALI_GS_H
#define MALI_GS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline bool
mali_experimental_gs(void)
{
   const char *e = getenv("MALI_EXPERIMENTAL_GS");
   return e && !strcmp(e, "1");   /* "12", "1x" and "1 " stay off: this gate decides what the device advertises */
}

/* Input primitive of a pass-through GS (Vulkan ExecutionMode InputPoints/InputLines/InputTriangles). */
enum mali_gs_prims { MALI_GS_POINTS = 1, MALI_GS_LINES = 2, MALI_GS_TRIANGLES = 3 };

/* Classify a geometry shader module.
 *   1 = provably a pass-through: 1-in/1-out, in order, single stream, single invocation, and every emitted output is a
 *       copy of the matching input vertex. The vertex shader alone therefore produces exactly the stream this GS
 *       would, so the driver compiles the VS and leaves the draw alone. *prims receives the input primitive.
 *   0 = refused; `why` (never empty) names the reason the caller reports.
 * The analysis is over SPIR-V words only: no NIR, no backend, so it cannot be changed by a lowering. */
int mali_gs_passthrough(const uint32_t *words, uint32_t nwords, const char *entry, unsigned *prims, char *why, size_t whysz);

#endif
