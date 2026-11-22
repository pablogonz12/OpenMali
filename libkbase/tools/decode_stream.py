#!/usr/bin/python3
# SPDX-License-Identifier: MIT
"""Offline CSF stream + descriptor decoder/checker for libkbase (no GPU, no Mesa link; own CS interpreter).

  decode_stream.py show   DUMP.jsonl                 print the decoded stream (ours, from out-fake dump) with RUN_* register files
  decode_stream.py show   --golden GOLDEN.jsonl      same for a PanVK ioctl trace (+ GOLDEN.mem snapshot next to it)
  decode_stream.py check  DUMP.jsonl [--golden G.jsonl --allow ALLOW.txt] [--arch 11]
        static rules (always) + field-by-field diff of every RUN_IDVS / RUN_FRAGMENT against the golden (when given).
        Prints only UNEXPLAINED findings, exits 1 if any. Allowlist line:  KEY [ours=0x..] [golden=0x..]   # why

Model: CS instructions are interpreted only for register state (MOVE32/MOVE48/ADD_IMM64/LOAD_MULTIPLE/CALL); RUN_* snapshot the file.
Descriptors are compared as raw 32-bit words; any 64-bit word where BOTH sides hold a GPU VA (0x5000000000..0x6200000000) is skipped.
Pointer registers followed (v11 IDVS): r16/18/20 SPD, r24 TSD, r50 blend, r52 ZSD, r8/12 FAU (count in the top byte), FBD r40 (RUN_FRAGMENT).
Limits: no execution, no shader/binary compare, v13 register map is NOT known (stream rules still apply)."""
import json, struct, sys, os, re

VA_LO, VA_HI = 0x5000000000, 0x6200000000
OPN = {0: 'NOP', 1: 'MOVE48', 2: 'MOVE32', 3: 'WAIT', 4: 'RUN_COMPUTE', 6: 'RUN_IDVS', 7: 'RUN_FRAGMENT', 9: 'FINISH_TILING', 11: 'FINISH_FRAGMENT',
       16: 'ADD_IMM32', 17: 'ADD_IMM64', 20: 'LOAD_MULTIPLE', 21: 'STORE_MULTIPLE', 22: 'BRANCH', 28: 'SET_STATE_IMM32', 32: 'CALL', 34: 'REQ_RESOURCE',
       36: 'FLUSH_CACHE2', 38: 'SYNC_SET32', 47: 'PROGRESS?', 48: 'HEAP_SET', 49: 'HEAP_OPERATION', 50: 'SYNC_ADD32', 51: 'SYNC_ADD64', 52: 'SYNC_SET64'}
RUNS = (4, 6, 7)


class Mem:
    """Sparse GPU memory: list of [va, bytearray] segments; later loads overwrite earlier ones."""
    def __init__(s): s.seg = []
    def add(s, va, data): s.seg.append((va, bytearray(data)))
    def find(s, va):
        for v, d in reversed(s.seg):
            if v <= va < v + len(d): return v, d
    def read(s, va, n):
        out = bytearray()
        while n > 0:
            f = s.find(va)
            if not f: out += b'\0' * n; break
            v, d = f
            k = min(n, v + len(d) - va)
            out += d[va - v: va - v + k]; va += k; n -= k
        return bytes(out)


def load_ours(path):
    mem, rings, regions, freed = Mem(), [], [], []
    for l in open(path):
        e = json.loads(l)
        k = e['ev']
        if k == 'region': regions.append((int(e['va'], 16), e['len'], e['kind']))
        elif k == 'mem': mem.add(int(e['va'], 16), bytes.fromhex(e['hex']))
        elif k == 'ring': rings.append(e)
        elif k == 'freed': freed.append((int(e['va'], 16), e['len'], e['seq']))
    return dict(mem=mem, rings=rings, regions=regions, freed=freed)


def load_golden(path):
    mem, rings = Mem(), []
    snap = {}
    mp = path[:-6] + '.mem' if path.endswith('.jsonl') else path + '.mem'
    if os.path.exists(mp):
        b = open(mp, 'rb').read(); o = 0
        while o + 40 <= len(b):
            magic, kick, va, ln, prot = struct.unpack_from('<5Q', b, o); o += 40
            if magic != 0x4d454d53: break
            snap[va] = b[o:o + ln]; o += ln  # last snapshot of each region wins
        for va, d in snap.items(): mem.add(va, d)
    for l in open(path):
        e = json.loads(l)
        if e['ev'] in ('blk', 'chunk') and 'hex' in e: mem.add(int(e['va'], 16), bytes.fromhex(e['hex']))
        elif e['ev'] == 'ring': rings.append(e)
    # PanVK submits 3 queues twice (setup, then the draw): keep the last 3 rings (draw submission).
    return dict(mem=mem, rings=rings[-3:], regions=[], freed=[])


def words(hexs): b = bytes.fromhex(hexs); return struct.unpack('<%dQ' % (len(b) // 8), b)


def interp(q, mem, out, depth=0, regs=None, ctx=None):
    """Interpret instruction qwords; append event dicts to out. regs: dict reg->u32 (shared across CALLs)."""
    regs = regs if regs is not None else {}
    ctx = ctx if ctx is not None else {}
    for i, w in enumerate(q):
        op, a = w >> 56, (w >> 48) & 0xff
        ev = dict(op=op, w=w, i=i, depth=depth, name=OPN.get(op, 'OP%d' % op))
        if op == 1: regs[a] = w & 0xffffffff; regs[a + 1] = (w >> 32) & 0xffff
        elif op == 2: regs[a] = w & 0xffffffff
        elif op == 17:
            v = ((regs.get((w >> 40) & 0xff, 0)) | regs.get(((w >> 40) & 0xff) + 1, 0) << 32) + struct.unpack('<i', struct.pack('<I', w & 0xffffffff))[0]
            regs[a] = v & 0xffffffff; regs[a + 1] = (v >> 32) & 0xffffffff
        elif op == 20:  # LOAD_MULTIPLE: first dst 55:48, addr pair 47:40, mask 31:16, off 15:0 (signed)
            ap = (w >> 40) & 0xff; base = regs.get(ap, 0) | regs.get(ap + 1, 0) << 32
            off = struct.unpack('<h', struct.pack('<H', w & 0xffff))[0]; m = (w >> 16) & 0xffff
            for b in range(16):
                if m >> b & 1: regs[a + b] = struct.unpack('<I', mem.read(base + off + 4 * b, 4))[0]
        elif op == 32 and depth < 4:  # CALL: addr pair 47:40... (Mesa: addr reg 47:40? we follow the tracer's convention)
            ra, rb = (w >> 40) & 0xff, (w >> 32) & 0xff
            base = regs.get(ra, 0) | regs.get(ra + 1, 0) << 32; ln = regs.get(rb, 0)
            ev['call'] = (base, ln)
            if ln and ln <= 1 << 16:
                sub = struct.unpack('<%dQ' % (ln // 8), mem.read(base, ln))
                out.append(ev)
                interp(sub, mem, out, depth + 1, regs, ctx)
                continue
        if op in RUNS or op in (11, 36, 34, 3, 28, 48, 49, 9, 51, 52, 38, 32):
            ev['regs'] = dict(regs) if op in RUNS else {}
        out.append(ev)
    return out


def pair(r, a): return r.get(a, 0) | r.get(a + 1, 0) << 32
def isva(v): return VA_LO <= v < VA_HI


# ---- descriptor map for RUN_IDVS (v11). (name, first reg, kind, bytes)
IDVS_PTRS = [('SPD_pos', 16, 'ptr', 32), ('SPD_vary', 18, 'ptr', 32), ('SPD_frag', 20, 'ptr', 32), ('TSD', 24, 'ptr', 32),
             ('ZSD', 52, 'ptr', 32), ('blend', 50, 'ptrlo4', 16)]
IDVS_ALIGN = {'SPD_pos': 32, 'SPD_vary': 32, 'SPD_frag': 32, 'TSD': 64, 'ZSD': 32, 'blend': 64, 'tiler_ctx': 64, 'FBD': 64}


def desc_words(mem, va, n):
    """32-bit words of a descriptor; any aligned qword holding a GPU VA is replaced by the marker 'VA' in both halves."""
    w = list(struct.unpack('<%dI' % (n // 4), mem.read(va, n)))
    for i in range(0, len(w) - 1, 2):
        if isva((w[i + 1] << 32 | w[i]) & 0xffffffffffff): w[i] = w[i + 1] = 'VA'
    return w


def collect(side):
    """Run the interpreter over all rings of a side; return list of events."""
    out = []
    for r in side['rings']:
        regs = {}
        interp(words(r['hex']), side['mem'], out, 0, regs)
    return out


def snapshot(side, ev, arch=11):
    """Flatten a RUN_* event to {key: value} for diffing: registers + pointed descriptor words."""
    r, m, d = ev['regs'], side['mem'], {}
    if ev['op'] == 6:
        d['word'] = ev['w']
        for k in list(range(32, 40)) + [42, 43, 44, 45, 48, 56, 57, 58, 60, 61]: d['r%d' % k] = r.get(k, 0)
        for nm, reg, kind, n in IDVS_PTRS:
            v = pair(r, reg)
            if kind == 'ptrlo4': d[nm + '.cnt'] = v & 15; v &= ~15
            d[nm + '.set'] = int(v != 0)
            if isva(v):
                for j, x in enumerate(desc_words(m, v, n)): d['%s.w%d' % (nm, j)] = x
        for nm, reg in (('FAUv', 8), ('FAUf', 12)):
            v = pair(r, reg); cnt = v >> 56; v &= (1 << 56) - 1
            d[nm + '.cnt'] = cnt
            if isva(v) and cnt <= 64:
                fw = list(struct.unpack('<%dI' % (2 * cnt), m.read(v, 8 * cnt)))
                for x in sorted(set(fw) - {0}): d['%s.has_%08x' % (nm, x)] = 1   # FAU is compared as a SET of non-zero words (layout is shader-dependent)
        for nm, reg in (('tiler_ctx', 40),):
            d[nm + '.set'] = int(isva(pair(r, reg)))
    elif ev['op'] == 7:
        d['word'] = ev['w']
        for k in (42, 43, 44, 45): d['r%d' % k] = r.get(k, 0)
        v = pair(r, 40)
        d['FBD.set'] = int(isva(v & ~63))
        if isva(v & ~63):
            ext = (v >> 1) & 1                              # tag bit 1: ZS/CRC extension block (64 B) between FBD and RT descriptors
            for j, x in enumerate(desc_words(m, v & ~63, 128)): d['FBD.w%d' % j] = x
            for j, x in enumerate(desc_words(m, (v & ~63) + 128 + 64 * ext, 64)): d['RT.w%d' % j] = x
            d['FBD.tag'] = v & 63
    return d


def allow_load(p):
    A = []
    if not p or not os.path.exists(p): return A
    for l in open(p):
        l = l.split('#')[0].strip()
        if not l: continue
        t = l.split(); k = t[0]; o = g = None
        for x in t[1:]:
            if x.startswith('ours='): o = int(x[5:], 0)
            if x.startswith('golden='): g = int(x[7:], 0)
        A.append((re.compile(k.replace('.', r'\.').replace('*', '.*') + '$'), o, g))
    return A


def allowed(A, key, o, g):
    return any(rx.match(key) and (ov is None or ov == o) and (gv is None or gv == g) for rx, ov, gv in A)


def diff_runs(ours, gold, A):
    res = []
    for kind, nm in ((6, 'IDVS'), (7, 'FRAG')):
        oe = [e for e in ours if e['op'] == kind]; ge = [e for e in gold if e['op'] == kind]
        if len(oe) != len(ge) and ge: res.append('RUN_%s count: ours %d golden %d' % (nm, len(oe), len(ge)))
        for n, (o, g) in enumerate(zip(oe, ge)):
            do, dg = snapshot(ours_side, o), snapshot(gold_side, g)
            for k in sorted(set(do) | set(dg), key=lambda s: (re.sub(r'\d+', '', s), int(re.sub(r'\D', '', s) or 0))):
                a, b = do.get(k), dg.get(k)
                if a == b: continue
                key = '%s.%s' % (nm.lower(), k)
                if allowed(A, key, a if isinstance(a, int) else None, b if isinstance(b, int) else None): continue
                res.append('%s: ours=%s golden=%s' % (key, hex(a) if isinstance(a, int) else a if a is not None else '-', hex(b) if isinstance(b, int) else b if b is not None else '-'))
    return res


# ---- static rules on OUR stream
def rules(side, events, arch):
    errs = []
    pend_flush, since_req, saw_run = {}, None, False
    pend_frag = None  # a RUN_FRAGMENT whose FINISH_FRAGMENT has not been seen yet (see the rule below)
    pend_store = {}   # STORE_STATE (op 40, async, signal slot 51:48): its result is only valid after WAIT on that slot
    sb_needed = False
    if not events: errs.append('no instructions were checked (empty ring dump?)')
    for e in events:
        op = e['op']; w = e['w']
        where = 'ins %d(%s)' % (e['i'], e['name'])
        if op in (38, 39, 51) and (pend_flush or pend_store):
            errs.append('%s: sync op before WAIT on async FLUSH_CACHE2/STORE_STATE slot(s) %s (INTERMITTENT.md)' % (where, sorted(set(pend_flush) | set(pend_store))))
        # Frame-ordering.md § Round 2, the bug round 1 shipped: a FINISH_FRAGMENT with wait mask 0 is a synchronous
        # instruction, so the packer drops its signal slot and the slot never increments (Mesa cs_apply_async,
        # cs_builder.h:866-889) - every wait on that slot, including the tail flush wait and the retire, is then vacuous
        # and the fence can signal before the fragment/tile writeback. (A mask-0 SYNC_ADD64 is not flagged on its own:
        # the ops/event path legitimately posts its own adds in a kick that also carries a fragment job.)
        if op == 7 and not pend_frag:   # RUN_FRAGMENT opens a fragment job: the frame must close it with FINISH_FRAGMENT
            pend_frag = where               # (slot 4), also on a clear-only frame - the tail then waits that slot before it
        if op == 11 and pend_frag:          # cleans L2/LSC, so without it the tile writeback is never cleaned
            pend_frag = None
        if op == 51 and pend_frag:          # the retire word: a fragment job still open here never signalled
            errs.append('%s: RUN_FRAGMENT (%s) with no FINISH_FRAGMENT before the retire SYNC_ADD64: no slot is signalled, so the tail cannot wait the writeback' % (where, pend_frag))
            pend_frag = None
        if op == 11 and (w >> 48) & 0xf and not (w >> 16) & 0xffff:
            errs.append('%s: FINISH_FRAGMENT signals slot %u with wait mask 0: the signal slot is dropped, nothing can wait on it' % (where, (w >> 48) & 0xf))
        if op == 36: pend_flush[(w >> 48) & 15] = where
        elif op == 40: pend_store[(w >> 48) & 15] = where
        elif op == 3:
            for s in list(pend_flush):
                if (w >> 16) & (1 << s): del pend_flush[s]
            for s in list(pend_store):
                if (w >> 16) & (1 << s): del pend_store[s]
        elif op == 34:
            if saw_run: sb_needed = True
        elif op == 28 and ((w >> 32) & 0xff) == 0: sb_needed = False
        if op in RUNS:
            if pend_flush: errs.append('%s: RUN before WAIT on FLUSH_CACHE2 slot(s) %s (INTERMITTENT.md)' % (where, sorted(pend_flush)))
            if sb_needed: errs.append('%s: no SET_STATE_IMM32 scoreboard re-select after REQ_RESOURCE' % where)
            saw_run = True
            r = e['regs']
            if op == 7:
                for k in (44, 45):
                    if r.get(k, 0): errs.append('%s: r%d = 0x%x not zero before RUN_FRAGMENT (aliases TEM pointer)' % (where, k, r[k]))
                v = pair(r, 40)
                if v & 63 and False: pass
                if isva(v & ~63) and (v & ~63) % 64: errs.append('%s: FBD misaligned' % where)
            if op == 6:
                if not (w >> 38) & 1: errs.append('%s: RUN_IDVS bit 38 (fragment SRT select) missing, word 0x%016x' % (where, w))
                for nm, reg in (('FAUv', 8), ('FAUf', 12)):
                    v = pair(r, reg)
                    if (v & ((1 << 56) - 1)) and not v >> 56: errs.append('%s: %s r%d has pointer but zero count byte (MOVE48 truncation of a >48-bit value?)' % (where, nm, reg))
                for nm, reg, kind, n in IDVS_PTRS + [('tiler_ctx', 40, 'ptr', 64)]:
                    v = pair(r, reg) & ~15 if kind == 'ptrlo4' else pair(r, reg)
                    al = IDVS_ALIGN[nm]
                    if v and v % al: errs.append('%s: %s r%d = 0x%x not %d-byte aligned' % (where, nm, reg, v, al))
    if pend_flush or pend_store: errs.append('stream ends with async FLUSH_CACHE2/STORE_STATE not waited: %s' % sorted(set(pend_flush) | set(pend_store)))
    return errs


def rules_global(side):
    errs = []
    for r in side['rings']:
        va, size = int(r['va'], 16), r['size']
        if va % 4096: errs.append('ring 0x%x not 4 KiB aligned' % va)
        if size & (size - 1) or size < 4096: errs.append('ring size %d not a power of two >= 4096' % size)
        if r['start'] % 64 or r['insert'] % 64: errs.append('ring submit %d..%d not 64-byte aligned (firmware prefetch unit)' % (r['start'], r['insert']))
    # no pointer into a freed region: scan non-zero live qwords that are reachable (descriptor pages of live regions) for VAs inside freed regions
    fr = [(v, v + n, s) for v, n, s in side['freed']]
    live = [(v, v + n) for v, n, k in side['regions']]
    if fr:
        for seg_va, d in side['mem'].seg:
            if any(lv <= seg_va < le for lv, le in live) is False: continue
            for o in range(0, len(d) - 7, 8):
                x = struct.unpack_from('<Q', d, o)[0]
                if isva(x & 0xffffffffffff) and any(a <= (x & 0xffffffffffff) < b for a, b, s in fr) and not any(a <= (x & 0xffffffffffff) < b for a, b in live):
                    errs.append('pointer 0x%x at 0x%x points into a freed region' % (x & 0xffffffffffff, seg_va + o)); break
    return errs


def show(side, events, arch=11):
    for e in events:
        w = e['w']; s = '%s%3d %016x %s' % ('  ' * e['depth'], e['i'], w, e['name'])
        if e['op'] in RUNS:
            s += '  regs:' + ' '.join('r%d=%x' % (k, v) for k, v in sorted(e['regs'].items()) if v)
        print(s)


ours_side = gold_side = None
if __name__ == '__main__':
    a = sys.argv[1:]
    cmd = a.pop(0)
    gold = allow = None; arch = 11
    f = []
    while a:
        x = a.pop(0)
        if x == '--golden': gold = a.pop(0)
        elif x == '--allow': allow = a.pop(0)
        elif x == '--arch': arch = int(a.pop(0))
        else: f.append(x)
    if cmd == 'show':
        side = load_golden(f[0] if not gold else gold) if (gold or not f) else load_ours(f[0])
        show(side, collect(side)); sys.exit(0)
    ours_side = load_ours(f[0]); events = collect(ours_side)
    errs = rules_global(ours_side) + rules(ours_side, events, arch)
    if gold:
        gold_side = load_golden(gold)
        errs += diff_runs(events, collect(gold_side), allow_load(allow))
    print('decode_stream: checked %d instructions in %d ring(s), %d RUN_* ' % (len(events), len(ours_side['rings']), sum(1 for e in events if e['op'] in RUNS)))
    for e in errs: print('FAIL', e)
    sys.exit(1 if errs else 0)
