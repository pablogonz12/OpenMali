/* SPDX-License-Identifier: MIT */
/* VK_EXT_transform_feedback is ON by default; MALI_EXPERIMENTAL_TF=0 is the full-legacy opt-out (it also drops the UBO
 * counts back to the Vulkan 1.0 minimums, mali_physical_device.c). The switch follows the MALI_EXPERIMENTAL_MSAA shape:
 * limits, features, properties, the extension entry and the XFB lowering all read this one function, so nothing is
 * advertised that the driver would then reject.
 *
 * Why default on: Mesa computes GL 3.0 / GLES 3.0 only when EXT_transform_feedback is exposed
 * (third_party/mesa/src/mesa/main/version.c:293,506) and zink derives that from max_stream_output_buffers
 * (zink_screen.c:982-984, st_extensions.c:1148), so without TF a zink client stays at GL 2.1 / GLES 2.0 whatever else
 * is advertised; Steam's CEF needs 3.x to create its window. With the gate the shelf is up in 9-13 s on G615/G925, with
 * it off Steam never gets there (300 s). The UBO counts are the same switch for the same reason: 128 per stage is what
 * st needs for ARB_uniform_buffer_object, and advertising that on a 2.1 context is the b900da2 regression (Steam's
 * webhelper restarting).
 *
 * How a capture works: a vertex shader that declares XFB is lowered to global stores (mali_compile.c:
 * nir_io_add_intrinsic_xfb_info + nir_lower_xfb_to_stores); no streamout job is needed, since the stores use the
 * ordinary memory path, and each draw writes the four buffer bases (bound buffer address + offset) into a 32 B
 * push-constant slot the pipeline reserved (mali_pipeline.c, mali_cmd.c). Verified on the fake backend and by
 * t_vk_feat --only tf_write on G610/G615/G925.
 *
 * Guard that lets it be default on: the stores exist only in a program the driver compiled WITH the lowering, and every
 * pipeline whose vertex shader declares XFB gets two vertex binaries, the second compiled without it
 * (mali_pipeline.c). A draw picks the plain one unless a BeginTransformFeedback/EndTransformFeedback range is open
 * (mali_cmd.c), so a shader with XFB outputs drawn outside a range stores nothing (before this, it stored to whatever
 * base the push block held: zero at the start of a command buffer, i.e. GPU VA 0).
 *
 * Known gaps while the gate is on (all silent, none refused; documented in mali_cmd.c and vk/README.md):
 * - counter buffers are marked written and never read, so a second draw inside one range restarts at the bind offset
 *   instead of appending (the last draw wins), and GL's pause/resume (ARB_transform_feedback2) is therefore wrong;
 * - VkBindTransformFeedbackBuffersEXT sizes are stored and never read: writes are not clamped to them, and a buffer
 *   index that was not bound while the range is open still yields address 0;
 * - VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT is not implemented (transformFeedbackQueries is false, but the query type
 *   comes with the extension, and mali_query.c refuses it with a sticky error);
 * - instanced XFB (instance_count > 1) is refused per draw (num_vertices is not wired), one stream only,
 *   geometryStreams and transformFeedbackDraw are false.
 * A device run of dEQP-VK.transform_feedback.simple.* has not happened yet (t_vk_feat tf_write is the only device
 * result). */
#ifndef MALI_TF_H
#define MALI_TF_H

#include <stdbool.h>
#include <stdlib.h>

static inline bool
mali_experimental_tf(void)
{
   const char *e = getenv("MALI_EXPERIMENTAL_TF");
   return !(e && e[0] == '0');   /* opt-out: =0 keeps the pre-TF capability set (no TF, UBO 12/72) */
}

#endif
