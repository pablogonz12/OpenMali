#!/bin/bash
# SPDX-License-Identifier: MIT
# Host test for the tiler hierarchy mask derivation (libkbase kb_tiler_hmask_from_tile, pan_desc.c rule).
set -e
V=$(cd "$(dirname "$0")/.." && pwd); KB=$V/../libkbase; OUT=${OUT:-$V/out/native}; mkdir -p "$OUT"
cc -std=gnu11 -D_GNU_SOURCE -O1 -Wall -Wextra -Werror -I"$KB/include" -I"$KB/src" -o "$OUT/t_hmask" "$V/tests/t_hmask.c" "$KB/src/draw.c"
timeout 120 "$OUT/t_hmask"
