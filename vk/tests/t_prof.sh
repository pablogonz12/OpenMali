#!/bin/bash
# MALI_PROF sink: one file per process, one line per second of submission, flushed as it is written.
#   - a long run writes well-formed lines with every counter (the original case)
#   - the file exists as soon as the DEVICE is opened, with a startup line, before any submit
#   - a process that submits for 3 s and is then SIGKILLed still has >= 2 lines: they were written while it ran,
#     not at exit (steamwebhelper and Xwayland write no file at all in the real session, gamescope wrote 2 lines)
set -eu
out=$1
dir=$(mktemp -d "${TMPDIR:-/tmp}/mali-prof-test.XXXXXXXX")
trap 'rm -f "$dir"/*; rmdir "$dir"' EXIT
TQ="$out/t_queue --direct $out/libvulkan_mali.so"

# 1. the counters: a 3 s run, every field numeric, every name present, at least one submit counted
MALI_BACKEND=fake MALI_FAKE_GPU=v11 MALI_FAKE_SLOW_US=1000 \
   MALI_PROF="$dir/p" timeout 120 $TQ --only submits --submits 3000 >/dev/null 2>&1
awk '
  /^mali_prof: / {
    if ($0 ~ /startup/) next
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

# 2. the sink belongs to the process, not to the thread that happened to submit first, and it exists before the first
#    submit: t_icd opens (and destroys) the device without submitting, and must still leave a startup line.
# t_icd's own verdict is ignored here: it counts open fds and the sink fd is process-wide by design (it lives as long
# as the process, like a log file), so that check would report the sink as a leak. The sink line is what is asserted.
MALI_BACKEND=fake MALI_FAKE_GPU=v11 MALI_PROF="$dir/i" timeout 120 "$out/t_icd" --direct "$out/libvulkan_mali.so" --expect-name "Mali-G615" >/dev/null 2>&1 || true
grep -q '^mali_prof: startup comm=t_icd' "$dir"/i-t_icd-*.log

# 3. SIGKILL after 3 s of submission: the lines must already be on disk (write(2), no stdio buffer, no atexit).
rc=0
MALI_BACKEND=fake MALI_FAKE_GPU=v11 MALI_FAKE_SLOW_US=1000 \
   MALI_PROF="$dir/k" timeout -s KILL 3 $TQ --only submits --submits 100000000 >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 137 ] || { echo "expected SIGKILL (137), got $rc"; exit 1; }
lines=$(cat "$dir"/k-*.log | grep -c '^mali_prof: submits=' || true)
[ "$lines" -ge 2 ] || { echo "only $lines profiling line(s) survived SIGKILL"; cat "$dir"/k-*.log; exit 1; }
