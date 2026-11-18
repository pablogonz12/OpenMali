#!/bin/sh
# guest/nokopper.c drops LIBGL_KOPPER_DISABLE from the process environment (only when BL_OPEN_DRIVER is set),
# keeps every other variable, and is a no-op otherwise. Runs real processes under LD_PRELOAD (host x86 build of the
# same source; the aarch64 build is `clang --target=aarch64-linux-gnu -fuse-ld=lld -shared -nostdlib`).
# Includes a mutation check: a build with the filter removed must make this test fail.
set -u
V=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/nokopper.XXXXXX") || { echo "FAIL mktemp"; exit 1; }
trap 'rm -rf "$T"' EXIT
# host compiler: same selection as the other host tests (CC, else cc, else clang)
CC=${CC:-$(command -v cc || command -v clang || true)}
[ -n "$CC" ] || { echo "FAIL no host C compiler (set CC)"; exit 1; }
$CC -O2 -fPIC -shared -nostdlib -o "$T/good.so" "$V/guest/nokopper.c" || { echo "FAIL build"; exit 1; }
sed 's/if (!eq(\*in, name, sizeof name - 1)) \*out++ = \*in;/*out++ = *in;/' "$V/guest/nokopper.c" > "$T/mut.c"
cmp -s "$T/mut.c" "$V/guest/nokopper.c" && { echo "FAIL mutation did not change the source"; exit 1; }
$CC -O2 -fPIC -shared -nostdlib -o "$T/mut.so" "$T/mut.c" || { echo "FAIL build mutant"; exit 1; }

probe() { # lib, vars...: runs env under the preload; a probe that cannot run is a distinct failure
  lib=$1; shift
  pout=$(env -i PATH=/usr/bin:/bin "$@" LD_PRELOAD="$lib" /usr/bin/env 2>&1) && echo "$pout" | grep -q '^PATH=' && { printf '%s\n' "$pout"; return 0; }
  echo "PROBE-FAILED: $* -> $pout" >&2; return 1
}
check() { # lib; prints failures, returns count (probe failures are reported as "probe failed")
  lib=$1; bad=0
  out=$(probe "$lib" BL_OPEN_DRIVER=1 LIBGL_KOPPER_DISABLE=true KEEP_A=1 KEEP_B=two) || { echo "  probe failed"; return 99; }
  echo "$out" | grep -q '^LIBGL_KOPPER_DISABLE=' && { echo "  gated on: variable still set"; bad=$((bad+1)); }
  echo "$out" | grep -q '^KEEP_A=1$' && echo "$out" | grep -q '^KEEP_B=two$' && echo "$out" | grep -q '^BL_OPEN_DRIVER=1$' || { echo "  gated on: other variables lost"; bad=$((bad+1)); }
  out=$(probe "$lib" LIBGL_KOPPER_DISABLE=true) || { echo "  probe failed"; return 99; }
  echo "$out" | grep -q '^LIBGL_KOPPER_DISABLE=true$' || { echo "  gated off: variable was removed"; bad=$((bad+1)); }
  out=$(probe "$lib" BL_OPEN_DRIVER=1) || { echo "  probe failed"; return 99; }
  echo "$out" | grep -q '^BL_OPEN_DRIVER=1$' || { echo "  no variable: environment damaged"; bad=$((bad+1)); }
  out=$(probe "$lib" BL_OPEN_DRIVER=1 LIBGL_KOPPER_DISABLE_X=1 LIBGL_KOPPER_DISABLEX=true) || { echo "  probe failed"; return 99; }
  echo "$out" | grep -q '^LIBGL_KOPPER_DISABLE_X=1$' && echo "$out" | grep -q '^LIBGL_KOPPER_DISABLEX=true$' || { echo "  prefix: similarly named variables removed"; bad=$((bad+1)); }
  return $bad
}
fail=0
gout=$(check "$T/good.so" 2>&1) && echo "ok    nokopper: variable dropped when gated, everything else kept" || { fail=1; echo "FAIL  nokopper"; echo "$gout" | sed 's/^/        /'; }
# the mutant (filter removed) must fail for the right reason: every probe ran, and the gated-on probe kept the variable
mout=$(check "$T/mut.so" 2>&1); mrc=$?
if [ $mrc -eq 0 ]; then echo "FAIL  nokopper mutation (filter removed) was not detected"; fail=1
elif echo "$mout" | grep -q "probe failed"; then echo "FAIL  nokopper mutation check: a probe could not run"; echo "$mout" | sed 's/^/        /'; fail=1
elif echo "$mout" | grep -q "gated on: variable still set"; then echo "ok    nokopper mutation detected (variable still set)"
else echo "FAIL  nokopper mutation failed for another reason"; echo "$mout" | sed 's/^/        /'; fail=1; fi
exit $fail
