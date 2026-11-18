# Mesa patch series on top of the pinned mesa-26.3.0 (third_party/mesa, untracked)

Upstream commits after the 26.3.0 tag that we carry (bi_ backend, not kraid). Apply with `vk/apply-mesa-patches.sh [MESA_SRC]`
(idempotent: already-applied patches are skipped), then re-run ninja in the Mesa build trees.

| # | upstream | what |
|---|----------|------|
| 0001 | ea40598a6 | va_optimize: out-of-bounds read when propagating swizzles on a pseudo instruction |
| 0002 | 555d65ac9 | load_num_subgroups from nir_load_workgroup_size (variable workgroup size) |

Checked and NOT carried: ee34430a4 (push_ubo OOB) and d0379e79f (RA spill cost) are already in the 26.3.0 tag.
Perf commits (1fe05a796, aa0c65155, da2d47315) need a full pin bump (API changes in mali_compile.c) and are not part of this series.
Any change here changes generated code: bump MALI_COMPILE_VERSION and re-bless (`check.sh --bless`).
