#!/bin/bash
# SPDX-License-Identifier: MIT
# The lane artefact: build the aarch64 X11+Wayland (glibc-wsi) ICD from THIS checkout and print
# path + sha256 + commit, so a deploy can be checked against the branch head before it happens.
#
#   tools/build-artefact.sh          build, then print
#   tools/build-artefact.sh --check  no build: print what is on disk and say whether it is the head
#
# One artefact per branch: the same run prints the commit it came from, and refuses to bless a
# dirty tree (a dirty tree means the .so is NOT what the branch head contains).
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
TARGET=a64-glibc-wsi
OUT=$R/vk/out/$TARGET/libvulkan_mali.so

commit=$(git -C "$R" rev-parse HEAD)
branch=$(git -C "$R" rev-parse --abbrev-ref HEAD)
# only the driver tree matters: docs/skill edits elsewhere do not change the .so
dirty=$(git -C "$R" status --porcelain --untracked-files=no -- vk | wc -l)

if [ "${1:-}" = "--check" ]; then
   [ -f "$OUT" ] || { echo "no artefact at $OUT (run tools/build-artefact.sh)" >&2; exit 1; }
else
   cd "$R/vk"
   ./cross/build-cross.sh glibc-wsi >/tmp/build-artefact.log 2>&1 || { tail -20 /tmp/build-artefact.log; exit 1; }
   grep -E 'warning:|error:' /tmp/build-artefact.log | grep -v 'tools/shaderdb' || true
fi

sum=$(sha256sum "$OUT" | cut -d' ' -f1)
f=$(file -b "$OUT")
echo "branch   $branch"
echo "commit   $commit"
echo "artefact $OUT"
echo "sha256   $sum"
echo "file     $f"
case "$f" in *aarch64*) ;; *) echo "NOT aarch64: do not deploy this" >&2; exit 1 ;; esac
if [ "$dirty" != 0 ]; then
   echo "WARNING: $dirty tracked file(s) under vk/ modified: this .so is not the branch head, do not deploy it" >&2
   exit 1
fi
echo "OK: deploy $OUT at $sum from $branch@${commit:0:7}"
