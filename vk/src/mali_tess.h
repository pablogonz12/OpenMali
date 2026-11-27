/* SPDX-License-Identifier: MIT */
/* Tessellation lane T1 (notes/tess-design.md §"Lane T1"): a tessellation pipeline whose TES provably changes nothing is
 * drawn by the vertex program alone — zero extra jobs, zero extra bytes, no tessellator chain, no compute pre-pass.
 *
 * The accepted shape, and nothing else:
 *   * patchControlPoints == 3 (checked by the caller) and a TCS that declares 3 output vertices;
 *   * the TCS writes gl_TessLevelOuter[0..3] and gl_TessLevelInner[0..1] as the *constant* 1.0 (in Vulkan the levels are
 *     TCS outputs — a VS cannot write them — so a real tess pipeline is VS + TCS + TES, and T1's TCS is the identity
 *     patch: every per-vertex output copied from the per-vertex input of the same varying and control point);
 *   * the TES is `layout(triangles, equal_spacing) in` with no point mode, and at gl_TessCoord = (1,0,0), (0,1,0),
 *     (0,0,1) — the three output vertices of a level-1 patch — every one of its outputs is the matching patch input of
 *     the *same* output index (same Location/component/type).
 * Then the three tessellated vertices are exactly the three patch vertices, in order, carrying exactly the varyings the
 * vertex shader wrote, so the vertex shader alone produces the stream the TES would and the draw is the existing one.
 *
 * The coord is not pattern-matched: the TES NIR is cloned three times, gl_TessCoord is replaced by the constant (1,0,0),
 * (0,1,0), (0,0,1), Mesa folds the body (nir_opt_constant_folding/algebraic/dce and friends), and the *result* is checked
 * to be an identity copy of the patch inputs. That is what the design note asks for ("check by constant-folding the coord,
 * not by pattern-matching arithmetic"), and it is why this file needs NIR while mali_gs.c works on SPIR-V words.
 *
 * What this is not: a tessellator. Levels above 1, a non-identity TCS, quads/isolines, point mode, patch inputs used as patch-level
 * values, or any TES that moves a vertex are all refused here with a named reason; they need lane T2's tessellator chain.
 * tessellationShader stays false (mali_physical_device.c), so no application can reach this path; MALI_EXPERIMENTAL_TESS=1
 * is the development gate the tess_* tests use and it never advertises anything. */
#ifndef MALI_TESS_H
#define MALI_TESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline bool
mali_experimental_tess(void)
{
   const char *e = getenv("MALI_EXPERIMENTAL_TESS");
   return e && !strcmp(e, "1");   /* "12", "1x" and "1 " stay off: this gate decides what the device advertises */
}

/* 1 = the pair is a provable pass-through at levels 1.0 (the caller then compiles the VS and ignores the TES), 0 = refused
 * with a reason in `why` (never empty). Both modules are complete SPIR-V; arch selects the NIR options (11/13/10). */
int mali_tess_passthrough(unsigned arch, const uint32_t *tcs, uint32_t ntcs, const char *tcs_entry,
                          const uint32_t *tes, uint32_t ntes, const char *tes_entry, char *why, size_t whysz);

#endif
