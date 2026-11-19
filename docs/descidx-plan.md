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
