#!/bin/sh
# guest/nokopper.c drops LIBGL_KOPPER_DISABLE from the process environment (only when BL_OPEN_DRIVER is set),
# keeps every other variable, and is a no-op otherwise. Runs real processes under LD_PRELOAD (host x86 build of the
# same source; the aarch64 build is `clang --target=aarch64-linux-gnu -fuse-ld=lld -shared -nostdlib`).
# Includes a mutation check: a build with the filter removed must make this test fail.
set -u
V=$(cd "$(dirname "$0")/.." && pwd); T=${TMPDIR:-/tmp}/nokopper.$$; mkdir -p "$T"; trap 'rm -rf "$T"' EXIT
CC=${CC:-clang}
$CC -O2 -fPIC -shared -nostdlib -o "$T/good.so" "$V/guest/nokopper.c" || { echo "FAIL build"; exit 1; }
sed 's/if (!eq(\*in, name, sizeof name - 1)) \*out++ = \*in;/*out++ = *in;/' "$V/guest/nokopper.c" > "$T/mut.c"
cmp -s "$T/mut.c" "$V/guest/nokopper.c" && { echo "FAIL mutation did not change the source"; exit 1; }
$CC -O2 -fPIC -shared -nostdlib -o "$T/mut.so" "$T/mut.c" || { echo "FAIL build mutant"; exit 1; }

check() { # lib; prints failures, returns count
  lib=$1; bad=0
  out=$(env -i PATH=/usr/bin:/bin BL_OPEN_DRIVER=1 LIBGL_KOPPER_DISABLE=true KEEP_A=1 KEEP_B=two LD_PRELOAD="$lib" /usr/bin/env)
  echo "$out" | grep -q '^LIBGL_KOPPER_DISABLE=' && { echo "  gated on: variable still set"; bad=$((bad+1)); }
  echo "$out" | grep -q '^KEEP_A=1$' && echo "$out" | grep -q '^KEEP_B=two$' && echo "$out" | grep -q '^BL_OPEN_DRIVER=1$' || { echo "  gated on: other variables lost"; bad=$((bad+1)); }
  out=$(env -i PATH=/usr/bin:/bin LIBGL_KOPPER_DISABLE=true LD_PRELOAD="$lib" /usr/bin/env)
  echo "$out" | grep -q '^LIBGL_KOPPER_DISABLE=true$' || { echo "  gated off: variable was removed"; bad=$((bad+1)); }
  out=$(env -i PATH=/usr/bin:/bin BL_OPEN_DRIVER=1 LD_PRELOAD="$lib" /usr/bin/env)
  echo "$out" | grep -q '^BL_OPEN_DRIVER=1$' || { echo "  no variable: environment damaged"; bad=$((bad+1)); }
  out=$(env -i PATH=/usr/bin:/bin BL_OPEN_DRIVER=1 LIBGL_KOPPER_DISABLE_X=1 LIBGL_KOPPER_DISABLEX=true LD_PRELOAD="$lib" /usr/bin/env)
  echo "$out" | grep -q '^LIBGL_KOPPER_DISABLE_X=1$' && echo "$out" | grep -q '^LIBGL_KOPPER_DISABLEX=true$' || { echo "  prefix: similarly named variables removed"; bad=$((bad+1)); }
  return $bad
}
fail=0
if check "$T/good.so"; then echo "ok    nokopper: variable dropped when gated, everything else kept"; else fail=1; echo "FAIL  nokopper"; fi
if check "$T/mut.so" >/dev/null; then echo "FAIL  nokopper mutation (filter removed) was not detected"; fail=1; else echo "ok    nokopper mutation detected"; fi
exit $fail
