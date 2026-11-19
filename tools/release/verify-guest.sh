#!/bin/bash
# SPDX-License-Identifier: MIT
# Check the driver an installed guest is really running against a release manifest.
#
#   tools/release/verify-guest.sh <serial> [--manifest FILE] [--ctl PATH] [--keep]
#
# Reads manifest.json from tools/release/build-release.sh, pushes the aarch64 probes from its probe/
# directory into the guest, finds the libvulkan_mali/panfrost library the session actually maps, and
# compares: library sha256, driverInfo (and its sha256), the four advertised surface extensions, and
# the t_dxvk_caps feature list / missing list.
#
# Needs droiddeckctl (default ~/Projects/mali-gaming/tools/droiddeckctl, override DROIDDECKCTL) and a
# running guest session. Read-only: never starts/stops the session, never changes prefs or env.
set -euo pipefail

SELF=$(cd "$(dirname "$0")" && pwd)
REPO=$(git -C "$SELF" rev-parse --show-toplevel)
MAN=""
CTL=${DROIDDECKCTL:-$HOME/Projects/mali-gaming/tools/droiddeckctl}
KEEP=0
SERIAL=""

die() { echo "verify-guest: $*" >&2; exit 2; }

while [ $# -gt 0 ]; do
   case $1 in
      --manifest) MAN=${2:?--manifest needs a value}; shift 2 ;;
      --ctl) CTL=${2:?--ctl needs a value}; shift 2 ;;
      --keep) KEEP=1; shift ;;
      -h|--help) sed -n '2,18p' "$SELF/verify-guest.sh" | sed 's/^# \{0,1\}//'; exit 0 ;;
      -*) die "unknown argument: $1" ;;
      *) [ -z "$SERIAL" ] || die "give exactly one serial"; SERIAL=$1; shift ;;
   esac
done
[ -n "$SERIAL" ] || die "usage: verify-guest.sh <serial> [--manifest FILE] [--ctl PATH]"
[ -x "$CTL" ] || die "no droiddeckctl at $CTL (set DROIDDECKCTL)"

if [ -z "$MAN" ]; then
   MAN=$(find "$REPO/tools/release/out" -name manifest.json -type f -printf '%T@ %p\n' 2>/dev/null \
         | sort -nr | head -1 | cut -d' ' -f2- || true)
fi
[ -n "$MAN" ] && [ -f "$MAN" ] || die "no manifest found (run tools/release/build-release.sh, or pass --manifest)"
PROBES=$(dirname "$MAN")/probe
for p in t_vk_release t_dxvk_caps; do
   [ -x "$PROBES/$p" ] || die "missing $PROBES/$p (rebuild with tools/release/build-release.sh)"
done

D=/storage/emulated/0/Download/mali_release_verify
W=$(mktemp -d "${TMPDIR:-/tmp}/oc/mali-verify-guest-XXXXXX")
cleanup() { [ "$KEEP" = 1 ] || rm -rf "$W"; }
trap cleanup EXIT

cat >"$W/guest.sh" <<'EOS'
#!/bin/sh
# Runs inside the guest (pushed by verify-guest.sh). $1 = expected sha256 (may be empty).
EXPECTED=${1:-}
D=/storage/emulated/0/Download/mali_release_verify
T=/tmp/mali_release_verify
rm -rf "$T"; mkdir -p "$T"
for f in t_vk_release t_dxvk_caps; do
   [ -f "$D/$f" ] && cp "$D/$f" "$T/$f" && chmod +x "$T/$f"
done

# The library a session process maps is the one actually in use; fall back to installed copies.
candidates=""
add() { [ -n "$1" ] || return 0; case " $candidates " in *" $1 "*) ;; *) candidates="$candidates $1" ;; esac; }
for m in /proc/[0-9]*/maps; do
   p=$(awk '$6 ~ /libvulkan_(mali|panfrost)\.so/ {print $6; exit}' "$m" 2>/dev/null)
   [ -n "$p" ] && add "$p"
done
for root in /tmp /root /home /data /usr/lib; do
   [ -d "$root" ] || continue
   for p in $(find "$root" -maxdepth 6 \( -name libvulkan_panfrost.so -o -name libvulkan_mali.so \) 2>/dev/null | head -8); do
      add "$p"
   done
done

pick=""; first=""
for p in $candidates; do
   [ -f "$p" ] || continue
   s=$(sha256sum "$p" 2>/dev/null | cut -d' ' -f1)
   [ -n "$first" ] || first=$p
   echo "{\"candidate\":\"$p\",\"sha256\":\"$s\"}"
   if [ -n "$EXPECTED" ] && [ "$s" = "$EXPECTED" ]; then pick=$p; break; fi
done
[ -n "$pick" ] || pick=$first
echo "{\"lib\":\"$pick\"}"
[ -n "$pick" ] || { echo '{"error":"no libvulkan_mali/panfrost found in the guest"}'; exit 1; }

cp "$pick" "$T/libvulkan_mali.so"
printf '{"file_format_version":"1.0.1","ICD":{"library_path":"%s","api_version":"1.3.0"}}\n' \
   "$T/libvulkan_mali.so" >"$T/icd.json"
export VK_DRIVER_FILES=$T/icd.json VK_ICD_FILENAMES=$T/icd.json VK_LOADER_LAYERS_DISABLE='~all~'

runprobe() {
   name=$1; shift
   [ -x "$T/$name" ] || { echo "{\"probe\":\"$name\",\"line\":\"missing probe binary\"}"; return 0; }
   "$T/$name" "$@" 2>&1 | sed 's/\\/\\\\/g; s/"/\\"/g; s/^\(.*\)$/{"probe":"'"$name"'","line":"\1"}/'
}

runprobe t_vk_release --direct "$T/libvulkan_mali.so"
runprobe t_dxvk_caps --direct "$T/libvulkan_mali.so"
EOS

say() { printf '%s\n' "$*"; }
say "verify: $SERIAL against $(basename "$MAN")"
adb -s "$SERIAL" get-state >/dev/null 2>&1 || die "adb does not see $SERIAL"
adb -s "$SERIAL" shell mkdir -p "$D" >/dev/null
for p in "$PROBES/t_vk_release" "$PROBES/t_dxvk_caps" "$W/guest.sh"; do
   adb -s "$SERIAL" push "$p" "$D/" >/dev/null || die "adb push $p failed"
done

EXPECTED=$(jq -r '.library_sha256' "$MAN")
LOG=$W/guest.out
if ! DROIDDECK_PACKAGE=${DROIDDECK_PACKAGE:-com.ludashi.benchmark} \
     "$CTL" --serial "$SERIAL" guest --timeout 300 -- sh "$D/guest.sh" "$EXPECTED" >"$LOG" 2>"$W/guest.err"; then
   say "guest command failed (no active session? try: $CTL --serial $SERIAL start steam --wait)"
   tail -5 "$W/guest.err" | sed 's/^/  /'
   exit 2
fi
if grep -q 'NO_ACTIVE_SESSION' "$LOG"; then
   say "no active guest session (start one, then rerun)"
   exit 2
fi

python3 - "$MAN" "$LOG" <<'PY'
import hashlib, json, re, sys

man = json.load(open(sys.argv[1]))
recs = []
for line in open(sys.argv[2]):
    line = line.strip()
    if line.startswith("{"):
        try:
            recs.append(json.loads(line))
        except ValueError:
            pass

fails = 0
def check(ok, text):
    global fails
    print(("PASS " if ok else "FAIL ") + text)
    fails += 0 if ok else 1

def lines(probe):
    return [r.get("line", "") for r in recs if r.get("probe") == probe]

lib = next((r.get("lib") for r in recs if r.get("lib")), "")
sha = next((r.get("sha256") for r in recs if r.get("candidate") == lib), "")
check(bool(lib), "installed driver: %s" % (lib or "not found"))
check(sha == man["library_sha256"], "library sha256 %s (manifest %s)" % (sha or "-", man["library_sha256"][:12]))

drv = next((l for l in lines("t_vk_release") if l.startswith("driver=")), "")
name, _, info = drv[len("driver="):].partition("|")
check(info == man["driver_info"], "driverInfo %r" % info)
check(hashlib.sha256(info.encode()).hexdigest() == man["driver_info_sha256"],
      "driverInfo sha256 %s" % hashlib.sha256(info.encode()).hexdigest()[:12])

exts = {l.split("=", 1)[1] for l in lines("t_vk_release") if l.startswith("instance_ext=")}
for s in man["surfaces"]:
    check(s in exts, "advertised %s" % s)

caps = lines("t_dxvk_caps")
missing = {l[len("MISSING "):] for l in caps if l.startswith("MISSING ")}
present = {l[len("ok"):].strip() for l in caps if l.startswith("ok")}
allowed = [set(man["dxvk_missing"]["v11"]), set(man["dxvk_missing"]["v13"])]
check(missing in allowed, "feature list: %d present, %d missing (manifest v11/v13: %d/%d)" %
      (len(present), len(missing), len(man["dxvk_missing"]["v11"]), len(man["dxvk_missing"]["v13"])))
if missing not in allowed:
    check(False, "unexpected missing: %s" % ", ".join(sorted(missing)) or "-")

print("VERDICT %s (%d checks, %d failed)" % ("PASS" if fails == 0 else "FAIL", 5 + len(man["surfaces"]), fails))
sys.exit(0 if fails == 0 else 1)
PY
