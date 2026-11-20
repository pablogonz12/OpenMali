/* SPDX-License-Identifier: MIT */
/* VK_EXT_transform_feedback, gated on MALI_EXPERIMENTAL_TF=1 (default off; unlike MSAA's opt-out in
 * mali_msaa.h): limits, features, properties, the extension entry and the XFB lowering all follow this one switch,
 * so nothing is advertised that the driver would then reject. With the gate on, a vertex shader that declares XFB
 * is lowered to global stores (mali_compile.c: nir_io_add_intrinsic_xfb_info + nir_lower_xfb_to_stores); no
 * streamout job is needed, since the stores use the ordinary memory path, and each draw writes the four buffer
 * bases (bound buffer address + offset) into a 32 B push-constant slot the pipeline reserved (mali_pipeline.c,
 * mali_cmd.c). Verified on the fake backend only so far; t_vk_feat --only tf_write is the device test.
 *
 * Known gaps while the gate is on: the stores run even outside BeginTransformFeedback/EndTransformFeedback;
 * geometryStreams and rasterizationStream are false (one stream, stream 0 only); num_vertices is 0, so draws with
 * instance_count > 1 are refused with a named message instead of overlapping the first instance's slots;
 * transformFeedbackQueries and transformFeedbackDraw are false. Do not advertise this by default until a device
 * run of tf_write passes. */
#ifndef MALI_TF_H
#define MALI_TF_H

#include <stdbool.h>
#include <stdlib.h>

static inline bool
mali_experimental_tf(void)
{
   const char *e = getenv("MALI_EXPERIMENTAL_TF");
   return e && e[0] == '1';
}

#endif
