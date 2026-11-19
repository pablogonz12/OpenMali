/* SPDX-License-Identifier: MIT */
/* VK_EXT_transform_feedback emulation on Valhall (no stream-out hardware): design in docs/xfb-emulation.md.
 *
 * The vertex shader is compiled a second time as an "XFB variant" (mali_compile_opts.xfb): every Xfb-decorated output is
 * stored to buffer_va[b] + out_vertex * stride[b] + offset with a 64-bit global store, there is no IDVS and nothing is
 * rasterised. The variant runs as a compute job over (output vertices x instances) in front of the real draw, exactly as
 * Mesa's gallium panfrost does on CSF (pan_csf.c, csf_launch_xfb). Nothing here appends atomically: with one stream the byte
 * position of every captured vertex follows from the draw alone, so the recorded count is computed on the host (or, for
 * indirect and counter-buffer draws, by the same maths in a tiny kernel).
 *
 * This file holds the part that does not depend on the command stream: the parameter block, the capture plan (which
 * primitives fit, counter values, query results) and a CPU reference executor used by the tests and the benchmark. */
#ifndef MALI_XFB_H
#define MALI_XFB_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALI_XFB_MAX_BUFFERS 4

/* FAU parameter block read by the XFB variant (mali_compile_opts.xfb_params_off). 40 bytes. */
struct mali_xfb_params {
   uint64_t addr[MALI_XFB_MAX_BUFFERS];   /* byte address of vertex 0 of each buffer (bound offset + counter start already added) */
   uint32_t num_vertices;                 /* output vertices per instance */
   uint32_t pad;
};

/* VkPrimitiveTopology values (the enum is not included on purpose: this header has no Vulkan dependency). */
enum mali_xfb_topology {
   MALI_XFB_POINT_LIST = 0, MALI_XFB_LINE_LIST = 1, MALI_XFB_LINE_STRIP = 2, MALI_XFB_TRIANGLE_LIST = 3,
   MALI_XFB_TRIANGLE_STRIP = 4, MALI_XFB_TRIANGLE_FAN = 5,
   /* adjacency (6..9) and patch lists (10) cannot reach a vertex shader capture: mali_xfb_plan refuses them */
};

struct mali_xfb_buffer {
   uint64_t size;       /* bytes available from the bound offset (VK_WHOLE_SIZE already resolved) */
   uint32_t stride;     /* xfb_stride of the shader for this buffer; 0 = the shader does not capture to it */
   uint32_t start;      /* byte offset to continue from: counter buffer value at BeginTransformFeedback, else 0 */
};

struct mali_xfb_plan {
   uint32_t verts_per_prim;
   uint64_t prims_needed;                        /* what the draw generates: XFB query "needed" / PRIMITIVES_GENERATED */
   uint64_t prims_written;                       /* whole primitives that fit in every active buffer: query "written" */
   uint64_t out_vertices;                        /* prims_written * verts_per_prim: the compute job's size */
   uint32_t out_per_instance;                    /* num_vertices of struct mali_xfb_params (verts_per_prim * prims per instance) */
   uint64_t counter[MALI_XFB_MAX_BUFFERS];       /* value EndTransformFeedback stores: start + bytes written; 0 for inactive buffers */
};

/* Capture plan for one non-indexed or indexed draw (indexed: vertex_count = index count, primitive restart not handled:
 * returns false for it, see the design doc). Returns false (plan untouched) for topologies the emulation cannot honour or
 * a stride that is not a multiple of 4. */
bool mali_xfb_plan(unsigned topology, uint32_t vertex_count, uint32_t instance_count, bool primitive_restart,
                   const struct mali_xfb_buffer *buf, unsigned nbuf, struct mali_xfb_plan *out);

/* Which input vertex (0-based within the draw) is corner `corner` of primitive `prim` of one instance. Strip/fan order
 * follows the Vulkan primitive-assembly definition (odd strip triangles swap the first two vertices; fans pivot on 0). */
uint32_t mali_xfb_input_vertex(unsigned topology, uint32_t prim, uint32_t corner);

/* CPU reference executor: runs `vs` once per captured output vertex and writes the Xfb outputs the way the compute variant
 * does. `out_vs` fills the packed record for one input vertex (vertex id, instance id) into rec[], nwords floats laid out
 * as the single buffer's stride; the executor copies them to base + out_vertex * stride. One buffer. Returns vertices run. */
typedef void (*mali_xfb_cpu_vs)(void *user, uint32_t vertex_id, uint32_t instance_id, float *rec);
uint64_t mali_xfb_run_cpu(unsigned topology, uint32_t vertex_count, uint32_t instance_count, const struct mali_xfb_plan *plan,
                          uint32_t stride, uint8_t *buffer_base, mali_xfb_cpu_vs vs, void *user);

#ifdef __cplusplus
}
#endif
#endif
