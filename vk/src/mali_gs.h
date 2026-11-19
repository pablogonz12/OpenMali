/* SPDX-License-Identifier: MIT */
/* Geometry shaders on Mali (v11/v13). The tiler has no geometry stage: the GS body runs as a compute
 * kernel that writes one record per emitted vertex, and the draw is re-issued against those records with a
 * generated index buffer (primitive restart pads unwritten slots) and a passthrough vertex shader that reads
 * the records. Layout and params block are shared by the shaders and the command stream, so they live here
 * with no Mesa headers. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mali_compile.h"

#ifdef __cplusplus
extern "C" {
#endif

struct nir_shader;
struct nir_shader_compiler_options;

#define MALI_GS_MAX_OUTPUTS 64

/* Per-output record slot: every written varying occupies 16 bytes (a vec4), PSIZ 4, LAYER/VIEWPORT/PRIM_ID 4.
 * Same layout as PanVK's gpu_prerast GS records (042-geometry-shaders-gpu-prerast.patch). */
struct mali_gs_output {
   uint16_t location;   /* gl_varying_slot */
   uint8_t ncomps;
   uint8_t pad;
   uint32_t alu_type;   /* nir_alu_type of the stored value (0 = float32) */
};

struct mali_gs_info {
   uint64_t outputs_written;   /* VARYING_BIT_* of the GS's outputs */
   uint32_t vertices_out;      /* gl_MaxVerticesOut: slots reserved per invocation */
   uint32_t invocations;
   uint32_t input_prim;        /* mesa_prim of gl_in */
   uint32_t output_prim;       /* mesa_prim passed to EndPrimitive */
   uint32_t record_stride;     /* bytes per emitted vertex */
   uint32_t index_stride;      /* u32 indices per invocation slot (2 * vertices_out, padded to 4) */
   uint32_t noutputs;
   struct mali_gs_output output[MALI_GS_MAX_OUTPUTS];
};

#define MALI_GS_MAX_VB 4   /* vertex bindings the VS-record kernel can read (params tail) */

/* The params block the compute kernels and the passthrough VS read. All three see it at push constant byte
 * offset pc_off (appended after the app's push constants, 8-byte aligned), so the driver writes it into the
 * draw's push block and the dispatches' push blocks. 96 bytes (64 + the vertex-buffer tail); keep in sync with
 * the NIR builders in mali_gs.c. */
struct mali_gs_params {
   uint64_t vs_records;        /* real VS output records: the GS kernel's per-vertex inputs */
   uint64_t records;           /* GS output records (this buffer); the passthrough VS reads it */
   uint64_t indices;           /* generated index buffer */
   uint64_t draw;              /* reserved: GPU-written draw params (v1: unused) */
   uint32_t vertex_count;
   uint32_t instance_count;
   uint32_t vs_record_capacity;
   uint32_t cap_slots;
   uint32_t primitive_restart;
   uint32_t prim;
   uint32_t provoking_last;
   uint32_t first_vertex;      /* vkCmdDraw firstVertex (VS-record kernel: gl_VertexIndex = gid + first_vertex) */
   uint64_t vb_va[MALI_GS_MAX_VB];   /* tail: vertex buffer addresses (offset applied), indexed by binding */
};

/* Record/index geometry for a GS. arena_bytes is the record+index region the draw may use. */
void mali_gs_layout(struct mali_gs_info *gs, uint64_t arena_bytes);
uint32_t mali_gs_cap_slots(const struct mali_gs_info *gs, uint64_t arena_bytes);

/* SPIR-V -> GS facts (vertices_out, invocations, primitives, output list, record/index strides). No binary is
 * produced: the kernel and passthrough variants are compiled by later PRs. Returns 0 or negative. */
int mali_gs_analyze(const struct mali_compile_opts *o, struct mali_gs_info *out, char *err, size_t errsz);

/* Passthrough VS NIR: gl_VertexIndex picks a record, every output is loaded from its 16-byte slot and
 * stored as a VS output. Returns a MESA_SHADER_VERTEX nir_shader (caller ralloc_free()s it), NULL on error.
 * The record base is read from push constant byte offset pc_off (struct mali_gs_params.records). */
struct nir_shader *mali_gs_passthrough_nir(const struct nir_shader_compiler_options *opts,
                                           const struct mali_gs_info *gs, unsigned pc_off);

/* Flags for mali_gs_kernel_nir. */
#define MALI_GS_LOWER_TEST_NO_TRUNC 0x1   /* mutation hook for tests: drop the vertex-count truncation */

/* GS body -> compute kernel (port of panvk_gpu_prerast_lower_gs, minimal subset): list input topologies
 * (points/lines/triangles), one invocation, constant-offset IO. One thread = one input primitive:
 * gl_GlobalInvocationID.x. It first fills its index slot with 0xffffffff, then each EmitVertex that fits in
 * vertices_out writes the current outputs into record (slot * vertices_out + vcount) and appends that record
 * index; EndPrimitive advances the index position (leaving the restart value). Vertices past vertices_out are
 * dropped. vs_outputs is the real VS's outputs_written (its record slots are popcount-ranked). Reads
 * struct mali_gs_params at pc_off. Returns a MESA_SHADER_COMPUTE nir_shader (caller ralloc_free()s), NULL on
 * error. */
struct nir_shader *mali_gs_kernel_nir(const struct mali_compile_opts *o, const struct mali_gs_info *gs,
                                      uint64_t vs_outputs, unsigned pc_off, unsigned flags,
                                      char *err, size_t errsz);

/* One vertex attribute the VS-record kernel fetches (per-vertex rate, float32 components only). */
struct mali_gs_vs_attr {
   uint32_t location;   /* VkVertexInputAttributeDescription.location */
   uint32_t binding;    /* < MALI_GS_MAX_VB */
   uint32_t offset;
   uint32_t stride;
   uint32_t ncomps;     /* 1..4 float32 components present in the vertex buffer (the rest read as 0,0,0,1) */
};

/* App VS -> compute kernel that runs it for gl_GlobalInvocationID.x (the vertex index) and stores its outputs
 * into the VS record array (vs_records, record = popcount(vs_outputs) 16-byte slots, popcount-ranked). Attributes
 * are fetched from params.vb_va[binding] + (first_vertex + gid) * stride + offset. Only gl_VertexIndex /
 * gl_VertexID are supported system values (InstanceIndex reads 0: instance_count must be 1). *vs_outputs gets the
 * VS's written outputs. Returns a MESA_SHADER_COMPUTE nir_shader (caller ralloc_free()s), NULL on error. */
struct nir_shader *mali_gs_vs_kernel_nir(const struct mali_compile_opts *o, const struct mali_gs_vs_attr *attr,
                                         unsigned nattr, unsigned pc_off, uint64_t *vs_outputs, char *err, size_t errsz);

#define MALI_GS_MAX_ATTR 16   /* vertex attributes the VS-record kernel can fetch */

/* Everything a pipeline with a geometry stage owns (built at vkCreateGraphicsPipelines, MALI_GS_EXPERIMENTAL). */
struct mali_gs_pipe {
   struct mali_gs_info info;
   uint64_t vs_outputs;
   unsigned pc_off;             /* byte offset of struct mali_gs_params in every push block */
   unsigned out_prim;           /* Mali draw mode of the generated draw: 1 points, 4 line strip, 10 triangle strip */
   unsigned nattr;
   struct mali_gs_vs_attr attr[MALI_GS_MAX_ATTR];   /* the app's vertex input, as the record kernel fetches it */
   struct mali_compiled vs_k;   /* app VS -> records */
   struct mali_compiled gs_k;   /* GS body -> records + indices */
   struct mali_compiled pt;     /* passthrough VS of the generated draw (compiled as a vertex shader) */
   uint64_t vs_k_va, gs_k_va;   /* GPU VAs of the kernel binaries (filled by the pipeline's code BO) */
};

/* The draw-time plan of one vkCmdDraw(vertex_count) through a GS pipeline: what the two dispatches run over and
 * how the generated indexed draw is sized. All indices of a slot past the emitted count keep 0xffffffff
 * (primitive restart), so the draw count is the whole padded buffer (index_count) and empty emissions simply
 * draw nothing. */
struct mali_gs_draw_plan {
   uint32_t vcount;             /* vertices: threads of the VS-record kernel */
   uint32_t prims;              /* input primitives: threads of the GS kernel, and record/index slots */
   uint32_t vs_dispatch[3];
   uint32_t dispatch[3];
   uint32_t records_bytes;      /* GS output records (prims * vertices_out * record_stride) */
   uint32_t vs_records_bytes;   /* real VS records (vcount * popcount(vs_outputs) * 16) */
   uint32_t indices_bytes;
   uint32_t index_count;        /* prims * index_stride: includes the restart padding */
   uint32_t index_stride;
   unsigned out_prim;           /* draw mode of the generated draw */
   bool empty;                  /* no input primitive (or no vertices): nothing to dispatch, nothing to draw */
};

/* MALI_GS_EXPERIMENTAL=1: pipelines may name a geometry stage. Nothing is advertised to the application. */
bool mali_gs_enabled(void);
/* Fills *out for one draw. Returns 0, or -EINVAL when the draw cannot be expressed (overflow of the 32-bit sizes). */
int mali_gs_plan_draw(const struct mali_gs_pipe *p, uint32_t vertex_count, struct mali_gs_draw_plan *out);
/* Primitive-restart padding of one generated index buffer: every index becomes 0xffffffff (the kernel then
 * overwrites the ones it emits). */
void mali_gs_pad_indices(uint32_t *idx, uint32_t cap_slots, uint32_t index_stride);

/* ovs / ogs: opts with the VS / GS SPIR-V (stage field ignored). push_size: the pipeline layout's push constant
 * bytes. attr/nattr: the pipeline's vertex input. Fails (negative, message) for anything the kernels cannot do
 * (list in vk/GEOMETRY-SHADERS.md). Free with mali_gs_pipe_free. */
int mali_gs_pipe_build(const struct mali_compile_opts *ovs, const struct mali_compile_opts *ogs,
                       const struct mali_gs_vs_attr *attr, unsigned nattr, unsigned push_size,
                       struct mali_gs_pipe *out, char *err, size_t errsz);
void mali_gs_pipe_free(struct mali_gs_pipe *p);

/* Generated draw sizing for a vkCmdDraw(vertex_count): input primitives, slots the arena holds, index count. */
uint32_t mali_gs_prims(const struct mali_gs_info *gs, uint32_t vertex_count);

#ifdef __cplusplus
}
#endif
