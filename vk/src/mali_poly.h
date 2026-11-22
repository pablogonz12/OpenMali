/* SPDX-License-Identifier: MIT */
/* GS lane 2, step (i) — notes/gs-design.md § 5(i) and § "Lane 2 step i": Mesa's src/poly NIR lowering, linked into this
 * driver as an unmodified library (libpoly_nir.a, built by vk/poly.sh for every target). This header is the only
 * Mesa-free face of it: no Mesa header is included here, so the drivers' callers keep compiling against plain C.
 *
 * Nothing in the draw path uses it yet. poly_nir_lower_gs() splits one GS into the four programs the proven vertex-pull
 * draw shape needs (main/count compute, a rast vertex program that pulls the arena, a pre-GS compute patch-up); turning
 * those into panfrost programs and recording the jobs is step (ii)/(iii), listed in the plan in notes/gs-design.md. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirror of the two Mesa enums this API reports, named so a caller needs no Mesa header. mali_poly.c static-asserts every
 * value (shader_enums.h:1334 enum mesa_prim, geometry.h:22 enum poly_gs_shape). `mode` is the GS's declared output
 * primitive: a strip stays a strip when the index buffer is dynamic (POLY_GS_SHAPE_DYNAMIC_INDEXED, the index buffer
 * supplies the primitives), and only becomes a decomposed list when poly could bake a static topology
 * (poly_nir_lower_gs.c:1149 runs u_decomposed_prim() inside optimize_static_topology). */
enum mali_poly_prim { MALI_POLY_PRIM_POINTS = 0, MALI_POLY_PRIM_LINES = 1, MALI_POLY_PRIM_TRIANGLES = 4,
                      MALI_POLY_PRIM_TRIANGLE_STRIP = 5 };
enum mali_poly_shape {
   MALI_POLY_SHAPE_DYNAMIC_INDEXED = 0,    /* index buffer the count shader writes, hardware instancing unused */
   MALI_POLY_SHAPE_STATIC_INDEXED = 1,     /* static index buffer, instance count = input primitives */
   MALI_POLY_SHAPE_STATIC_PER_PRIM = 2,    /* (max_indices, input_primitives) */
   MALI_POLY_SHAPE_STATIC_PER_INSTANCE = 3,
};

/* What poly_nir_lower_gs() produced, in plain integers (Mesa's enum values, not our own: `mode` is enum mesa_prim —
 * an enum mesa_prim value, not our own — and `shape` is enum poly_gs_shape). */
struct mali_poly_gs {
   uint32_t ok;              /* 1 = the lowering returned true */
   uint32_t mode;            /* enum mesa_prim: output primitive of the rasterization draw */
   uint32_t shape;           /* enum poly_gs_shape: how the rast draw is indexed/instanced */
   uint32_t count_words;     /* words per primitive in the count buffer (0 = counts are compile-time) */
   uint32_t max_indices;     /* per-input-primitive stride of the output index buffer */
   uint32_t xfb, prefix_sum, multistream;
   uint32_t rast_vertices;   /* poly_gs_rast_vertices(shape, max_indices, 1, 1): vertices the rast draw fetches */
   /* Which of the four programs exist. count is absent exactly when count_words == 0 (static counts). */
   uint32_t has_main, has_count, has_copy, has_pre_gs;
   uint32_t main_stage, count_stage, copy_stage, pre_gs_stage;   /* mesa_shader_stage of each (copy = VERTEX by design) */
   uint32_t nir_valid;       /* 1 = nir_validate_shader() accepted all four programs */
};

/* Compile a geometry shader module (a complete SPIR-V module, entry point "main" unless `entry`) with Mesa's poly
 * lowering, from the same SPIR-V -> NIR step mali_compile() uses.
 *   1 = lowered (out filled; the shaders are freed before returning), 0 = refused (message in err). */
int mali_poly_lower_gs(unsigned arch, const uint32_t *spirv, uint32_t nwords, const char *entry,
                       struct mali_poly_gs *out, char *err, size_t errsz);

/* Provenance of the linked library, for logs and the t_gs_lower line: never empty. */
const char *mali_poly_provenance(void);

/* ---- the tessellation half of the same library (notes/tess-design.md, lane T1) ----
 * poly_nir_lower_tes(tes, to_hw_vs) as one call, for the smoke test that proves Mesa's tess lowering is linked and runs
 * (t_tess_lower). T1's *draw* does not use it: a provable pass-through tess at levels 1.0 is compiled away (mali_tess.c),
 * exactly like lane 1's pass-through GS, and the tessellator chain that needs it is our T2. */
struct mali_poly_tes {
   uint32_t ok;
   uint32_t nir_valid;
   uint32_t lowered;          /* 1 = poly_nir_lower_tes returned true */
   uint32_t stage;            /* afterwards: VERTEX when to_hw_vs is set, else TESS_EVAL */
};
int mali_poly_lower_tes(unsigned arch, const uint32_t *spirv, uint32_t nwords, const char *entry, int to_hw_vs,
                        struct mali_poly_tes *out, char *err, size_t errsz);

/* ---- the control half, for T2 step 2 (notes/tess-design.md § T2): poly_nir_lower_tcs(tcs, false) as one call. The
 * lowered shader is compute-shaped (its stores go to the TCS output buffer through `load_tess_param_buffer_poly`), so the
 * record side needs two numbers from it: the per-patch stride of that buffer and the control points per patch. */
struct mali_poly_tcs {
   uint32_t ok;
   uint32_t nir_valid;
   uint32_t lowered;          /* 1 = poly_nir_lower_tcs returned true */
   uint32_t stride;           /* poly_tcs_output_stride(): bytes per patch in the TCS output buffer */
   uint32_t per_vertex_outputs; /* poly_tcs_per_vertex_outputs(): bitmask of varyings written per control point */
   uint32_t vertices_out;     /* nir->info.tess.tcs_vertices_out */
   uint32_t shared_size;      /* nir->info.shared_size after lowering (the barrier check's input) */
};
int mali_poly_lower_tcs(unsigned arch, const uint32_t *spirv, uint32_t nwords, const char *entry,
                        struct mali_poly_tcs *out, char *err, size_t errsz);

/* ---- shared with mali_tess.c: SPIR-V -> NIR with this driver's options, then the standard preprocessing (the list is in
 * mali_poly.c and is the one AgX hands poly). Both return NULL on failure with the reason in err. */
void *mali_poly_nir_from_spirv(unsigned arch, const uint32_t *spirv, uint32_t nwords, unsigned stage, const char *entry,
                               char *err, size_t errsz);
void mali_poly_preprocess(void *nir);

#ifdef __cplusplus
}
#endif
