# VK_EXT_descriptor_indexing: staged plan

Honest `descriptorIndexing` needs the whole spec minimum set (see PR #166 notes: a flag-only shim lies). Stages, in lane-hours:

| # | Stage | Hours | Status |
|---|-------|-------|--------|
| 1 | Per-binding flags + UAB_POOL layout/pool compat, validated, `MALI_DESCIDX_FLAGS=1` (default off, advertises nothing) | 3 | this PR (`mali_descidx.h`) |
| 2 | Variable descriptor count: alloc sizing (PR #81) + VkDescriptorSetVariableDescriptorCountAllocateInfo hookup onto stage 1 flags | 1 | #81 |
| 3 | Enforce at write/bind: non-UAB bindings may not be written while set in use, PARTIALLY_BOUND skips unbound slots in the compiler lowering | 3 | todo |
| 4 | Limits query struct: real maxPerStage/maxDescriptorSet UAB limits (500000 for storage buffers/images needs descriptor tables, not 4/24 slot caps) | 8-12 | todo |
| 5 | Dynamic + NonUniform indexing for UBO/SSBO/storage image/texel buffers: SPIR-V NonUniformIndexing capabilities in compiler, waterfall lowering of non-image handles (compile version bump 38 when output changes) | 6-10 | todo |
| 6 | UAB on buffers/storage images/texel buffers (memory is GPU-read at submit so writes are plain; needs tests) | 2-3 | todo |
| 7 | Input-attachment arrays, input attachment NonUniform | 2-3 | todo |
| 8 | Advertise `descriptorIndexing`, CTS dEQP-VK.descriptor_indexing.* on X8, drop shim | 2-3 | todo |

Remaining after stage 1: ~25-35 h. Stage 1 does not change compiler output (no compile-version bump).

## Stage 2 (this PR, reused from #81)
The alloc-sizing and `maxVariableDescriptorCount` code is #81's commit 74f8b53 cherry-picked (not duplicated); here it is gated on
`MALI_DESCIDX_FLAGS=1` (default off: flag ignored, support reports 0, as before). If #81 merges first, rebase drops the duplicate
hunks; the gate is the only delta. Host test: `t_desc` `test_variable_count` (5 mutants killed incl. gate and count check).

## Descriptor tables (stages 3/4/5 design)
Today a shader addresses descriptors through fixed slots (4 sets / 24 slot caps in the lowering). Real UAB limits (500000 per stage)
need one indirection: a per-set table `mali_dtab_entry[binding]` = {slot, stride, count, flags} uploaded next to the set (or
derived from the layout, which the compiler already receives), and lowering `addr = set_va + (slot + idx*stride) * slot_size`.
`vk/src/mali_desctable.h` is the pure core: `mali_dtab_slot` (binding+index -> slot, OOB/missing-binding never yield a slot, so the
lowering clamps to a null slot), `mali_dtab_write_allowed` (stage 3 write-while-in-use rule) and `mali_dtab_may_skip_unwritten`
(PARTIALLY_BOUND). Not wired anywhere: compiler output unchanged, **no compile version bump** in this PR.

Wiring estimates (lane-hours): write enforcement in vkUpdateDescriptorSets + in-use tracking per set 2-3; push table pointer through
the CS (new per-set table VA in the resource table, hot file mali_cs_emit.c) 3-4; compiler lowering for non-constant indices with
bounds clamp 4-6 (bumps compile version to 38 when it first changes output); limits struct + advertise 1-2. Total ~10-15 h on top of the
table core.

## Stage 4 wiring (descidx3 PR 2)
Wired behind `MALI_DESCIDX_FLAGS=1` (default path untouched; goldens/check.sh unchanged): per-layout `bflags`, per-set `var_count`,
`mali_dset_dtab` / `mali_dset_binding_va` (address table, variable count honoured), `mali_dset_use` + drop-and-count of writes/copies to
non-UAB bindings while a set is in use. NOT wired: `mali_dset_use` callers in queue submit/retire (hot files mali_cmd.c/queue; ~2 h) and the
shader-side access path (needs compiler lowering, compile version 38, separate default-off PR, ~6-8 h).

## Shader-side access path (descidx4 PR)
`mali_compile_layout::descidx_clamp` (set from `MALI_DESCIDX_FLAGS=1` at the two `robust_access` sites in `mali_pipeline.c`): dynamic indices into
image/sampler descriptor arrays are clamped with `umin(idx, count-1)` in `deref_handle`. Hashed into the cache key only when nonzero, so default
output and keys are unchanged: `MALI_COMPILE_VERSION` stays 22 (no bump). NonUniform waterfall and buffer-array indexing were already handled.
Limit: the bound is the layout's max count, not the allocate-time variable count. NOT done: `mali_dset_use` callers in queue submit/retire.
