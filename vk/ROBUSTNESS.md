# Buffer robustness: what an out-of-bounds access does (2026-11-19)

Advertised since this change: `robustBufferAccess = true`, `robustBufferAccess2 = true`
(`VK_EXT_robustness2`, next to `nullDescriptor`). **Not** advertised: `robustImageAccess2`,
`robustBufferAccess`'s image half.

## What happens today, by access kind

| access | lowering | result |
|---|---|---|
| SSBO/UBO **load** past the descriptor range | `robust_zero_loads` (`src/mali_descriptor_lower.c`): compare `offset + size - 1` with the descriptor's size word, `if` around the load, result is `0` | returns 0, no read outside the range |
| SSBO **store** past the range | `nir_lower_robust_access` (Mesa), filtered to buffers | discarded (the write is predicated off) |
| SSBO **atomic** past the range | same | discarded; the atomic's return value is 0 |
| `array.length()` (`get_ssbo_size`/`get_ubo_size`) | `lower_size`: reads word 1 of the 32-byte Buffer descriptor, addressed through the table VAs in the push block | real range length, no backend assert (this is what used to make a robust compile fail) |
| **vertex fetch** past the vertex buffer | none in the compiler | Mali returns 0 for reads past the bound VkBuffer (X8 evidence below) |
| image load/store, shared memory | none | not covered — that is why `robustImageAccess2` stays false |
| far OOB address (1 GiB, 0xFFFFFFF0 words) | the same predication | never dereferenced: the size word decides |
| **null descriptor** (robustness2 `nullDescriptor`, `VK_NULL_HANDLE` written into a set) | `mali_descriptor.c:406`: the hardware Null Descriptor (Type = 0) is written instead of a resource | loads return **0**, stores and atomics are swallowed by the hardware, no fault; `t_vk_feat nulld` covers 7 descriptor types |

Summary: an out-of-bounds buffer load returns **0** (not clamped, not the neighbouring word); an
out-of-bounds store or atomic is **dropped**; a null descriptor reads **0** and swallows writes; nothing
faults. Out-of-bounds **images** are not checked (`robustImageAccess2` stays false).

Without the feature enabled the lowering does not run at all: an OOB buffer load reads whatever is at
that address in the 64-bit VA space — inside the BO it is garbage, outside it is a GPU page fault that
kills the submission (`VK_ERROR_DEVICE_LOST`). There is no clamping in hardware.

## Evidence

- `tests/t_vk_robust.c` is the end-to-end probe (hardware, not host): OOB SSBO/UBO read = 0, OOB
  store/atomic discarded (canaries in the same `VkBuffer` past the range stay untouched), `length()`,
  vertex fetch 8..15 of a 16-vertex draw from an 8-word buffer = 0, and a "wild" case with 1 GiB and
  `0xFFFFFFF0` indexes that must neither fault nor hang. Run on the X8 (G715) when the lowering landed;
  `check.sh` runs it on the fake backend with `MALI_TEST_FORCE_ROBUST=1` (results are skipped there —
  the fake backend does not execute shaders; only the API path is exercised).
- `tests/t_desc.c` is the host test: a robust compile must succeed, must differ from the non-robust
  binary, must place the size table at the expected push offset and must add FAU words. Mutation check:
  drop `robust_zero_loads`/the buffer filter bit and the "must differ" assertion fails.
- `tests/t_vk_feat.c` asserts the two advertised robustness2 bits agree with each other and that
  `robustImageAccess2` is false; the `nulld` case reads and writes a null-descriptor binding of every
  type and requires zero/absorbed results (`--only nulld`, both fake GPUs).
- Mutation check (host): skipping the `lower_size` pass makes `t_desc` abort in the Bifrost backend on
  the unlowered `get_ssbo_size` intrinsic, so the host test does catch a missing lowering.

## Rules

- `robustImageAccess2` may only be set when image loads/stores are predicated too.
- The lowering must stay on the *enabled feature* (`mali_pipeline.c`: `robustBufferAccess ||
  robustBufferAccess2`), not on an env var: a device that does not enable the feature must get the raw
  (fast) path, and one that does must get the checks. `MALI_TEST_FORCE_ROBUST` exists only for the
  hardware probe.
- DXVK relies on both bits at feature level 11_0; with them and `MALI_ADVERTISE_GS`/`..._CULL`, the
  remaining DXVK gap is the `descriptorIndexing` umbrella (see `vk/FEATURE-AUDIT.md`).
