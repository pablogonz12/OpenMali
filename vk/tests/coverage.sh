#!/bin/bash
# SPDX-License-Identifier: MIT
# gcov line coverage of the driver sources under the offline (fake backend) check. Usage: vk/tests/coverage.sh [--no-build]
# Builds out/native-cov with --coverage -O0 (MALI_COV=1), runs check.sh on it, prints "lines% of-lines file" sorted ascending.
# Run it through mali-slot. libkbase bo/queue/sync/heap/ctx and mali_backend_kbase.c read 0% by design here: the vk ICD uses the fake
# backend, the real kbase path only runs on a phone (libkbase/check.sh covers libkbase against its own fake).
set -uo pipefail
V=$(cd "$(dirname "$0")/.." && pwd)
export MALI_COV=1
OBJ=$V/out/native-cov/obj
[ "${1:-}" = --no-build ] || find "$OBJ" -name '*.gcda' -delete 2>/dev/null
"$V/check.sh" "$@" | tail -3
cd "$OBJ" || exit 1
for g in *.gcda; do
   gcov -n -o . "$g" 2>/dev/null | awk '/^File/{f=$2} /Lines executed/{ if (f ~ /\.c.$/ && f !~ /gen/) { split($2,a,":"); print a[2], $4, f }; f=""}'
done | sed "s#'$V/../##;s#'##g" | sort -n
