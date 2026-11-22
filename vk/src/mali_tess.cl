/* SPDX-License-Identifier: MIT */
/* Tessellation lane T2, step 1 (notes/tess-design.md § T2): our three tessellator kernels.
 *
 * Mesa's tessellator is a set of OpenCL C routines in src/poly/cl/tessellator.h (`poly_tess_{isoline,tri,quad}_process`),
 * with the three trivial KERNEL(64) entry points AgX uses in src/asahi/libagx/tessellator.cl:24-44. The entry points are
 * the only part a driver owns: they take `poly_tess_params` from the kernel argument buffer and one patch index, and do
 * the work. We cannot reuse AgX's file (it is compiled into `libagx`, which exists only for the AgX driver), so this is
 * the same three wrappers, copied verbatim apart from the names — the bodies stay Mesa's, unmodified, exactly like
 * `vkNir→poly` in the GS lane.
 *
 * Compiled by `vk/poly.sh` with Mesa's own `mesa_clc` against the unmodified `src/poly` headers, next to the
 * `libpoly.spv` that the same configure already produces. The result is `out/<target>/poly/mali_tess.spv`: a SPIR-V
 * module with three kernels that `mali_compile.c`'s compute path turns into panfrost programs (step 3 of the T2 plan).
 *
 * `mode` is `POLY_TESS_MODE_COUNT` for the first pass (write the per-patch index counts) and
 * `POLY_TESS_MODE_WITH_COUNTS` for the second (allocate indices from those counts) — the two-pass shape of
 * `tessellator.cl`'s callers, driven by the 136-byte `struct poly_tess_params` header. */
#include "poly/cl/tessellator.h"

KERNEL(64)
mali_tess_isoline(constant struct poly_tess_params *p, enum poly_tess_mode mode)
{
   uint patch = cl_global_id.x;
   poly_tess_isoline_process(p, patch, mode);
}

KERNEL(64)
mali_tess_tri(constant struct poly_tess_params *p, enum poly_tess_mode mode)
{
   uint patch = cl_global_id.x;
   poly_tess_tri_process(p, patch, mode);
}

KERNEL(64)
mali_tess_quad(constant struct poly_tess_params *p, enum poly_tess_mode mode)
{
   uint patch = cl_global_id.x;
   poly_tess_quad_process(p, patch, mode);
}
