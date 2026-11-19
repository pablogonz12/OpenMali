/* SPDX-License-Identifier: MIT */
/* Geometry shaders on Mali (v11/v13). The tiler has no geometry stage: the GS body runs as a compute
 * kernel that writes one record per emitted vertex, and the draw is re-issued against those records with a
 * generated index buffer (primitive restart pads unwritten slots) and a passthrough vertex shader that reads
 * the records. Layout and params block are shared by the shaders and the command stream, so they live here
 * with no Mesa headers. */
#pragma once
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

/* The params block the compute kernel and the passthrough VS read. Both shaders see it at
 * mali_compile_opts.push_base (appended after the app's push constants), so the driver writes it with the
 * draw's FAU push block. 64 bytes, 8-byte aligned; keep in sync with the GS_NIR builders. */
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
   uint32_t pad;
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

#ifdef __cplusplus
}
#endif
