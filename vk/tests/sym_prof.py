#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Resolves t_skia_draws --prof samples (PC + callers, with /proc/self/maps) against each module's symbol table (nm), like
# `perf report --sort symbol`: self = samples whose PC is in the symbol, incl = samples with the symbol anywhere on the stack.
#   sym_prof.py samples.txt [top] [callee ...]   (callee: also print who calls it, e.g. memcpy getenv)
import bisect, subprocess, sys, collections

def main():
    path = sys.argv[1]; top = int(sys.argv[2]) if len(sys.argv) > 2 else 20; callees = sys.argv[3:]
    maps, samples = [], []
    for line in open(path):
        if line.startswith('map '):
            f = line.split()
            if len(f) >= 7 and 'x' in f[2] and f[6].startswith('/'):
                lo, hi = (int(x, 16) for x in f[1].split('-'))
                maps.append((lo, hi, int(f[3], 16), f[6]))
        elif line.startswith('s '):
            samples.append([int(x, 16) for x in line.split()[1:]])
    tabs = {}
    def table(mod):
        if mod not in tabs:
            out = subprocess.run(['nm', '-n', '--defined-only', '-C', mod], capture_output=True, text=True).stdout
            if not out.strip():
                out = subprocess.run(['nm', '-n', '-D', '--defined-only', '-C', mod], capture_output=True, text=True).stdout
            addrs, names = [], []
            for l in out.splitlines():
                p = l.split(None, 2)
                if len(p) == 3 and p[1] in 'tTwWiI':
                    addrs.append(int(p[0], 16)); names.append(p[2])
            # file offset -> vaddr: first PT_LOAD of a PIE/so maps file offset == vaddr for the text segment in practice
            tabs[mod] = (addrs, names)
        return tabs[mod]
    def sym(pc):
        for lo, hi, off, mod in maps:
            if lo <= pc < hi:
                a, n = table(mod)
                rel = pc - lo + off
                i = bisect.bisect_right(a, rel) - 1
                short = mod.rsplit('/', 1)[-1]
                return (n[i] if i >= 0 else '?') + ' [' + short + ']'
        return '[unknown]'
    self_c, incl_c = collections.Counter(), collections.Counter()
    callers = {c: collections.Counter() for c in callees}
    for s in samples:
        names = [sym(pc) for pc in s]
        self_c[names[0]] += 1
        for c in callees:
            if names[0].startswith(c):
                callers[c][names[1] if len(names) > 1 else '?'] += 1
        for n in set(names):
            incl_c[n] += 1
    tot = len(samples) or 1
    print(f'samples: {len(samples)}')
    print('| # | self % | incl % | symbol |')
    print('|---|---|---|---|')
    for k, (n, c) in enumerate(self_c.most_common(top), 1):
        print(f'| {k} | {100*c/tot:.1f} | {100*incl_c[n]/tot:.1f} | `{n}` |')
    print()
    print('Inclusive (top, driver only):')
    print('| incl % | symbol |')
    print('|---|---|')
    for n, c in [x for x in incl_c.most_common() if 'libvulkan_mali' in x[0]][:top]:
        print(f'| {100*c/tot:.1f} | `{n}` |')

    for c in callees:
        print(f'\nCallers of {c} (self samples):')
        for n, k in callers[c].most_common(8):
            print(f'  {100*k/tot:.1f} % {n}')

main()
