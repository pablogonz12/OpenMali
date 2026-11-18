#!/bin/bash
# SPDX-License-Identifier: MIT
# PR #14 guard (host, no Mesa build): the meta copy shader must not use SPIR-V atomics. v13 type 2 memory is not COHERENT_LOCAL, where
# byte atomics (atomicAnd/atomicOr merge of sub-word stores) are lost; the byte path gives each destination word one plain-store writer instead.
# Parses vk/shader/meta_copy.comp.spv and the embedded copy in src/mali_meta_spv.h would drift, so both must agree (header is regenerated from the spv).
# Mutation check: restore the pre-PR spv (git show origin/main:vk/shader/meta_copy.comp.spv) and this fails.
set -euo pipefail
V=$(cd "$(dirname "$0")/.." && pwd)
python3 -I - "${1:-$V/shader/meta_copy.comp.spv}" "${2:-$V/src/mali_meta_spv.h}" <<'PY'
import re, struct, sys
ATOMIC = {227: 'AtomicLoad', 228: 'AtomicStore', 229: 'AtomicExchange', 230: 'AtomicCompareExchange', 231: 'AtomicCompareExchangeWeak',
          232: 'AtomicIIncrement', 233: 'AtomicIDecrement', 234: 'AtomicIAdd', 235: 'AtomicISub', 236: 'AtomicSMin', 237: 'AtomicUMin',
          238: 'AtomicSMax', 239: 'AtomicUMax', 240: 'AtomicAnd', 241: 'AtomicOr', 242: 'AtomicXor', 318: 'AtomicFAddEXT'}
def scan(name, w):
    assert w[0] == 0x07230203, name + ": not SPIR-V"
    i, n, found = 5, 0, []
    while i < len(w):
        cnt, op = w[i] >> 16, w[i] & 0xffff
        assert cnt > 0, "bad instruction"
        n += 1
        if op in ATOMIC: found.append(ATOMIC[op])
        i += cnt
    print("%s: %d instructions, atomics: %s" % (name, n, ",".join(found) or "none"))
    return w, bool(found)
b = open(sys.argv[1], 'rb').read()
f, bad1 = scan("meta_copy.comp.spv", struct.unpack('<%dI' % (len(b) // 4), b))
h = open(sys.argv[2]).read()
m = re.search(r'meta_copy_spv\[\]\s*=\s*\{(.*?)\};', h, re.S)
hw = tuple(int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', m.group(1)))
_, bad2 = scan("mali_meta_spv.h meta_copy_spv (what the driver embeds)", hw)
same = hw == f
print("header matches .spv: %s" % same)
sys.exit(1 if (bad1 or bad2 or not same) else 0)
PY
