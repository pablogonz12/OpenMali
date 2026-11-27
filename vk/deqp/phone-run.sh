#!/usr/bin/env bash
# phone-run.sh — run a dEQP-VK case list on a phone's DroidDeck GUEST (glibc there), chunked, and summarise.
# Replaces the missing scripts/quick/qdeqp (M/notes/audit-coverage.md:65-67): the old vk/deqp/qdeqp.py imported a
# `quick` package that never existed in this repository, so this is a self-contained runner with the same shape.
#   usage: vk/deqp/phone-run.sh <phone> <caselist> [--chunk 500] [--case-timeout 60] [--deqp PATH] [--out DIR] [--dry-run]
# <phone> is the ddc alias (x4|cmf|x8). The caselist is one dEQP case name per line ('#' comments and blank lines skipped).
# Needs: a DEQP-VK BINARY (--deqp, else vk/deqp/build/deqp-vk, else $DEQP_VK) and the WSI ICD pair of
# M/notes/device-bundles.md (a64-glibc-wsi build run inside the guest); no Steam launch, `ddc <phone> guest` only.
set -euo pipefail
M=${M:-$HOME/.config/opencode/mali-orchestrator/bin}
V=$(cd "$(dirname "$0")/.." && pwd)          # vk/
phone=""; list=""; chunk=500; cto=60; deqp="${DEQP_VK:-$V/deqp/build/deqp-vk}"; out="${OUT:-$HOME/.cache/mali-orchestrator/deqp-phone}"; dry=0; covdb=0
while [ $# -gt 0 ]; do case "$1" in
  --chunk) chunk=$2; shift 2;; --case-timeout) cto=$2; shift 2;; --deqp) deqp=$2; shift 2;;
  --out) out=$2; shift 2;; --dry-run) dry=1; shift;; --covdb) covdb=1; shift;;
  -*) echo "unknown option $1" >&2; exit 2;;
  *) if [ -z "$phone" ]; then phone=$1; elif [ -z "$list" ]; then list=$1; else echo "extra arg $1" >&2; exit 2; fi; shift;;
esac; done
[ -n "$phone" ] && [ -n "$list" ] || { sed -n '2,10p' "$0"; exit 2; }
[ -f "$list" ] || { echo "no case list: $list" >&2; exit 2; }
[ -x "$deqp" ] || [ "$dry" = 1 ] || { echo "no deqp-vk at $deqp (pass --deqp)" >&2; exit 2; }
so=$V/out/a64-glibc-wsi/libvulkan_mali.so
tag=$(date +%Y%m%d-%H%M%S)-$(basename "$list" .txt)
run="$out/$tag"; mkdir -p "$run"
mapfile -t cases < <(grep -vE '^\s*(#|$)' "$list")
n=${#cases[@]}; total_chunks=$(( (n + chunk - 1) / chunk ))
echo "phone=$phone cases=$n chunks=$total_chunks chunk=$chunk case_timeout=${cto}s out=$run"
[ "$dry" = 1 ] && { echo "DRY RUN: would push $deqp and $so, then for each chunk run inside the guest:"; \
  echo "  ddc $phone guest -- sh -c 'cd /tmp/deqp && timeout <t> ./deqp-vk --deqp-caselist-file=chunk.cases ...'"; exit 0; }
dl=/sdcard/Download/deqp-phone
"$M/ddc" "$phone" adb shell "rm -rf /sdcard/Download/deqp-phone.bundle && mkdir -p /sdcard/Download/deqp-phone.bundle"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
cp "$deqp" "$tmp/deqp-vk"; [ -f "$so" ] && cp "$so" "$tmp/libvulkan_mali.so"
tar czf "$tmp/b.tgz" -C "$tmp" deqp-vk libvulkan_mali.so
"$M/ddc" "$phone" adb push "$tmp/b.tgz" "$dl.bundle/b.tgz"
pass=0; fail=0; lost=0
for ((i=0; i<n; i+=chunk)); do
  k=$(( i / chunk )); seg=( "${cases[@]:i:chunk}" ); printf '%s\n' "${seg[@]}" > "$tmp/chunk.cases"
  echo "== chunk $((k+1))/$total_chunks (${#seg[@]} cases)"
  "$M/ddc" "$phone" adb push "$tmp/chunk.cases" "$dl.bundle/chunk.cases" >/dev/null
  # guest: glibc deqp-vk, our ICD by path, one QPA per chunk; the newline-joined caselist is what --deqp-caselist-file takes
  "$M/ddc" "$phone" guest -- sh -c "set -e; rm -rf /tmp/deqp-phone && mkdir -p /tmp/deqp-phone && \
      cd /tmp/deqp-phone && tar xzf $dl.bundle/b.tgz && cp $dl.bundle/chunk.cases chunk.cases && \
      MALI_SHARED_CACHE= MALI_ICD=\$PWD/libvulkan_mali.so timeout $(( cto * ${#seg[@]} + 120 )) \
      ./deqp-vk --deqp-caselist-file=chunk.cases --deqp-log-filename=chunk.qpa --deqp-surface-type=fbo \
        --deqp-gl-config-name=rgba8888d24s8 --deqp-visibility=hidden --deqp-shadercache=disable; \
      echo qpa=\$(ls -l chunk.qpa | awk '{print \$5}')" > "$run/chunk-$k.log" 2>&1 || true
  tail -2 "$run/chunk-$k.log"
  "$M/ddc" "$phone" adb pull "/tmp/deqp-phone/chunk.qpa" "$run/chunk-$k.qpa" >/dev/null 2>&1 || \
    "$M/ddc" "$phone" guest -- sh -c "tar cf - -C /tmp/deqp-phone chunk.qpa" > "$run/chunk-$k.qpa.tar"
  if [ -s "$run/chunk-$k.qpa" ]; then
    p=$(grep -c 'StatusCode="Pass"' "$run/chunk-$k.qpa" || true); f=$(grep -c 'StatusCode="Fail"' "$run/chunk-$k.qpa" || true)
    l=$(grep -c 'StatusCode="DeviceLost"' "$run/chunk-$k.qpa" || true)
    pass=$((pass+p)); fail=$((fail+f)); lost=$((lost+l)); echo "   pass=$p fail=$f lost=$l"
    # one TSV row per case for covdb (vk/deqp/tools/covdb import: case<TAB>status)
    awk '/beginTestCaseResult/{sub(/^.*beginTestCaseResult /,""); c=$0} /StatusCode=/{s=$0; sub(/^.*StatusCode="/,"",s); sub(/".*$/,"",s); if(c!=""){print c"\t"s; c=""}}' \
        "$run/chunk-$k.qpa" >> "$run/results.tsv" 2>/dev/null || true
  else echo "   no .qpa pulled"; fi
done
sum="$run/summary.txt"; { echo "list=$list cases=$n pass=$pass fail=$fail lost=$lost chunk=$chunk out=$run"; } | tee "$sum"
echo "results: $run/chunk-*.qpa  summary: $sum"
if [ "$covdb" = 1 ] && [ -s "$run/results.tsv" ] && [ -x "$V/deqp/tools/covdb" ]; then
  "$V/deqp/tools/covdb" import --phone "$phone" --tool phone-run --notes "$(basename "$list")" "$run/results.tsv" \
    && echo "covdb: imported $run/results.tsv" || echo "covdb import failed (kept $run/results.tsv)" >&2
fi
[ "$fail" = 0 ] && [ "$lost" = 0 ]
