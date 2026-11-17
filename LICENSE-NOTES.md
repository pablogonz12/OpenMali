# Licence notes (for the project owner to decide)

Status: **decided: MIT** (top-level `LICENSE`, "Copyright (c) 2026 the mali-open authors"). This is an inventory and a recommendation, not legal advice.

## Our code

Every file in `vk/src`, `libkbase/src`, `libkbase/include`, `vk/shader` and the scripts carries `SPDX-License-Identifier: MIT`.
A grep for `Copyright`, `GPL` and licence text in those trees found **no copied third-party notices**. The packaging script writes
a `LICENSE` (MIT, "Copyright (c) 2026 the mali-open authors") into each zip, so that name is already public in artefacts.
**The owner must confirm the copyright holder line and the licence before any public release.**

**Recommendation: MIT.** It is compatible with everything linked (Mesa is MIT) and matches what the code headers already say.

## Third-party code

| Component | Licence | How used | Obligation |
|---|---|---|---|
| Mesa 26.3.0 (NIR, spirv_to_nir, Bifrost/Valhall compiler, `vk_*` runtime, WSI, `pan_*` helpers) | MIT | Linked statically, unmodified (pinned tag and commit in `third_party/MESA-SOURCE.txt`) | Ship Mesa's notices (`NOTICE-MESA.txt` in the zip does this) |
| Parts of Mesa under other permissive licences (blake3 CC0/Apache-2.0, xxhash BSD-2, SPIR-V headers Khronos MIT-style, others in `third_party/mesa/licenses/`) | Permissive | Linked as part of Mesa | Same notices |
| Mesa `licenses/` dir also holds GPL-1.0-or-later and GPL-2.0-only texts | GPL | Sit in Mesa's tree for specific files (build tools, scripts). **Check each is not linked into the `.so`** | Verify with `readelf`/link map before release |
| libdrm, libz, libzstd, libstdc++, libm, libc | MIT / zlib / BSD / GPL+exception / LGPL | Dynamic `NEEDED` entries of the ICD | Normal dynamic linking |
| libelf (LGPL/GPL) | LGPL | On the borrowed PanVK link line only. `.so` must not list it in NEEDED (not present in the current a64-glibc build) | Keep the `check.sh` assertion |
| LLVM | Apache-2.0 with LLVM exception | Host build tool `mesa_clc` only. Not in the ICD | None for the binary |
| Vulkan CTS / dEQP (`third_party/cts-src`) | Apache-2.0 | Tests only, never shipped or linked | None; keep out of release zips |
| Vulkan-Headers / `vk.xml`, glslang, SPIRV-Tools | Apache-2.0 / MIT | Build time (shader generation); generated SPIR-V arrays are in `vk/src/*_spv.h` | Notice if SPIRV-Tools is bundled |
| PanVK (`src/panfrost/vulkan`) | MIT | Oracle only: run and compared, not linked, not copied | None |
| Arm kbase kernel UAPI | GPL-2.0 WITH Linux-syscall-note (kernel headers) | Used as a reference only. `libkbase/include/kbase_uapi.h` is written by us (ioctl numbers and layouts are interface facts) | See flag below |
| Android NDK (bionic builds) | Apache-2.0 / LLVM | Build time | None |

## Flags for the owner

1. `kbase_uapi.h` says it is written from the Arm headers plus a community PanVK-on-kbase project. Layouts follow kernel ABI, but
   someone should confirm no struct text was pasted verbatim from the GPL-licensed headers.
2. `libkbase` cites "Mesa genxml v10, study only" for CS instruction layouts. Mesa is MIT, so this is allowed, but keep the attribution in `NOTICE-MESA.txt`.
3. Mesa's source tree, build trees and the CTS are untracked. Do not commit them. Publish the pinned tag and commit instead.
4. The Android half of a bundle zip is a stock PanVK driver (MIT, third-party). Credit it in the release notes.
5. Check that no GPL file from Mesa's tree ends up in the link line before tagging a release.
