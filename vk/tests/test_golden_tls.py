# SPDX-License-Identifier: MIT
"""The packed TSD TLS pointer must be checked as an address, not a raw word."""
import importlib.util
import os
import struct

spec = importlib.util.spec_from_file_location('golden', os.path.join(os.path.dirname(__file__), 'golden.py'))
golden = importlib.util.module_from_spec(spec)
spec.loader.exec_module(golden)

base = golden.VA_LO + 0x10000
desc = golden.VA_LO + 0x20000
for state in ('live', 'freed', 'missing'):
    dev = golden.Dev(0)
    dev.mem.add(desc, struct.pack('<8I', 1, 0, (base >> 8) & 0xffffffff,
                                   (base >> 40) | (1 << 28), 0, 0, 0, 0))
    if state != 'missing':
        dev.reg[1] = dict(va=base, len=4096, id=1, kind=0, live=state == 'live')
    violations = []
    ctx = golden.Ctx(dev, 1, 11, [], violations)
    text = ' '.join(ctx.words(desc, 32, 'TSD'))
    assert ('BO1' in text) == (state != 'missing'), (state, text)
    assert bool(violations) == (state != 'live'), (state, text, violations)
print('packed TLS live, freed, missing: OK')
