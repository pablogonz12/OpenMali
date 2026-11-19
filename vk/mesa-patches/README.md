# Mesa patch series on top of the pinned mesa-26.3.0 (third_party/mesa, untracked)

Upstream commits after the 26.3.0 tag that we carry (bi_ backend, not kraid). Apply with `vk/apply-mesa-patches.sh [MESA_SRC]`
(idempotent: already-applied patches are skipped), then re-run ninja in the Mesa build trees.

| # | upstream | what |
|---|----------|------|
| 0001 | ea40598a6 | va_optimize: out-of-bounds read when propagating swizzles on a pseudo instruction |
| 0002 | 555d65ac9 | load_num_subgroups from nir_load_workgroup_size (variable workgroup size) |
| 0003 | 03bc0ff2 | pan/compiler: missing discard after atest on v9 first gen (G57/G77); changes `pan_nir_lower_fs_outputs(shader, skip_atest, arch, model, fragcolor_nr_cbufs)` |
| 0004 | a2de3317 | panvk/csf: keep VT self-waits needed by another subqueue (panvk only, not linked) |
| 0005 | 0a65740a | panvk: flush after copy_fbd (Mesa 26.2.7 fix; panvk only, not linked) |
| 0006 | 75217eab | panvk: restore zero flush_id after copy_fbd (part of the same fix; needs 0005) |
| 0007 | 1fe05a79 | pan/bi: run CSE before load_store_vectorize |

Checked and NOT carried: ee34430a4 (push_ubo OOB) and d0379e79f (RA spill cost) are already in the 26.3.0 tag.
Still not carried: aa0c65155 (fpow to fmul chains), da2d47315 (drop bi_lower_divergent_indirects), fa252c1c (nir idiv).
Any change here changes generated code: bump MALI_COMPILE_VERSION and re-bless (`check.sh --bless`).
