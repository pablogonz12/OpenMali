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

/* Mirror of the two Mesa enums this API reports, named so a caller needs no Mesa header. mali_poly.c static-asserts
 * every value (shader_enums.h:1334 enum mesa_prim, geometry.h:22 enum poly_gs_shape); `mode` is always a *decomposed*
 * primitive, because poly_nir_lower_gs() decomposes the output mode itself (poly_nir_lower_gs.c:1149). */
enum mali_poly_prim { MALI_POLY_PRIM_POINTS = 0, MALI_POLY_PRIM_LINES = 1, MALI_POLY_PRIM_TRIANGLES = 4 };
enum mali_poly_shape {
   MALI_POLY_SHAPE_DYNAMIC_INDEXED = 0,    /* index buffer the count shader writes, hardware instancing unused */
   MALI_POLY_SHAPE_STATIC_INDEXED = 1,     /* static index buffer, instance count = input primitives */
   MALI_POLY_SHAPE_STATIC_PER_PRIM = 2,    /* (max_indices, input_primitives) */
   MALI_POLY_SHAPE_STATIC_PER_INSTANCE = 3,
};

/* What poly_nir_lower_gs() produced, in plain integers (Mesa's enum values, not our own: `mode` is enum mesa_prim —
 * a decomposed primitive, i.e. strips and fans come back as triangles/lines — and `shape` is enum poly_gs_shape). */
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

#ifdef __cplusplus
}
#endif
