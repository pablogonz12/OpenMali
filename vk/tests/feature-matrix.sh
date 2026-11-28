#!/bin/bash
# SPDX-License-Identifier: MIT
# Capture defaults, not inherited experimental settings or host ICDs/layers.
set -eu
V=$(cd "$(dirname "$0")/.." && pwd)
OUT=$1
g=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for name in ${!MALI_EXPERIMENTAL_@}; do unset "$name"; done
unset VK_INSTANCE_LAYERS VK_ADD_DRIVER_FILES VK_LOADER_DRIVERS_SELECT VK_LOADER_DRIVERS_DISABLE
export VK_DRIVER_FILES="$OUT/mali_icd.json" VK_ICD_FILENAMES="$OUT/mali_icd.json"
export VK_LOADER_LAYERS_DISABLE='~all~' MALI_BACKEND=fake MALI_FAKE_GPU="$g"
vulkaninfo --json=0 -o "$tmp/probe.json" >"$tmp/probe.log" 2>&1 || {
   cat "$tmp/probe.log"; echo "feature-matrix: vulkaninfo probe failed"; exit 1;
}
test -s "$tmp/probe.json" || { echo "feature-matrix: missing probe data"; exit 1; }
python3 -I "$V/../tools/feature-matrix/feature_matrix.py" check --gpu "$g" --vulkaninfo "$tmp/probe.json"
