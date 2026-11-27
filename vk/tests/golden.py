# SPDX-License-Identifier: MIT
"""Golden command-stream dumps + hardware-bug rules for the fake backend.   golden.py DUMP.jsonl OUT.txt

DUMP.jsonl is written by the fake backend (vk/src/mali_backend_fake.c, MALI_FAKE_DUMP=path): per submit_cs a "kick" with the live BO
list, the memory pages that changed and the command stream. This script
  1. renders every kick as address-normalised text (GPU VAs -> BO<id>+off, no host pointers, no timestamps) into OUT.txt, and
  2. checks the rules below on every job (RUN_*), printing one RULE line per violation (exit 1).

Rules (each one is a bug that was only found on the phone):
  R-VA    every GPU address a job can reach (RUN_* register pairs, descriptors reachable from them, SRT entries, sync addresses)
          lies inside a live BO: no dangling pointer, no pointer past the end of its BO, none into a freed BO.
  R-GEOM  tiler context geometry buffer is exactly 64 KiB on both arches (v11 renders nothing with 4 MiB, v13 coverage 0 with 4 MiB;
          67e1aa0/11e6249), lies fully inside its BO and the tiler heap descriptor is the page just before it.
  R-HEAP  the tiler heap descriptor page is written once (c66eb3e: a per-frame rewrite reset firmware-grown chunks, TRANSLATION_FAULT).
  R-SET   the driver descriptor set (SRT entry 0 of every VS/FS/compute job) has 17 entries of 32 B (794a571: the dummy sampler,
          now at entry 16, lay past a one-entry set and texelFetch read zeros).
  R-CS    decode_stream.py stream rules (INTERMITTENT.md: no RUN before WAIT on FLUSH_CACHE2, bit 38 of RUN_IDVS, FAU count byte,
          descriptor alignment, no TEM pointer in r44/r45 at RUN_FRAGMENT, ...).
BO flag rules (R-FLAGS) are enforced where the BO is allocated (fake_flag_rule in the fake backend)."""
import importlib.util, json, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('decode_stream', os.path.join(HERE, '..', '..', 'libkbase', 'tools', 'decode_stream.py'))
ds = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ds)
ds.RUNS = (4, 6, 7, 12)      # v13 RUN_IDVS2 is op 12; stock decode_stream only knows the v11 ops
ds.OPN[12] = 'RUN_IDVS2'

MASK48 = (1 << 48) - 1
GEOM_BYTES = 65536
SET_BYTES = 17 * 32          # MALI_DUMMY_SAMPLER_IDX + 1 entries of KB_DESC_SET_STRIDE
VA_LO, VA_HI = 0x5f00000000, 0x6000000000   # the fake backend's bump allocator window (header words like 0x5000000019 are not VAs)
HEAP_LO, HEAP_HI = 0xf0000000, 0xf8000000
MAPS = {   # (name, first reg, bytes to follow, kind) -- same as cmp_cmd.py
    (11, 6): [('SRT_v', 0, 64, 'srt'), ('SRT_f', 4, 64, 'srt'), ('FAU_v', 8, 0, 'fau'), ('FAU_f', 12, 0, 'fau'), ('SPD_pos', 16, 32, 'b'),
              ('SPD_vary', 18, 32, 'b'), ('SPD_frag', 20, 32, 'b'), ('TSD', 24, 32, 'b'), ('tiler_ctx', 40, 128, 'tiler'), ('blend', 50, 16, 'b'),
              ('ZSD', 52, 32, 'b')],
    (13, 12): [('SRT_v', 0, 64, 'srt'), ('SRT_f', 2, 64, 'srt'), ('FAU_v', 8, 0, 'fau'), ('FAU_f', 10, 0, 'fau'), ('SPD_v', 16, 32, 'b'),
               ('SPD_f', 18, 32, 'b'), ('TSD_v', 24, 32, 'b'), ('TSD_f', 26, 32, 'b'), ('tiler_ctx', 40, 128, 'tiler'), ('ZSD', 52, 32, 'b'),
               ('blend', 54, 16, 'b')],
}
COMPUTE_MAP = [('SRT', 0, 64, 'srt'), ('FAU', 8, 0, 'fau'), ('SPD', 16, 32, 'b'), ('TSD', 24, 32, 'b')]


class Mem:
    """Sparse GPU memory in 4 KiB pages (later dumps overwrite earlier ones)."""
    def __init__(s): s.pg = {}
    def add(s, va, data):
        for o in range(0, len(data), 4096): s.pg[(va + o) >> 12] = bytes(data[o:o + 4096])
    def read(s, va, n):
        out = bytearray()
        while n > 0:
            p = s.pg.get(va >> 12)
            k = min(n, 4096 - (va & 4095))
            out += p[va & 4095:(va & 4095) + k] if p else bytes(k)
            va += k; n -= k
        return bytes(out)


class Dev:
    def __init__(s, n):
        s.n, s.mem, s.reg = n, Mem(), {}
        s.shown = {}
        s.heap_page = {}     # geo BO id -> first non-zero content of the heap descriptor page


class Ctx:
    def __init__(s, dev, kick, arch, out, viol):
        s.dev, s.kick, s.arch, s.out, s.viol = dev, kick, arch, out, viol
        s.regions = sorted(dev.reg.values(), key=lambda r: r['va'])

    def find(s, va):
        for r in s.regions:
            if r['va'] <= va < r['va'] + r['len']: return r
        return None

    def violate(s, rule, msg):
        s.viol.append('RULE %s: dev%d kick %d: %s' % (rule, s.dev.n, s.kick, msg))

    def looks_va(s, v):
        v &= MASK48
        return VA_LO <= v < VA_HI or HEAP_LO <= v < HEAP_HI

    def nv(s, q, fm=63, where='', check=True):
        """Normalise a 64-bit value that may be a GPU address (+ low flag bits fm, + high byte). Non-addresses stay numbers."""
        lo = (q & MASK48) & ~fm
        if not s.looks_va(lo): return '%#x' % q
        r = s.find(lo)
        if not r:
            if check: s.violate('R-VA', '%s %#x is not inside any BO' % (where or 'value', q & MASK48))
            return 'VA?%#x' % lo
        if not r['live']:
            if check: s.violate('R-VA', '%s %#x points into freed BO%d' % (where or 'value', q & MASK48, r['id']))
        t = ('HEAP%d' if r['kind'] else 'BO%d') % r['id']
        t += '+%#x' % (lo - r['va']) if lo != r['va'] else ''
        if not r['live']: t += '(freed)'
        if q & fm: t += '|%#x' % (q & fm)
        if q >> 48: t += '^%#x' % (q >> 48)
        return t

    def words(s, va, n, where, check=True):
        """Descriptor bytes as text lines of 8 dwords; aligned qwords holding an address are normalised."""
        b = s.dev.mem.read(va, n)
        w = struct.unpack('<%dI' % (n // 4), b)
        toks = []
        i = 0
        while i < len(w):
            q = w[i] | (w[i + 1] << 32) if i + 1 < len(w) else 0
            if where in ('TSD', 'TSD_v', 'TSD_f') and i == 2 and q & ((1 << 40) - 1):
                # tls_fill stores VA >> 8 in words 2/3; bit 60 is a
                # separate enable flag, not part of the 48-bit address.
                addr = (q & ((1 << 40) - 1)) << 8
                toks.append((s.nv(addr, 0, '%s.tls' % where, check) + '^%#x' % (q >> 40), 2)); i += 2
            elif i + 1 < len(w) and i % 2 == 0 and s.looks_va(q):
                toks.append((s.nv(q, 63, '%s+%#x' % (where, i * 4), check), 2)); i += 2
            else:
                toks.append(('%08x' % w[i], 1)); i += 1
        lines, cur, cnt = [], [], 0
        for t, k in toks:
            cur.append(t); cnt += k
            if cnt >= 8: lines.append('      ' + ' '.join(cur)); cur, cnt = [], 0
        if cur: lines.append('      ' + ' '.join(cur))
        return lines


def render_job(c, e, op_name):
    """One RUN_*: register file + every reachable descriptor, normalised; rules R-VA/R-GEOM/R-HEAP/R-SET."""
    out, r = c.out, e['regs']
    arch = c.arch
    mp = COMPUTE_MAP if e['op'] == 4 else MAPS[(arch, e['op'])] if (arch, e['op']) in MAPS else None
    out.append('  %s regs:' % op_name)
    done = set()
    if e['op'] == 7:   # RUN_FRAGMENT: FBD at r40
        q = ds.pair(r, 40); done |= {40, 41}
        out.append('    FBD r40 = %s' % c.nv(q, 63, 'FBD'))
        lo = q & MASK48 & ~63
        if c.looks_va(lo) and c.find(lo):
            ext = (q >> 1) & 1
            out.append('    FBD'); out += c.words(lo, 128, 'FBD')
            if ext: out.append('    ZS/CRC ext'); out += c.words(lo + 128, 64, 'ZSEXT')
            out.append('    RTD'); out += c.words(lo + 128 + 64 * ext, 64, 'RTD')
    elif mp:
        for nm, reg, n, kind in mp:
            q = ds.pair(r, reg); done |= {reg, reg + 1}
            if not q: continue
            fm = 15 if kind in ('srt',) else 0
            if nm == 'blend': fm = 15
            out.append('    %s r%d = %s' % (nm, reg, c.nv(q, fm, nm)))
            lo = q & MASK48 & ~fm
            if not (c.looks_va(lo) and c.find(lo)): continue
            if kind == 'fau':
                cnt = q >> 56
                out.append('    %s words (%d qwords)' % (nm, cnt)); out += c.words(lo, 8 * cnt, nm)
                continue
            out.append('    %s' % nm); out += c.words(lo, n, nm)
            if kind == 'srt':
                for i in range(n // 16):
                    en = struct.unpack_from('<4I', c.dev.mem.read(lo + 16 * i, 16))
                    va = (en[0] | en[1] << 32) & MASK48
                    if not c.looks_va(va): continue
                    sz = en[2]
                    out.append('    %s set%d size=%d' % (nm, i, sz))
                    if i == 0 and sz < SET_BYTES and e['op'] in (4, 6, 12):
                        c.violate('R-SET', '%s entry 0 (driver set) is %d bytes, want >= %d (17 entries of 32 B: dummy sampler idx 16)' % (nm, sz, SET_BYTES))
                    out += c.words(va, min(sz, 1024) & ~7, '%s.set%d' % (nm, i))
                    rr = c.find(va)
                    if rr and va + sz > rr['va'] + rr['len']:
                        c.violate('R-VA', '%s set%d [%#x, +%d) runs past the end of BO%d' % (nm, i, va, sz, rr['id']))
            if kind == 'tiler':
                check_tiler(c, lo)
    # everything else: raw registers, pairs holding an address normalised
    rest = []
    for reg in sorted(r):
        if reg in done or not r[reg]: continue
        if reg + 1 in r and c.looks_va(ds.pair(r, reg)) and reg % 2 == 0 and c.find(ds.pair(r, reg) & MASK48 & ~63):
            rest.append('r%d=%s' % (reg, c.nv(ds.pair(r, reg), 63, 'r%d' % reg))); done.add(reg + 1)
        elif reg not in done: rest.append('r%d=%x' % (reg, r[reg]))
    if rest: out.append('    regs: ' + ' '.join(rest))


def check_tiler(c, lo):
    d = struct.unpack('<32I', c.dev.mem.read(lo, 128))
    heap = d[6] | d[7] << 32
    geom = d[8] | d[9] << 32
    size = (geom & 0xfff) << 12
    gva = geom & MASK48 & ~0xfff
    if size != GEOM_BYTES:
        c.violate('R-GEOM', 'geometry buffer %d bytes, want %d (64 KiB on v11 and v13)' % (size, GEOM_BYTES))
    r = c.find(gva)
    if not r: c.violate('R-GEOM', 'geometry buffer address %#x not inside a BO' % gva); return
    if gva + size > r['va'] + r['len']: c.violate('R-GEOM', 'geometry buffer [%#x,+%d) runs past BO%d (%d bytes)' % (gva, size, r['id'], r['len']))
    h = c.find(heap & MASK48)
    if not h or (heap & MASK48) != gva - 4096 or h['id'] != r['id']:
        c.violate('R-GEOM', 'heap descriptor %#x is not the page before the geometry buffer %#x' % (heap & MASK48, gva))
    # R-HEAP: the heap descriptor page, once non-zero, must never be rewritten by the driver (the fake GPU never touches it)
    page = c.dev.mem.read(h['va'] if h else gva - 4096, 4096)
    prev = c.dev.heap_page.get(r['id'])
    if prev is None:
        if any(page): c.dev.heap_page[r['id']] = page
    elif prev != page:
        c.violate('R-HEAP', 'tiler heap descriptor page of BO%d rewritten after the first frame' % r['id'])
        c.dev.heap_page[r['id']] = page


def rules_idvs2(evs):
    """R-CS for v13's RUN_IDVS2 (op 12), which decode_stream.py does not know: flags word (V13-NEXT.md: malloc_enable bit 33 set, stage [51:48] = 0,
    no v11 'fragment SRT select' bit 38), FAU count byte (kb_cs_move48 truncation) for FAU_v r8 / FAU_f r10, tiler context 64-byte aligned."""
    errs = []
    for e in evs:
        if e['op'] != 12: continue
        w, r, where = e['w'], e['regs'], 'ins %d(RUN_IDVS2)' % e['i']
        if not (w >> 33) & 1: errs.append('%s: malloc_enable (bit 33) missing, word 0x%016x' % (where, w))
        if (w >> 48) & 15: errs.append('%s: stage field [51:48] = %d, want 0, word 0x%016x' % (where, (w >> 48) & 15, w))
        for nm, reg in (('FAUv', 8), ('FAUf', 10)):
            v = ds.pair(r, reg)
            if (v & ((1 << 56) - 1)) and not v >> 56: errs.append('%s: %s r%d has pointer but zero count byte' % (where, nm, reg))
        t = ds.pair(r, 40)
        if t and t % 64: errs.append('%s: tiler_ctx r40 = 0x%x not 64-byte aligned' % (where, t))
    return errs


def process(path, outpath, hook=None):
    devs, viol, out = {}, [], []
    warn = set()
    arch = None
    kicks = 0
    cur_regions = {}
    for line in open(path):
        e = json.loads(line)
        k = e['ev']
        if k == 'kick':
            d = devs.setdefault(e['dev'], Dev(e['dev']))
            cur = (d, e['n'])
        elif k == 'region':
            d = devs.setdefault(e['dev'], Dev(e['dev']))
            d.reg[e['id']] = dict(id=e['id'], va=int(e['va'], 16), len=e['len'], kind=e['kind'], flags=e['flags'], live=bool(e['live']))
        elif k == 'mem':
            devs[e['dev']].mem.add(int(e['va'], 16), bytes.fromhex(e['hex']))
        elif k == 'ring':
            d, n = devs[e['dev']], e['n']
            if arch is None: arch = int(os.environ.get('GOLDEN_ARCH', '11'))
            kicks += 1
            if hook: hook(d, e)
            q = struct.unpack('<%dQ' % (len(e['hex']) // 16), bytes.fromhex(e['hex']))
            evs = []
            ds.interp(q, d.mem, evs, 0, {})
            c = Ctx(d, n, arch, out, viol)
            out.append('kick dev%d #%d  (%d instructions)' % (d.n, n, len(q)))
            for r in c.regions:   # only BOs created/freed since the previous kick (VAs are not printed: they are not part of the contract)
                st = (r['len'], r['flags'], r['live'])
                if d.shown.get(r['id']) != st:
                    d.shown[r['id']] = st
                    out.append('  %s%d %s len=%#x flags=%#x' % ('HEAP' if r['kind'] else 'BO', r['id'], 'live' if r['live'] else 'FREED', r['len'], r['flags']))
            regs = {}
            for ev in evs:
                op, w = ev['op'], ev['w']
                a = (w >> 48) & 0xff
                if op == 1: regs[a] = w & 0xffffffff; regs[a + 1] = (w >> 32) & 0xffff
                elif op == 2: regs[a] = w & 0xffffffff
                elif op == 17:
                    v = ds.pair(regs, (w >> 40) & 0xff) + struct.unpack('<i', struct.pack('<I', w & 0xffffffff))[0]
                    regs[a] = v & 0xffffffff; regs[a + 1] = (v >> 32) & 0xffffffff
                if op in (0, 1, 2): continue
                if op in (21, 38, 50, 51, 52):   # memory ops: the address register pair (47:40) must hold an address inside a live BO
                    ar = (w >> 40) & 0xff
                    off = struct.unpack('<h', struct.pack('<H', w & 0xffff))[0] if op == 21 else 0
                    out.append('  %3d %-14s %016x addr=%s' % (ev['i'], ev['name'], w, c.nv((ds.pair(regs, ar) + off) & MASK48, 0, '%s ins %d address' % (ev['name'], ev['i']))))
                else:
                    out.append('  %3d %-14s %016x' % (ev['i'], ev['name'], w))
                if op in ds.RUNS: render_job(c, ev, ev['name'])
            for msg in ds.rules(dict(mem=d.mem), evs, arch) + rules_idvs2(evs):
                if 'scoreboard re-select' in msg:   # advisory: mixed compute+draw streams trip it, hardware dEQP passes them
                    warn.add(msg.split(': ', 1)[1] if ': ' in msg else msg); continue
                viol.append('RULE R-CS: dev%d kick %d: %s' % (d.n, n, msg))
    if warn: print('warn: %d RUN(s) after REQ_RESOURCE without a SET_STATE_IMM32 scoreboard re-select (advisory)' % len(warn))
    open(outpath, 'w').write('\n'.join(out) + '\n')
    return kicks, viol


if __name__ == '__main__':
    kicks, viol = process(sys.argv[1], sys.argv[2])
    seen = set()
    for v in viol:
        if v in seen: continue
        seen.add(v); print(v)
    print('golden.py: %d kicks, %d violations' % (kicks, len(viol)))
    sys.exit(1 if viol else 0)
