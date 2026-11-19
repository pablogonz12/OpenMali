/* SPDX-License-Identifier: MIT */
/* Software rasteriser over a recorded frame (struct mali_frame): the desktop can render a real frame through the real
 * command-buffer/frame record, with the real attachment addresses, row strides, load/store ops and format metadata, and
 * diff the pixels. See docs/host-harness.md.
 *
 * Scope: the fixed-function pipeline (viewport, scissor, cull, depth test/write, depth bias, blend, write mask) and the
 * attachment metadata; triangles only; single-sampled. Shading is a CPU program the caller registers per pipeline
 * (mali_soft_bind_program), not the app's SPIR-V: see docs/host-harness.md "shader code is out of scope" and the plan
 * for a SPIR-V interpreter. Memory is reached through hw->va_cpu (the fake backend's BO registry).
 */
#ifndef MALI_SOFT_H
#define MALI_SOFT_H

#include <stdbool.h>
#include <stdint.h>

#include "mali_cs_emit.h"
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALI_SOFT_VARY 8
#define MALI_SOFT_MRT 4   /* colour attachments the rasteriser can write in one pass */

struct mali_soft_program {
   const char *name;
   void *user;
   /* Vertex stage: attributes already converted to float[4] by location, the vertex-stage view of the push constants,
    * and the Vulkan vertex/instance ids. Writes clip space position and up to MALI_SOFT_VARY float4 varyings. */
   void (*vs)(void *user, const float attr[MALI_MAX_ATTR][4], const uint8_t *push,
              unsigned vertex_id, unsigned instance_id, float clip[4], float vary[MALI_SOFT_VARY][4]);
   /* Fragment stage: interpolated varyings at pixel (x, y), the fragment-stage push constants; writes RGBA. */
   void (*fs)(void *user, const float vary[MALI_SOFT_VARY][4], const uint8_t *push, unsigned x, unsigned y, float out[4]);
   /* Optional multi-render-target fragment stage: writes RGBA for every attachment of the frame (out[rt]). Used instead of
    * `fs` when set; required for frames with more than one colour attachment. */
   void (*fs_mrt)(void *user, const float vary[MALI_SOFT_VARY][4], const uint8_t *push, unsigned x, unsigned y,
                  float out[MALI_SOFT_MRT][4]);
};

struct mali_soft_stats {
   unsigned frames, draws, tris, frags;
   unsigned unsupported;          /* draws the rasteriser could not do (reason in why) */
   char why[160];
};

/* Register the CPU program that stands in for a pipeline's shaders. Keyed by the compiled shader pointers the frame
 * record carries (mali_gfx_state.vs/fs), so it survives pipeline rebinds and command-buffer reuse. */
void mali_soft_bind_program(const struct mali_compiled *vs, const struct mali_compiled *fs,
                            const struct mali_soft_program *p);
void mali_soft_clear_programs(void);
/* Drop the program registered for a pipeline that is being destroyed: the key is the address of the compiled-shader
 * members inside the pipeline, and a freed pipeline's storage can be handed to a new one. */
void mali_soft_retire(const struct mali_compiled *vs, const struct mali_compiled *fs);

/* hw->soft: render one frame into its attachments. The result is a BOOLEAN, not an errno: false means "this frame needs
 * something the rasteriser does not do", with the reason in mali_soft_last()->why. The only caller,
 * mali_frame_build() in mali_cs_emit.c, maps false to -ENOTSUP, which mali_cmd.c reports as
 * VK_ERROR_FEATURE_NOT_PRESENT; nothing here returns -ENOTSUP itself. */
bool mali_soft_frame(const struct mali_hw *hw, const struct mali_frame *f);

/* The compiled-shader pointers a graphics pipeline's frame records carry (mali_pipeline.c), for mali_soft_bind_program. */
void mali_soft_pipeline_keys(VkPipeline p, const struct mali_compiled **vs, const struct mali_compiled **fs);

/* The device's frame hardware context (mali_queue.c), for tests that build a frame record by hand. */
const struct mali_hw *mali_soft_device_hw(VkDevice dev);

/* Statistics of the last mali_soft_frame (for the test's assertions and the golden report). */
const struct mali_soft_stats *mali_soft_last(void);

#ifdef __cplusplus
}
#endif
#endif
