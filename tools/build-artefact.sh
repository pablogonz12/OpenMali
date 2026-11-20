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
dirty=$(git -C "$R" status --porcelain --untracked-files=no -- vk libkbase | wc -l)

if [ "${1:-}" = "--check" ]; then
   [ -f "$OUT" ] || { echo "no artefact at $OUT (run tools/build-artefact.sh)" >&2; exit 1; }
else
   cd "$R/vk"
   log=$(mktemp "${TMPDIR:-/tmp}/build-artefact.XXXXXX")
   trap 'rm -f "$log"' EXIT
   ./cross/build-cross.sh glibc-wsi >"$log" 2>&1 || { tail -20 "$log"; exit 1; }
   grep -E 'warning:|error:' "$log" | grep -v 'tools/shaderdb' || true
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
   echo "WARNING: $dirty tracked file(s) under vk/ or libkbase/ modified: this .so is not the branch head, do not deploy it" >&2
   # A lane builds dirty on purpose (tools/hostcheck sets this); a deploy must not. Everything else stays strict.
   [ "${MALI_ARTEFACT_ALLOW_DIRTY:-0}" = 1 ] || exit 1
fi
prov=$OUT.provenance
# Provenance: the hash of the .so this commit actually built. `--check` must not approve an artefact that was
# built from a different commit or replaced afterwards, so both records have to exist and match.
if [ "${1:-}" = "--check" ]; then
   [ -f "$prov" ] || { echo "no provenance record at $prov (rebuild with tools/build-artefact.sh)" >&2; exit 1; }
   read -r p_commit p_sum <"$prov" || true
   [ "${p_commit:-}" = "$commit" ] || { echo "provenance commit ${p_commit:-<none>} != HEAD ${commit:0:7}: rebuild before deploying" >&2; exit 1; }
   [ "${p_sum:-}" = "$sum" ] || { echo "provenance sha256 ${p_sum:-<none>} != artefact $sum: the .so was replaced after the build" >&2; exit 1; }
else
   printf '%s %s\n' "$commit" "$sum" >"$prov"
fi
echo "OK: deploy $OUT at $sum from $branch@${commit:0:7}"
