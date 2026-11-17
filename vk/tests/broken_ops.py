# SPDX-License-Identifier: MIT
# Writes a copy of a ring dump with a deliberately broken stream: the first WAIT after STORE_STATE/FLUSH_CACHE2 is turned into a NOP (async hazard).
import json, struct, sys
src, dst = sys.argv[1:3]
out = open(dst, 'w')
for l in open(src):
    e = json.loads(l)
    b = bytearray(bytes.fromhex(e['hex'])); n = 0
    for i in range(0, len(b), 8):
        if b[i + 7] == 3: b[i:i + 8] = bytes(8); n += 1
    e['hex'] = b.hex(); out.write(json.dumps(e) + '\n')
