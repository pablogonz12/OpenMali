#!/bin/bash
# SPDX-License-Identifier: MIT
# Apply vk/mesa-patches/*.patch to the pinned Mesa tree. Idempotent: a patch that is already applied (reverse applies cleanly) is skipped.
set -eu
D=$(cd "$(dirname "$0")" && pwd)
M=${1:-${MESA_SRC:-$D/../third_party/mesa}}
for p in "$D"/mesa-patches/*.patch; do
   if patch -p1 -R -s --dry-run -d "$M" <"$p" >/dev/null 2>&1; then echo "skip   $(basename "$p") (already applied)"; continue; fi
   patch -p1 -s --dry-run -d "$M" <"$p" >/dev/null || { echo "FAIL   $(basename "$p") does not apply to $M"; exit 1; }
   patch -p1 -s -d "$M" <"$p"; echo "apply  $(basename "$p")"
done
echo "note: the disk shader cache key does not include the applied patch set. Rebuild and deploy the patched driver, stop every running copy of the old driver, and only THEN clear \$MALI_SHADER_CACHE (default ~/.cache/mali-gaming/shaders); an old driver still running can refill the cache with unpatched shaders. Two builds that must run side by side need separate caches."
