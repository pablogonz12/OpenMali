#!/bin/bash
# Megabonk driver comparison (ours vs bridge vs panvk), same protocol for each.
#   megabonk-compare.sh run <ours|bridge|panvk> [measure_s=90] [warmup_s=30]   # needs a phone; NOT used by tests
#   megabonk-compare.sh compare <rundir>...                                   # parse only, exit 1 if any run has problems
# `run` wraps mali-gaming scripts/bench.sh (30 s warm-up, 90 s measure, pad A every 200 ms), then writes label.txt and
# driver-identity.txt (guest ICD + vulkaninfo) so the driver is proven, not assumed. Select the driver BEFORE running
# (session env / driver-setup); the label is only an expectation: `compare` fails if evidence disagrees or is missing.
# Run one phone job at a time, stop at 43 C (the run records thermal pre/post).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
MG=${MALI_GAMING:-$HOME/Projects/mali-gaming}
case "${1:-}" in
  compare) shift; exec python3 -I "$HERE/megabonk_compare.py" "$@" ;;
  run)
    d=${2:?ours|bridge|panvk}; case $d in ours|bridge|panvk) ;; *) echo "bad driver $d" >&2; exit 2;; esac
    "$MG/scripts/bench.sh" "cmp-$d" "${3:-90}" "${4:-30}" || exit $?
    out=$(ls -dt "$MG"/runs/*-cmp-"$d" | head -1)
    echo "$d" > "$out/label.txt" || exit $?
    python3 -I - "$out" "$MG/tools/droiddeckctl" "$MG/tools" <<'PY' || exit $?
import sys, json, subprocess
from pathlib import Path
sys.path.insert(0, sys.argv[3])   # -I ignores PYTHONPATH: add the tools dir explicitly
import fpsdip_snapshot as s
out, ctl = Path(sys.argv[1]), sys.argv[2]
r = subprocess.run(["python3", "-I", ctl, "guest", "--", "sh", "-c", s.GUEST_DRIVER_SCRIPT], capture_output=True, text=True)
try:
    text = (json.loads(r.stdout).get("stdout") or {}).get("text", "")
except ValueError:
    text = ""
(out / "driver-identity.txt").write_text(text)  # empty => compare reports NO DRIVER EVIDENCE
if r.returncode != 0:
    print(f"driver identity capture failed (droiddeckctl exit {r.returncode}): {r.stderr.strip()[:200]}", file=sys.stderr)
    sys.exit(1)
PY
    echo "run dir: $out (now: $0 compare $out)" ;;
  *) sed -n 2,5p "$0" >&2; exit 2 ;;
esac
