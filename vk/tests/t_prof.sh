#!/bin/bash
set -eu
out=$1
dir=$(mktemp -d "${TMPDIR:-/tmp}/mali-prof-test.XXXXXXXX")
trap 'rm -f "$dir"/*; rmdir "$dir"' EXIT
MALI_BACKEND=fake MALI_FAKE_GPU=v11 MALI_FAKE_SLOW_US=1000 \
   MALI_PROF="$dir/p" timeout 120 "$out/t_queue" --direct "$out/libvulkan_mali.so" --only submits --submits 3000 >/dev/null 2>&1
awk '
  /^mali_prof: / {
    delete found
    for (i = 2; i <= NF; i++) {
      split($i, a, "=")
      if (a[2] !~ /^[0-9]+$/) exit 1
      found[a[1]] = a[2]
    }
    split("submits presents submit_ns wait_ns d2c_count d2c_bos d2c_bytes d2c_ns c2d_count c2d_bos c2d_bytes c2d_ns present_ns", names)
    for (i = 1; i <= 13; i++) if (!(names[i] in found)) exit 1
    if (found["submits"] > 0) ok = 1
  }
  END { if (!ok) exit 1 }
' "$dir"/p-*.log
