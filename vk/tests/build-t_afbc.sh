#!/bin/bash
# SPDX-License-Identifier: MIT
# Host-only AFBC layout test: plain cc, no Mesa build tree needed. Usage: build-t_afbc.sh
set -euo pipefail
V=$(cd "$(dirname "$0")/.." && pwd); OUT=$V/out/native; mkdir -p "$OUT"
${CC:-cc} -std=gnu11 -Wall -Wextra -O1 -g "$V/tests/t_afbc.c" -o "$OUT/t_afbc"
timeout -k 5 60 "$OUT/t_afbc"
