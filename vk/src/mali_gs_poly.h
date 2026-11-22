/* SPDX-License-Identifier: MIT */
/* GS lane 2, step (ii): the four programs poly_nir_lower_gs() emits from one geometry module, compiled by our backend
 * (mali_gs_poly.c). Step (iii) — the arena, the count/prefix-sum dispatches and the rast draw — is what turns these into
 * a draw; until it exists mali_pipeline.c refuses a non-pass-through GS with this function's verdict in the reason. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "mali_compile.h"
#include "mali_poly.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mali_gs_programs {
   struct mali_compiled main, count, copy, pre_gs;
   struct mali_compiled vs;     /* the application VS as the compute pre-pass (only when the caller passed the VS module) */
   uint64_t vs_outputs;         /* its outputs_written: the mask poly's vertex param block carries */
   struct mali_poly_gs info;
   uint32_t has_main, has_count, has_copy, has_pre_gs, has_vs;
};

/* Lower a GS module with poly and compile the programs it returns. 0 = every present program compiled; -1 = refused
 * (message in err: the spirv/nir failure, poly's refusal, or the first program the backend rejected). `base` is the
 * pipeline's compile options (layout, arch, spec constants); its stage/spirv fields are ignored. */
int mali_gs_poly_compile(unsigned arch, const uint32_t *spirv, uint32_t nwords, const char *entry,
                         const struct mali_compile_opts *base, const uint32_t *vs_spirv, uint32_t vs_words, const char *vs_entry,
                         struct mali_gs_programs *out, char *err, size_t errsz);

/* Bytes of the pull table appended to the vertex param block (struct poly_vertex_params, 64 B), see mali_gs_poly.c. */
#define MALI_GS_VP_EXTRA 64u

void mali_gs_programs_free(struct mali_gs_programs *p);

/* One line for a log or a refusal reason: shapes and the four program sizes. Returns buf. */
const char *mali_gs_programs_report(const struct mali_gs_programs *p, char *buf, size_t n);

#ifdef __cplusplus
}
#endif
