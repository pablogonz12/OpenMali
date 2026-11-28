#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host-only. Builds into tools/cefprof/out/:
#   libcefprof.so + layer/cefprof.json    the counting Vulkan layer
#   tree/vk/out/native/mali_icd.json      a THROWAWAY copy of the driver where the real kbase backend runs on libkbase's
#                                         fake kernel (libkbase/src/backend_fake.c) with every ioctl counted (kbstats.c).
# The two test-only edits are made in the copy, never in the checked-out sources: the fake GPU completes a counter when
# the backend waits on it, and the fake kernel's region table is enlarged for a Chromium-sized process.
# The plain fake-backend driver is the normal vk/out/native build (vk/build.sh).
# Env: MESA_SRC, MESA_BUILD (default: the repo's third_party, X11-WSI tree).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=$here/out
export MESA_SRC=${MESA_SRC:-$root/third_party/mesa}
export MESA_BUILD=${MESA_BUILD:-$root/third_party/build-vk-x86-x11}
[ -f "$MESA_BUILD/src/vulkan/util/vk_dispatch_table.h" ] || { echo "MESA_BUILD=$MESA_BUILD is not a complete X11 Mesa build tree" >&2; exit 2; }
mkdir -p "$out/gen" "$out/layer" "$out/tree"

python3 "$here/gen_layer.py" "$MESA_SRC/src/vulkan/registry/vk.xml" "$here/cefprof_layer.c" "$out/gen"
cc -std=gnu11 -O2 -g -fPIC -shared -fvisibility=hidden -Wall -Wno-unused-parameter -Wno-unused-function -I"$out/gen" -I"$MESA_SRC/include" \
   -o "$out/libcefprof.so" "$here/cefprof_layer.c" -lpthread
cat > "$out/layer/cefprof.json" <<EOF
{"file_format_version":"1.2.0","layer":{"name":"VK_LAYER_MALI_cefprof","type":"GLOBAL","library_path":"$out/libcefprof.so",
"api_version":"1.3.0","implementation_version":"1","description":"per-frame entry point counter"}}
EOF

[ -f "$root/vk/out/native/mali_icd.json" ] || (cd "$root/vk" && ./build.sh)

for d in vk libkbase; do
   mkdir -p "$out/tree/$d"
   (cd "$root/$d" && tar --exclude='./out' --exclude='./out-*' -cf - .) | tar -xf - -C "$out/tree/$d"
done
ln -sfn "$(cd "$root/third_party" && pwd -P)" "$out/tree/third_party"
mkdir -p "$out/tree/tools" && ln -sfn "$root/tools/shaderdb" "$out/tree/tools/shaderdb"
sed -i 's|^      if (\*c >= target) {$|      *(volatile uint64_t *)c = target; /* cefprof: the fake GPU finishes when waited on */\n&|' "$out/tree/vk/src/mali_backend_kbase.c"
grep -q 'cefprof: the fake GPU' "$out/tree/vk/src/mali_backend_kbase.c" || { echo "wait_counter edit did not apply" >&2; exit 2; }
sed -i 's|static struct region reg\[1024\];|static struct region reg[1 << 17];|; s|if (nreg < 1024) reg\[nreg++\]|if (nreg < (1 << 17)) reg[nreg++]|' "$out/tree/libkbase/src/backend_fake.c"
grep -q 'reg\[1 << 17\]' "$out/tree/libkbase/src/backend_fake.c" || { echo "region table edit did not apply" >&2; exit 2; }
cc -std=gnu11 -D_GNU_SOURCE -O2 -g -fPIC -w -I"$out/tree/libkbase/include" -I"$out/tree/libkbase/src" -c "$out/tree/libkbase/src/backend_fake.c" -o "$out/backend_fake.o"
cc -std=gnu11 -O2 -g -fPIC -Wall -c "$here/kbstats.c" -o "$out/kbstats.o"
(cd "$out/tree/vk" && MALI_EXTRA_LD="$out/backend_fake.o $out/kbstats.o -Wl,--wrap=ioctl -Wl,--defsym=open64=open -Wl,--defsym=mmap64=mmap" ./build.sh > "$out/kbfake-build.log" 2>&1) ||
   { tail -20 "$out/kbfake-build.log"; exit 1; }
echo "layer:  $out/libcefprof.so"
echo "fake:   $root/vk/out/native/mali_icd.json"
echo "kbfake: $out/tree/vk/out/native/mali_icd.json"
