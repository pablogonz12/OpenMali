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

## Honest advertisement (lane/descidx-subset)

`VkPhysicalDeviceVulkan12Features::descriptorIndexing` is now **true by default**. Before this change the driver advertised five
sub-features while the umbrella stayed false, which is a spec violation (VUID-VkPhysicalDeviceVulkan12Features-descriptorIndexing-03830:
when `descriptorIndexing` is false every descriptor-indexing sub-feature must be false). The umbrella is set only because the exact
subset below is implemented; nothing outside it is advertised.

| Sub-feature | Implemented | Advertised | DXVK 3.1.1 |
|---|---|---|---|
| `descriptorBindingPartiallyBound` | yes: unwritten slots are the all-zero hardware Null Descriptor (set memory is zeroed) and an OOB array index is `bcsel`-selected to the same Null Descriptor instead of reading past the set | yes | enables it |
| `descriptorBindingSampledImageUpdateAfterBind` | yes: descriptor memory is GPU-read at submit, writes are plain; layout/pool compatibility enforced | yes | enables it |
| `descriptorBindingUpdateUnusedWhilePending` | yes: per-set `in_use` count, writes/copies to non-UAB bindings dropped while pending | yes | enables it |
| `runtimeDescriptorArray` | yes: `OpTypeRuntimeArray` lowers to the layout's element count; `didx.frag` is an unsized `texture2D tex[]` | yes | enables it |
| `shaderSampledImageArrayNonUniformIndexing` | yes: `nir_lower_non_uniform_access` waterfall after the handle lowering; `didx.frag` uses `nonuniformEXT` | yes | requests it false (we can still advertise) |
| `shader*ArrayDynamicIndexing` (core) | yes | yes (core) | required for FL11_0 |
| `descriptorBindingVariableDescriptorCount` | allocation-sizing only (layout check, `vkCmd` hookup); the shader cannot see the allocate-time count | **no** | never uses it |
| `shaderUniform/StorageBuffer/StorageImage/*TexelBufferArrayNonUniformIndexing` | yes for buffer/SSBO/storage-image via the same waterfall, but UAB and the 500000 limits are not honoured | **no** | requests all false |
| `descriptorBindingStorageImage/StorageBuffer/*TexelBufferUpdateAfterBind` | no (types outside the image/sampler subset) | **no** | requests all false |

DXVK only marks `descriptorIndexing` itself `require=true` in `dxvk_device_info.cpp`; every sub-feature is `require=false` and is
enabled only when the driver reports support. A subset therefore satisfies DXVK: it never uses variable descriptor count, and its
only descriptor-indexing layout is a sampler array with UAB + Unused-While-Pending + Partially-Bound.

Limits still cap the advertised subset: `maxPerStageDescriptorUpdateAfterBindSamplers/SampledImages` are 500000 but the actual set
is limited by the 1M-slot layout cap and the 1 GiB pool, and buffer/storage-image arrays keep the low 4/12 counts.

### Variable descriptor count (deliberately withheld)
`VkDescriptorSetVariableDescriptorCountAllocateInfo` sizing and `maxVariableDescriptorCount` work, but the compiler only knows the
layout's maximum count. An index in `[allocate_count, layout_count)` would read a slot that was never allocated, so the feature is
not advertised. Making it honest needs the per-set count (or a descriptor address table) visible to the shader; DXVK does not use it.

### Tests and scope
Host-only, no hardware claims. `t_desc` `test_descidx_oob` compiles `didx.frag` with and without `PARTIALLY_BOUND` and requires the
output to differ (mutation: removing the `bcsel` makes them identical and fails); it also asserts the flag reaches `mali_lbinding`
and survives `mali_pipeline_layout_flatten`. `t_vk_info` asserts the advertised vk12 set equals the implemented subset (umbrella on,
variable count and all buffer/storage/texel sub-features off). `t_vk_feat --only didxoob` draws a partially-bound 8-element array
with four written slots and an index of 64; the pixel check (hardware only, fake backend prints skipped) requires zero for both the
unwritten and the out-of-range cases.
