#!/usr/bin/python3
# SPDX-License-Identifier: MIT
"""fake2mem.py DUMP.jsonl OUT.mem : KB_FAKE_DUMP (src/backend_fake.c) -> kdecode .mem snapshot (kick 1 = first kick).
Prints 'ring VA size_bytes' per kick for the kdecode command line."""
import json, struct, sys
regs = {}; kick = 0; out = open(sys.argv[2], 'wb')
def flush(k):
    for va, b in regs.items(): out.write(struct.pack('<5Q', 0x4d454d53, k, va, len(b), 3) + bytes(b))
cur = None
for l in open(sys.argv[1]):
    j = json.loads(l)
    if j['ev'] == 'kick': kick = j['n']; regs = {}
    elif j['ev'] == 'region': cur = int(j['va'], 16); regs[cur] = bytearray(j['len'])
    elif j['ev'] == 'mem':
        va = int(j['va'], 16); b = bytes.fromhex(j['hex'])
        base = max(r for r in regs if r <= va); regs[base][va - base: va - base + len(b)] = b
    elif j['ev'] == 'ring':
        va = int(j['va'], 16); flush(kick)
        print('kick', kick, 'ring %x' % (va + j['start'] % j['size']), j['insert'] - j['start'])
