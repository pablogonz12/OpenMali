#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""VK_KHR_index_type_uint8 on the host: every IDVS draw of `t_vk_feat --only idx8` must carry PRIMITIVE_FLAGS index type UINT8.

The fake backend has no shader interpreter, so the pixels of the case are checked on hardware only. What the host can see is the
draw's tiler/primitive flags register (KB_IDVS_TILER_FLAGS / KB13_IDVS_TILER_FLAGS = r56), decoded by tests/golden.py:
bits 10:8 are "Index type" (genxml v10/v12/v13 enum "Index Type": 0 none, 1 UINT8, 2 UINT16, 3 UINT32). The case issues two direct
indexed draws and one indexed indirect draw, so at least three RUN_IDVS must appear and every one must say UINT8.
usage: idx8-flags.py <rendered golden text>
"""
import re
import sys


def idvs_flags(text):
    """r56 of every RUN_IDVS/RUN_IDVS2, in order (the regs line that follows the RUN_IDVS regs block)."""
    out, pending = [], False
    for line in text.splitlines():
        if re.search(r'RUN_IDVS2? regs:', line):   # v10/v11 RUN_IDVS, v13 RUN_IDVS2
            pending = True
        elif pending and line.strip().startswith('regs:'):
            m = re.search(r'\br56=([0-9a-f]+)\b', line)
            out.append(int(m.group(1), 16) if m else None)
            pending = False
    return out


def main():
    flags = idvs_flags(open(sys.argv[1]).read())
    if len(flags) < 3:
        print(f'expected >= 3 RUN_IDVS draws, found {len(flags)}')
        return 1
    bad = [f for f in flags if f is None or (f >> 8) & 7 != 1]
    if bad:
        print('index type != UINT8 in r56: ' + ' '.join('none' if f is None else hex(f) for f in flags))
        return 1
    print(f'{len(flags)} draws, index type UINT8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
