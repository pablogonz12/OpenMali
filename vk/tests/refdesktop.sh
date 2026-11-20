#!/bin/bash
# SPDX-License-Identifier: MIT
# Desktop-GPU reference frame for the reference scene (docs/desktop-gpu.md). Not a check.sh case: it needs a desktop GPU
# and a Vulkan implementation that is not ours, and it is the *other* side of a comparison, not a pass/fail test.
#
#   tests/refdesktop.sh [outdir]        -> <outdir>/ref.ppm, <outdir>/ref.json
#
# REF_ICD=<icd.json> picks the implementation explicitly; without it the loader's default device is used (the JSON
# records which device rendered, so the output is self-describing). All implicit layers are disabled: this machine's
# MangoHud/Steam/LSFG layers change results and one of them does not load.
set -eu
V=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$V/out/ref}
shift || true
BIN=$V/out/native/t_vk_ref
[ -x "$BIN" ] || { echo "no $BIN (run vk/build.sh)"; exit 2; }
mkdir -p "$OUT"
env -u DISPLAY VK_LOADER_LAYERS_DISABLE=~all~ ${REF_ICD:+VK_ICD_FILENAMES=$REF_ICD} \
   "$BIN" --spv-dir "$V/shader" --out "$OUT/ref.ppm" --json "$OUT/ref.json" "$@"
