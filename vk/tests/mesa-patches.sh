#!/bin/bash
# SPDX-License-Identifier: MIT
# Every vk/mesa-patches/*.patch must apply (or already be applied) in order. Checked on a scratch copy of just the files the patches touch,
# so the shared Mesa tree is never modified. The patches only take effect in the Mesa roll-in (apply-mesa-patches.sh + rebuild of the Mesa trees).
# Usage: mesa-patches.sh [MESA_SRC]; exit 0 = ok or no Mesa tree (skip).
set -eu
V=$(cd "$(dirname "$0")/.." && pwd)
M=${1:-${MESA_SRC:-$V/../third_party/mesa}}
[ -d "$M/src" ] || { echo "skip (no Mesa tree at $M)"; exit 0; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
for f in $(cat "$V"/mesa-patches/*.patch | sed -n 's#^+++ b/##p' | sort -u); do
   [ -f "$M/$f" ] || continue
   mkdir -p "$T/$(dirname "$f")"; cp "$M/$f" "$T/$f"
done
out=$("$V/apply-mesa-patches.sh" "$T" 2>&1) || { echo "$out"; exit 1; }
echo "mesa patches: $(ls "$V"/mesa-patches/*.patch | wc -l) apply cleanly in order"
