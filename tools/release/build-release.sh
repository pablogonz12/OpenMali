#!/bin/bash
# SPDX-License-Identifier: MIT
# Repeatable release build: clean worktree of a commit -> a64-glibc-wsi zip -> host gate -> packaging checks -> manifest.
#
#   tools/release/build-release.sh [--ref REF] [--out DIR] [--worktree DIR] [--skip-gate] [--keep]
#
# Defaults: --ref origin/main, --out tools/release/out, worktree under /tmp/oc (removed on exit).
# Env:
#   MESA_SRC                Mesa source (default <main checkout>/third_party/mesa)
#   MESA_BUILD              host gate Mesa build tree, passed to vk/check.sh (default: vk/build.sh picks it)
#   MALI_A64_MESA_BUILD     a64-glibc-wsi Mesa build tree (default <main checkout>/third_party/build-vk-a64-glibc-wsi)
#   MALI_SLOTS              passed to mali-slot when present (default 3)
#
# Output in --out:
#   mali-open-a64-glibc-wsi-<YYYYmmdd>-g<commit>.zip   deterministic name (date + commit)
#   manifest.json                                      commit, date, flags, checks, t_dxvk_caps missing list
#   probe/{t_vk_release,t_dxvk_caps}                   aarch64 probes for verify-guest.sh
#   check.log, build.log, dxvk-{v11,v13}.log
#
# The build worktree is clean: it contains only the commit, plus an ignored third_party symlink to the
# main checkout (Mesa build trees stay out of the repository). Nothing is committed by this script.
set -euo pipefail

SELF=$(cd "$(dirname "$0")" && pwd)
REPO=$(git -C "$SELF" rev-parse --show-toplevel)
REF=origin/main
OUT=""
WT=""
SKIP_GATE=0
KEEP=0
NO_FETCH=0

die() { echo "build-release: $*" >&2; exit 1; }
say() { printf '%s\n' "$*"; }

while [ $# -gt 0 ]; do
   case $1 in
      --ref) REF=${2:?--ref needs a value}; shift 2 ;;
      --out) OUT=${2:?--out needs a value}; shift 2 ;;
      --worktree) WT=${2:?--worktree needs a value}; shift 2 ;;
      --mesa-a64) MALI_A64_MESA_BUILD=${2:?--mesa-a64 needs a value}; shift 2 ;;
      --skip-gate) SKIP_GATE=1; shift ;;
      --no-fetch) NO_FETCH=1; shift ;;
      --keep) KEEP=1; shift ;;
      -h|--help) sed -n '2,25p' "$SELF/build-release.sh" | sed 's/^# \{0,1\}//'; exit 0 ;;
      *) die "unknown argument: $1 (see --help)" ;;
   esac
done

# third_party lives in the main checkout: a linked worktree has no copy and must not grow one.
COMMON=$(git -C "$REPO" rev-parse --path-format=absolute --git-common-dir)
MAIN=$(dirname "$COMMON")
TP=$MAIN/third_party
MESA_SRC=${MESA_SRC:-$TP/mesa}
MESA_A64=${MALI_A64_MESA_BUILD:-$TP/build-vk-a64-glibc-wsi}
export MESA_SRC
[ -n "${MESA_BUILD:-}" ] && export MESA_BUILD

[ -f "$MESA_SRC/include/vulkan/vulkan.h" ] || die "no Mesa source at $MESA_SRC (set MESA_SRC)"
[ -f "$MESA_A64/build.ninja" ] || die "no a64-glibc-wsi Mesa build at $MESA_A64 (run vk/cross/build-cross.sh glibc-wsi in $MAIN)"

if [ "$NO_FETCH" = 0 ]; then
   git -C "$REPO" fetch --quiet origin '+refs/heads/main:refs/remotes/origin/main' || die "git fetch origin main failed"
fi
COMMIT=$(git -C "$REPO" rev-parse "$REF^{commit}") || die "cannot resolve ref $REF"
SHORT=$(git -C "$REPO" rev-parse --short=12 "$COMMIT")
DATE=$(date +%Y%m%d)
BUILT_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
ZIPNAME=mali-open-a64-glibc-wsi-$DATE-g$SHORT.zip
OUT=${OUT:-$REPO/tools/release/out/$DATE-g$SHORT}
OUT=$(realpath -m "$OUT")
mkdir -p "$OUT"

if [ -z "$WT" ]; then
   mkdir -p "${TMPDIR:-/tmp}/oc"
   WT=$(mktemp -d "${TMPDIR:-/tmp}/oc/mali-release-XXXXXX")
else
   WT=$(realpath -m "$WT")
   [ -e "$WT" ] && die "worktree path already exists: $WT"
   mkdir -p "$(dirname "$WT")"
fi
CREATED=0
V=""
cleanup() {
   rc=$1
   [ -n "$V" ] && rm -rf "$V"
   if [ "$KEEP" = 0 ] && [ "$CREATED" = 1 ]; then
      git -C "$REPO" worktree remove --force "$WT" >/dev/null 2>&1 || rm -rf "$WT"
   fi
   exit "$rc"
}
trap 'cleanup "$?"' EXIT

say "release: $REF -> $COMMIT ($SHORT), date $DATE"
git -C "$REPO" worktree add --detach "$WT" "$COMMIT" >/dev/null
CREATED=1
git -C "$REPO" worktree list --porcelain >"$OUT/worktrees.txt"
grep -qx "worktree $WT" "$OUT/worktrees.txt" || die "worktree was not created"
# Ignored in git: the Mesa trees stay in the main checkout, the release worktree stays clean.
ln -sfn "$TP" "$WT/third_party"

# 1. a64-glibc-wsi driver (Wayland + X11 platforms; libxcb NEEDED).
say "build: a64-glibc-wsi with $MESA_A64"
( cd "$WT" && MESA_SRC="$MESA_SRC" MESA_BUILD="$MESA_A64" MALI_TARGET=a64-glibc-wsi ./vk/build.sh ) \
   2>&1 | tee "$OUT/build.log" | tail -3
[ -f "$WT/vk/out/a64-glibc-wsi/libvulkan_mali.so" ] || die "a64-glibc-wsi build produced no libvulkan_mali.so"

# 2. Full host gate (its own native build; MESA_BUILD only selects the tree).
if [ "$SKIP_GATE" = 1 ]; then
   say "gate: skipped (--skip-gate)"
   GATE=skip
else
   say "gate: vk/check.sh"
   if ( cd "$WT" && ./vk/check.sh ) >"$OUT/check.log" 2>&1; then
      GATE=pass
      tail -2 "$OUT/check.log" | sed 's/^/  /'
   else
      tail -25 "$OUT/check.log" | sed 's/^/  /'
      die "host gate failed (log: $OUT/check.log)"
   fi
fi

NATIVE=$WT/vk/out/native
[ -x "$NATIVE/t_dxvk_caps" ] && [ -f "$NATIVE/libvulkan_mali.so" ] || \
   die "no native test build at $NATIVE/t_dxvk_caps (needed for the manifest; run without --skip-gate)"
[ -x "$NATIVE/t_vk_info" ] || die "no native t_vk_info at $NATIVE/t_vk_info"

# 3. Package the a64-glibc-wsi variant, then give the zip its deterministic name.
say "package: a64-glibc-wsi"
mkdir -p "$OUT/zip"
( cd "$WT" && ./vk/package/build-zip.sh -o "$OUT/zip" a64-glibc-wsi ) | tail -1
ZIPSRC=$OUT/zip/mali-open-a64-glibc-wsi-$DATE.zip
[ -f "$ZIPSRC" ] || die "build-zip.sh did not produce $ZIPSRC"
mv "$ZIPSRC" "$OUT/$ZIPNAME"
ZIP=$OUT/$ZIPNAME

# 4. Packaging verification, on the zip as a phone would receive it.
say "verify: package"
V=$(mktemp -d "${TMPDIR:-/tmp}/oc/mali-verify-XXXXXX")
unzip -q "$ZIP" -d "$V"
( cd "$V" && sha256sum -c SHA256SUMS >/dev/null ) || die "SHA256SUMS does not verify"
for f in meta.json libvulkan_panfrost.so icd.json LICENSE NOTICE-MESA.txt VERSION NEEDED.txt README.txt SHA256SUMS; do
   [ -f "$V/$f" ] || die "zip is missing $f"
done
file -b "$V/libvulkan_panfrost.so" | grep -q '^ELF 64-bit LSB.*ARM aarch64' || die "driver is not an aarch64 ELF"
jq -e '.kind == "linux-vulkan-icd" and .libraryName == "libvulkan_panfrost.so"' "$V/meta.json" >/dev/null || \
   die "meta.json is not a linux-vulkan-icd package"
NEEDED=$(readelf -d "$V/libvulkan_panfrost.so" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')
grep -qx 'libxcb.so.1' <<<"$NEEDED" || die "libxcb.so.1 missing from NEEDED (wrong Mesa platforms: want wayland,x11)"
SURFACES=(VK_KHR_wayland_surface VK_KHR_xcb_surface VK_KHR_xlib_surface VK_EXT_headless_surface)
for s in "${SURFACES[@]}"; do
   grep -aq "$s" "$V/libvulkan_panfrost.so" || die "surface extension $s not in the packaged driver"
done
EXPORTS=$(nm -D --defined-only "$V/libvulkan_panfrost.so" | awk '{print $3}' | grep -v '^$' | sort | tr '\n' ' ')
WANT="vk_icdGetInstanceProcAddr vk_icdGetPhysicalDeviceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion "
[ "$EXPORTS" = "$WANT" ] || die "exports are not the three loader entry points: $EXPORTS"
if readelf -d "$V/libvulkan_panfrost.so" | grep NEEDED | grep -qi libelf; then die "libelf in NEEDED"; fi
SO_SHA=$(sha256sum "$V/libvulkan_panfrost.so" | cut -d' ' -f1)
ZIP_SHA=$(sha256sum "$ZIP" | cut -d' ' -f1)
say "  aarch64 ELF, 3 exports, four surface names, libxcb NEEDED, checksums OK"
rm -rf "$V"; V=""

# 5. Feature evidence from the native build (same front end, fake v11/v13).
DRIVER_INFO=$(MALI_BACKEND=fake MALI_FAKE_GPU=v11 "$NATIVE/t_vk_info" --direct "$NATIVE/libvulkan_mali.so" --dump 2>/dev/null \
              | sed -n 's/.*driver=mali-own (\(.*\)) subgroupSize=.*/\1/p' | head -1)
[ -n "$DRIVER_INFO" ] || die "could not read driverInfo from $NATIVE/t_vk_info"
DI_SHA=$(printf '%s' "$DRIVER_INFO" | sha256sum | cut -d' ' -f1)

for g in v11 v13; do
   MALI_BACKEND=fake MALI_FAKE_GPU=$g "$NATIVE/t_dxvk_caps" --direct "$NATIVE/libvulkan_mali.so" \
      >"$OUT/dxvk-$g.log" 2>&1 || die "t_dxvk_caps $g failed (log: $OUT/dxvk-$g.log)"
done
missing() { sed -n 's/^MISSING //p' "$OUT/dxvk-$1.log" | jq -R -s 'split("\n") | map(select(length > 0))'; }
present() { sed -n 's/^ok  *//p' "$OUT/dxvk-$1.log" | jq -R -s 'split("\n") | map(select(length > 0))'; }
M11=$(missing v11); M13=$(missing v13)
P11=$(present v11); P13=$(present v13)
UNION=$(jq -n --argjson a "$M11" --argjson b "$M13" '$a + $b | unique')
NEEDED_JSON=$(printf '%s\n' "$NEEDED" | jq -R -s 'split("\n") | map(select(length > 0))')
SURFACES_JSON=$(printf '%s\n' "${SURFACES[@]}" | jq -R -s 'split("\n") | map(select(length > 0))')

# 6. aarch64 probes for verify-guest.sh (t_vk_release + the DXVK feature test).
say "probes: build t_vk_release for aarch64"
mkdir -p "$OUT/probe"
( cd "$MESA_A64"
  CMDS=$(ninja -t commands src/panfrost/vulkan/libvulkan_panfrost.so)
  CCLINE=$(grep -m1 -- 'panvk_instance\.c$' <<<"$CMDS")
  [ -n "$CCLINE" ] || { echo "cannot find the reference compile line in $MESA_A64" >&2; exit 1; }
  CC=$(sed -E 's# -MD -MQ [^ ]+ -MF [^ ]+##; s# -o [^ ]+ -c [^ ]+$##; s# -I[^ ]*src/panfrost/vulkan[^ ]*##g' <<<"$CCLINE")
  eval "$CC -std=gnu11 -O2 -Wall -Wextra -Werror -I$MESA_SRC/include -o $OUT/probe/t_vk_release $SELF/t_vk_release.c -ldl"
)
cp "$WT/vk/out/a64-glibc-wsi/t_dxvk_caps" "$OUT/probe/t_dxvk_caps"
file -b "$OUT/probe/t_vk_release" | grep -q 'ARM aarch64' || die "t_vk_release was not built for aarch64"

# 7. Manifest.
mesa_ver=$(cat "$MESA_SRC/VERSION" 2>/dev/null || echo unknown)
jq -n \
   --arg commit "$COMMIT" --arg short "$SHORT" --arg ref "$REF" \
   --arg date "$DATE" --arg built_at "$BUILT_AT" --arg variant a64-glibc-wsi \
   --arg mesa "$mesa_ver" --arg mesa_build "$MESA_A64" \
   --arg zip "$ZIPNAME" --arg zip_sha "$ZIP_SHA" \
   --arg library "libvulkan_panfrost.so" --arg so_sha "$SO_SHA" \
   --arg driver_info "$DRIVER_INFO" --arg di_sha "$DI_SHA" \
   --arg gate "$GATE" \
   --arg msaa "${MALI_EXPERIMENTAL_MSAA:-unset}" --arg wsi "${MALI_WSI:-unset}" \
   --argjson surfaces "$SURFACES_JSON" --argjson needed "$NEEDED_JSON" \
   --argjson m11 "$M11" --argjson m13 "$M13" \
   --argjson p11 "$P11" --argjson p13 "$P13" --argjson union "$UNION" \
   '{
      schema: 1,
      commit: $commit, commit_short: $short, ref: $ref,
      date: $date, built_at: $built_at, variant: $variant,
      mesa: $mesa, mesa_build: $mesa_build,
      zip: $zip, zip_sha256: $zip_sha, library: $library, library_sha256: $so_sha,
      driver_info: $driver_info, driver_info_sha256: $di_sha,
      surfaces: $surfaces,
      feature_flags: {
         platforms: "wayland,x11",
         MALI_EXPERIMENTAL_MSAA: $msaa,
         MALI_WSI: $wsi
      },
      checks: {
         "vk/check.sh": $gate,
         "package:sha256": "pass",
         "package:aarch64": "pass",
         "package:exports": "pass",
         "package:surfaces": "pass",
         "package:libxcb-needed": "pass",
         "dxvk_caps:v11": "pass",
         "dxvk_caps:v13": "pass"
      },
      needed: $needed,
      dxvk_missing: {v11: $m11, v13: $m13},
      dxvk_missing_union: $union,
      dxvk_present: {v11: $p11, v13: $p13},
      probes: ["probe/t_vk_release", "probe/t_dxvk_caps"]
   }' >"$OUT/manifest.json"

say ""
say "zip:      $ZIP"
say "sha256:   $ZIP_SHA  $ZIPNAME"
say "manifest: $OUT/manifest.json"
say "missing (v11/v13): $(jq -r '(.dxvk_missing.v11|length) as $a | (.dxvk_missing.v13|length) as $b | "\($a)/\($b)"' "$OUT/manifest.json") DXVK items"
jq -r '.dxvk_missing_union[]' "$OUT/manifest.json" | sed 's/^/  - /'
say "verify on a phone: tools/release/verify-guest.sh <serial> --manifest $OUT/manifest.json"
