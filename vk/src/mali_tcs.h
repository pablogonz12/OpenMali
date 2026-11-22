/* SPDX-License-Identifier: MIT */
/* T1's tessellation-control identity check, from SPIR-V words (notes/tess-design.md § "T1 implementation").
 * mali_tess.c keeps the TES half (which folds gl_TessCoord instead of matching arithmetic); the TCS half has to be read
 * from words because the generic IO lowering destroys the per-invocation index (mali_tcs.c says which pass and why). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 = the TCS is provably the identity patch (3 output vertices, every level 1.0, every per-vertex output a copy of the
 * matching per-vertex input at the same control point), 0 = refused with a reason in `why` (never empty). */
int mali_tcs_passthrough(const uint32_t *spirv, uint32_t nwords, const char *entry, char *why, size_t whysz);

#ifdef __cplusplus
}
#endif
