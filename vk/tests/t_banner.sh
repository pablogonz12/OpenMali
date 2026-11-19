#!/bin/sh
# MALI_STATUS_FILE: vkCreateInstance appends exactly one "mali-open <sha> pid <pid> exe <name>" line; unset = no file.
# usage: t_banner.sh <out dir with libvulkan_mali.so and t_icd>
set -u
OUT=${1:?out dir}
T=$(mktemp -d "${TMPDIR:-/tmp}/banner.XXXXXX") || { echo "FAIL mktemp"; exit 1; }
trap 'rm -rf "$T"' EXIT
run() { env MALI_BACKEND=fake MALI_FAKE_GPU=v13 "$@" "$OUT/t_icd" --direct "$OUT/libvulkan_mali.so" --expect-name Mali-G925 >/dev/null 2>&1; }
run MALI_STATUS_FILE="$T/s" || { echo "FAIL t_icd run"; exit 1; }
[ "$(wc -l <"$T/s")" = 1 ] || { echo "FAIL want 1 line, got: $(cat "$T/s")"; exit 1; }
grep -Eq '^mali-open [0-9a-f]+|^mali-open unknown ' "$T/s" && grep -Eq ' pid [0-9]+ exe t_icd$' "$T/s" || { echo "FAIL format: $(cat "$T/s")"; exit 1; }
run MALI_STATUS_FILE="$T/s" || exit 1
[ "$(wc -l <"$T/s")" = 2 ] || { echo "FAIL append"; exit 1; }
run MALI_BANNER=1 || exit 1
run MALI_STATUS_FILE= || exit 1
[ "$(wc -l <"$T/s")" = 2 ] || { echo "FAIL empty env wrote"; exit 1; }
run MALI_STATUS_FILE="$T/nodir/x" || { echo "FAIL unwritable path must not break instance"; exit 1; }
mkfifo "$T/fifo" || { echo "FAIL mkfifo"; exit 1; }
timeout 20 env MALI_BACKEND=fake MALI_FAKE_GPU=v13 MALI_STATUS_FILE="$T/fifo" "$OUT/t_icd" --direct "$OUT/libvulkan_mali.so" --expect-name Mali-G925 >/dev/null 2>&1 || { echo "FAIL FIFO without reader blocked or broke the instance"; exit 1; }
echo "banner ok"
