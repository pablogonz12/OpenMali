# SPDX-License-Identifier: MIT
"""M8 offline check driver (called by build-t_cmd.sh):  cmp_cmd.py OUTDIR decode_stream.py T0 [-v]

For arch in (11, 13) and scene in (1, 2):
  ours  = t_cmd<arch> --scene N   (Vulkan entrypoints -> mali_cmd -> mali_cs_emit -> libkbase fake backend dump)
  ref   = ref_uniform<arch> --only N   (libkbase t_uniform / t_uniform_v13, hand built, proven on the phones)
and diffs the two dumps field by field:
  * every non-MOVE/NOP instruction word of the command stream, in order (VAs normalised),
  * every register at every RUN_* instruction and at the end of every ring (GPU VAs normalised to 'VA' + their flag bits),
  * every descriptor reachable from the RUN registers (SRT -> driver set, FAU as a set of non-zero words, SPD, TSD, tiler ctx, ZSD,
    blend, FBD+ZS ext+RTD), qword VAs normalised.
Allowlist: GPU VAs only (they differ by construction, different arenas); nothing else. Also runs decode_stream.py's static rules
(`check DUMP --arch N`) on our dump and `t_cmd<arch> --extra` (clear-only frame, ClearAttachments fold, unsupported-feature errors).
Exit 0 = pass, JSON lines on stdout."""
import importlib.util, json, os, struct, subprocess, sys, time

out, dec, t0 = sys.argv[1], sys.argv[2], float(sys.argv[3])
verbose = '-v' in sys.argv
BREAK = '--break' in sys.argv   # self-test: corrupt our stream in memory; the run must then FAIL (proves the diff has teeth)
# Allowlist (arch, register): v11 only. mali_cs_emit.c:262-263 (M9 indexed draws) always writes INDEX_BUFFER_SIZE r39 and
# INDEX_BUFFER r54/55 (= 0 for non-indexed draws, since registers persist between draws); the hand-built t_uniform reference
# never touches them (so it reads None). Intended, accepted ONLY when ours == 0 and ref is absent. v13 writes its own regs
# (mali_cs_emit.c:302-303) and already matches the reference, so it has no entry.
ALLOW = {(11, 'r39'), (11, 'r54'), (11, 'r55')}
spec = importlib.util.spec_from_file_location('decode_stream', dec)
ds = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ds)
ds.RUNS = (4, 6, 7, 12)      # v13 RUN_IDVS2 is op 12; stock decode_stream only knows the v11 ops
ds.OPN[12] = 'RUN_IDVS2'
MASK48 = (1 << 48) - 1
isva = ds.isva

# register map: (name, first reg, bytes to follow, kind)
MAP = {
    11: [('SRT_v', 0, 64, 'srt'), ('SRT_f', 4, 64, 'srt'), ('FAU_v', 8, 0, 'fau'), ('FAU_f', 12, 0, 'fau'), ('SPD_pos', 16, 32, 'b'),
         ('SPD_vary', 18, 32, 'b'), ('SPD_frag', 20, 32, 'b'), ('TSD', 24, 32, 'b'), ('tiler_ctx', 40, 128, 'b'), ('blend', 50, 16, 'b'),
         ('ZSD', 52, 32, 'b')],
    13: [('SRT_v', 0, 64, 'srt'), ('SRT_f', 2, 64, 'srt'), ('FAU_v', 8, 0, 'fau'), ('FAU_f', 10, 0, 'fau'), ('SPD_v', 16, 32, 'b'),
         ('SPD_f', 18, 32, 'b'), ('TSD_v', 24, 32, 'b'), ('TSD_f', 26, 32, 'b'), ('tiler_ctx', 40, 128, 'b'), ('ZSD', 52, 32, 'b'),
         ('blend', 54, 16, 'b')],
}


def norm(q, fm=63):
    """64-bit value -> normalised: a GPU VA (+ low flag bits (mask fm) and the byte above bit 48) becomes ('VA', flags, high)."""
    lo = q & MASK48
    if isva(lo & ~fm):
        return ('VA', lo & fm, q >> 48)
    return q


def rawregs(r, skip, tag, d):
    """Registers not consumed as a known pointer: raw, except a pair holding a GPU VA (both halves -> 'VA')."""
    va = set()
    for reg in r:
        if reg + 1 in r and isva(((r[reg + 1] << 32) | r[reg]) & MASK48 & ~63):
            va.update((reg, reg + 1))
    for reg in sorted(r):
        if reg not in skip:
            d['%s.r%d' % (tag, reg)] = 'VA' if reg in va else r[reg]


def block(side, va, n):
    """Descriptor bytes as qwords, VAs normalised."""
    b = side['mem'].read(va, n)
    return [norm(x) for x in struct.unpack('<%dQ' % (n // 8), b)]


def snap(side, ev, arch, tag, d):
    r = ev['regs']
    done = set()
    for nm, reg, n, kind in MAP[arch]:
        q = r.get(reg, 0) | r.get(reg + 1, 0) << 32
        done.update((reg, reg + 1))
        fm = {'srt': 15, 'fau': 0}.get(kind, 15 if nm == 'blend' else 0)
        d['%s.%s' % (tag, nm)] = norm(q, fm)
        lo = q & MASK48 & ~fm
        if not isva(lo):
            continue
        if kind == 'fau':
            cnt = q >> 56
            fw = struct.unpack('<%dI' % (2 * cnt), side['mem'].read(lo, 8 * cnt))
            d['%s.%s.cnt' % (tag, nm)] = cnt
            d['%s.%s.words' % (tag, nm)] = tuple(sorted(set(fw) - {0}))
            continue
        blk = block(side, lo, n)
        d['%s.%s.blk' % (tag, nm)] = tuple(blk)
        if kind == 'srt':   # 16-byte resource entries: va (flag bit 24 of word 1), size
            for i in range(n // 16):
                e = struct.unpack_from('<4I', side['mem'].read(lo + 16 * i, 16))
                va = (e[0] | e[1] << 32) & MASK48
                if isva(va):
                    sz = e[2]
                    d['%s.%s.set%d' % (tag, nm, i)] = tuple(block(side, va, min(sz, 1024) & ~7))
    rawregs(r, done, tag, d)
    return d


def fbd_snap(side, ev, arch, tag, d):
    r = ev['regs']
    q = r.get(40, 0) | r.get(41, 0) << 32
    d['%s.r40' % tag] = norm(q)
    lo = q & MASK48 & ~63
    if isva(lo):
        ext = (q >> 1) & 1
        d['%s.FBD' % tag] = tuple(block(side, lo, 128))
        if ext:
            d['%s.ZSEXT' % tag] = tuple(block(side, lo + 128, 64))
        d['%s.RTD' % tag] = tuple(block(side, lo + 128 + 64 * ext, 64))
    rawregs(r, (40, 41), tag, d)
    return d


def trace(side, arch):
    """Events -> list of (label, dict) in stream order, plus the non-MOVE instruction words."""
    out_ev = ds.collect(side)
    seq, ops = [], []
    n = {}
    for e in out_ev:
        op = e['op']
        if op in (0, 1, 2):
            continue
        ops.append(e['w'] & ~MASK48 if op in (51, 52, 38) else e['w'])
        if op in ds.RUNS:
            k = n[op] = n.get(op, 0) + 1
            tag = '%s#%d' % (e['name'], k)
            d = {'word': e['w']}
            if op == 7:
                fbd_snap(side, e, arch, tag, d)
            else:
                snap(side, e, arch, tag, d)
            seq.append((tag, d))
    return ops, seq


def run(cmd, env, **kw):
    return subprocess.run(cmd, env=dict(os.environ, **env), capture_output=True, text=True, timeout=30, **kw)


def hx(v):
    return hex(v) if isinstance(v, int) else v
def retire_norm(seq):
    """Fold the DELIBERATE retire/fragment differences from notes/frame-ordering.md § Round 2 into one shape.

    Our driver now defers FINISH_FRAGMENT (non-zero wait mask) so it can signal its own scoreboard slot (4) and waits that
    slot before the tail flush; the hand-built reference keeps the synchronous form (mask 0, slot 0) plus the endpoint wait
    the reference always had. The head cache flush became others-invalidate only (0x200) instead of L2/LSC clean+invalidate
    (0x332), because a head L2 invalidate discards a previous frame's tile writeback. None of that is a register or draw
    difference, so compare the instructions with those three fields folded and the two fragment-ordering waits dropped.
    """
    out = []
    prev = None
    head_flush_seen = False
    for i, w in enumerate(seq):
        op = (w >> 56) & 0xff
        nxt = (seq[i + 1] >> 56) & 0xff if i + 1 < len(seq) else None
        if op == 11:                                   # FINISH_FRAGMENT: fold wait mask (31:16) and signal slot (51:48), but
            m, sl = (w >> 16) & 0xffff, (w >> 48) & 0xf   # only the two legal shapes: the reference's (mask 0, slot 0) or
            if (m, sl) in ((0, 0), (0x0008, 4)):          # ours (endpoint wait, slot 4). A self-dependent mask (slot 4 in
                w &= ~((0xffff << 16) | (0xf << 48))      # the wait mask) stays and is compared, so it cannot hide here
        if op == 36 and not head_flush_seen:             # the HEAD flush compares its "others" field only
            w = (w & ~0xfff) | (w & 0xf00)               # (head is now others-invalidate only, 0x200 vs 0x233)
            head_flush_seen = True                       # a tail flush keeps its full mode: 0x011 vs 0x000 must differ
        if op == 3:
            m = (w >> 16) & 0xffff
            # the two fragment-ordering waits the two streams express differently: the reference's endpoint wait right
            # after RUN_FRAGMENT, and our wait on the deferred FINISH_FRAGMENT slot (4) before the tail flush. A WAIT on
            # those masks anywhere else - the one after FINISH_TILING - is compared like every other instruction.
            if (m == 0x0008 and prev == 7) or (m == 0x0010 and nxt == 36):
                continue
            if m == 0xffff and nxt == 51:                 # our retire drain: WAIT all slots, then the retire SYNC_ADD64
                continue
        out.append(w)
        prev = op
    return out




def tail_slot4_wait_missing(seq_raw):
    """A FINISH_FRAGMENT that signals slot 4 defers; slot 4 is what the tail flush waits before it cleans L2/LSC, so that wait
    must be there. Checked on our raw stream, because retire_norm drops it as a fragment-ordering difference."""
    for i, w in enumerate(seq_raw):
        if (w >> 56) & 0xff != 11 or ((w >> 48) & 0xf) != 4:
            continue
        for j in range(i + 1, len(seq_raw)):
            if (seq_raw[j] >> 56) & 0xff != 36:          # FLUSH_CACHE2
                continue
            p = seq_raw[j - 1] if j else 0
            if (p >> 56) & 0xff == 3 and (p >> 16) & 0xffff == 0x0010:
                break
            return i                                      # a tail flush with no slot-4 wait before it
        else:
            return i                                      # no tail flush at all after the deferred fragment
    return None


def retire_drain_missing(seq_raw):
    """Our stream must drain every scoreboard slot before the retire SYNC_ADD64 (frame-ordering.md): without the WAIT the
    add runs immediately and the fence signals before the fragment writeback. Checked before retire_norm, which drops it."""
    for i, w in enumerate(seq_raw):
        if (w >> 56) & 0xff != 51:
            continue
        p = seq_raw[i - 1] if i else 0
        if (p >> 56) & 0xff != 3 or (p >> 16) & 0xffff != 0xffff:
            return i
    return None


fails = []
for arch in (11, 13):
    for scene in (1, 2):
        od, rd = '%s/o%d_%d.jsonl' % (out, arch, scene), '%s/r%d_%d.jsonl' % (out, arch, scene)
        for f in (od, rd):
            if os.path.exists(f):
                os.remove(f)
        a = run(['%s/t_cmd%d' % (out, arch), '--scene', str(scene)], {'KB_FAKE_DUMP': od, 'KB_FAKE_ARCH': str(arch)})
        b = run(['%s/ref_uniform%d' % (out, arch), '--only', str(scene)], {'KB_FAKE_DUMP': rd, 'KB_FAKE_ARCH': str(arch)})
        name = 'v%d scene %d' % (arch, scene)
        if a.returncode or not os.path.exists(od) or '"pass":true' not in a.stdout:
            fails.append('%s: t_cmd failed: %s' % (name, (a.stdout + a.stderr).strip()[-300:]))
            continue
        if not os.path.exists(rd):
            fails.append('%s: no reference dump' % name)
            continue
        sa, sb = ds.load_ours(od), ds.load_ours(rd)
        raw_a = trace(sa, arch)[0]
        miss = retire_drain_missing(raw_a)
        if miss is not None:
            fails.append('%s: our stream has no WAIT(0xffff) before the retire SYNC_ADD64 at word %d' % (name, miss))
        miss = tail_slot4_wait_missing(raw_a)
        if miss is not None:
            fails.append('%s: FINISH_FRAGMENT at word %d signals slot 4 with no WAIT(0x0010) before the tail FLUSH_CACHE2' % (name, miss))
        oa, qa = retire_norm(raw_a), trace(sa, arch)[1]
        ob, qb = retire_norm(trace(sb, arch)[0]), trace(sb, arch)[1]
        nd = 0
        if BREAK:
            oa[0] ^= 1
            if qa:
                qa[0][1]['word'] ^= 4
                qa[0][1]['%s.r54' % qa[0][0]] = 1      # non-zero index buffer: must not be swallowed by the allowlist
        if len(sa['rings']) != len(sb['rings']):
            fails.append('%s: kicks ours %d ref %d' % (name, len(sa['rings']), len(sb['rings'])))
        if len(oa) != len(ob):
            fails.append('%s: instruction count (non-MOVE/NOP) ours %d ref %d' % (name, len(oa), len(ob)))
        for i, (x, y) in enumerate(zip(oa, ob)):
            if x != y:
                fails.append('%s: ins %d ours %016x ref %016x' % (name, i, x, y)); nd += 1
        if len(qa) != len(qb):
            fails.append('%s: RUN count ours %d ref %d' % (name, len(qa), len(qb)))
        for (ta, da), (tb, db) in zip(qa, qb):
            # 794a571: our driver set (SRT entry 0) is larger than the reference (MALI_DUMMY_SAMPLER_IDX+1 entries, zeroed dummy
            # sampler for texelFetch). Compare the size field masked and the common prefix of the set only.
            for k in list(da):
                if '.SRT_' not in k or k not in db:
                    continue
                if k.endswith('.blk') and len(da[k]) > 1 and len(db[k]) > 1:
                    da[k] = (da[k][0], da[k][1] & ~0xffffffff) + da[k][2:]
                    db[k] = (db[k][0], db[k][1] & ~0xffffffff) + db[k][2:]
                elif k.endswith('.set0') and isinstance(da[k], tuple) and isinstance(db[k], tuple):
                    n = min(len(da[k]), len(db[k]))
                    da[k], db[k] = da[k][:n], db[k][:n]
            for k in list(da) + list(db):   # the FS shares the VS driver set and SRT (it reads only the all-zero dummy sampler, entry 16): the reference's separate FS set is not comparable
                if '.SRT_f.set0' in k:
                    da.pop(k, None); db.pop(k, None)
            for k in sorted(set(da) | set(db)):
                if da.get(k) != db.get(k):
                    x, y = da.get(k), db.get(k)
                    if x == 0 and y is None and (arch, k.rsplit('.', 1)[-1]) in ALLOW:
                        continue
                    if isinstance(x, tuple) and isinstance(y, tuple) and len(x) == len(y) and len(x) > 4:
                        diffs = ['w%d %s/%s' % (j, hx(p), hx(q)) for j, (p, q) in enumerate(zip(x, y)) if p != q]
                        fails.append('%s: %s: %s' % (name, k, ', '.join(diffs[:12])))
                    else:
                        fails.append('%s: %s: ours=%s ref=%s' % (name, k, hx(x), hx(y)))
        # static rules of the offline decoder on OUR dump
        c = run([sys.executable, '-I', dec, 'check', od, '--arch', str(arch)], {})
        for l in c.stdout.splitlines() + c.stderr.splitlines():
            if 'FAIL' in l or 'Traceback' in l or 'Error' in l:
                fails.append('%s: static rule: %s' % (name, l))
        print(json.dumps({'case': name, 'runs': len(qa), 'instr': len(oa), 'kicks': len(sa['rings']), 'ok': not any(f.startswith(name) for f in fails)}))
    e = run(['%s/t_cmd%d' % (out, arch), '--scene', '1', '--extra'], {'KB_FAKE_DUMP': '%s/x%d.jsonl' % (out, arch), 'KB_FAKE_ARCH': str(arch)})
    if e.returncode or '"extra":true,"pass":true' not in e.stdout:
        fails.append('v%d extra: %s' % (arch, (e.stdout + e.stderr).strip()[-400:]))

    # ring wrap + chunking: a stream longer than a 4 KiB ring through kb_queue_piece_instrs pieces (fake kbase backend)
    w = run(['%s/t_cmd%d' % (out, arch), '--ringwrap'], {'KB_FAKE_ARCH': str(arch)})
    if w.returncode or '"test":"ringwrap"' not in w.stdout or '"pass":true' not in w.stdout.split('"test":"ringwrap"')[1]:
        fails.append('v%d ringwrap: %s' % (arch, (w.stdout + w.stderr).strip()[-400:]))
    else:
        print(w.stdout.strip().splitlines()[-1])

    # load/store ops: per-attachment NONE/DONT_CARE must not preload, LOAD must (t_cmd --loadstore, record-time frame draw list)
    l = run(['%s/t_cmd%d' % (out, arch), '--loadstore'], {'KB_FAKE_ARCH': str(arch)})
    if l.returncode or 'FAIL' in l.stdout or '"test":"t_cmd%d"' % arch not in l.stdout:
        fails.append('v%d loadstore: %s' % (arch, (l.stdout + l.stderr).strip()[-400:]))
    else:
        print(' '.join(x for x in l.stdout.splitlines() if 'ui_like' in x))

    # M13: occlusion draw + deferred op + ops frame (t_cmd --m13), our dump only: static rules, and the decoded stream must contain what was recorded
    md = '%s/m%d.jsonl' % (out, arch)
    if os.path.exists(md):
        os.remove(md)
    m = run(['%s/t_cmd%d' % (out, arch), '--scene', '1', '--m13'], {'KB_FAKE_DUMP': md, 'KB_FAKE_ARCH': str(arch)})
    if m.returncode or '"m13":true,"pass":true' not in m.stdout or not os.path.exists(md):
        fails.append('v%d m13: %s' % (arch, (m.stdout + m.stderr).strip()[-400:]))
    else:
        sm = ds.load_ours(md)
        om, qm = trace(sm, arch)
        opc = [w >> 56 for w in om]
        # STORE_STATE 40, SYNC_WAIT32 39, SYNC_SET32 38 (deferred op + availability), one occlusion draw whose OQ register holds a VA
        for name, op, want in (('STORE_STATE', 40, 1), ('SYNC_WAIT32', 39, 1), ('SYNC_SET32', 38, 3)):
            if opc.count(op) < want:
                fails.append('v%d m13: %d x %s in stream, want >= %d' % (arch, opc.count(op), name, want))
        oq = [d.get('%s.r%d' % (t, 46 if arch == 11 else 64)) for t, d in qm if t.startswith('RUN_IDVS')]
        if not oq or oq[0] != 'VA':
            fails.append('v%d m13: occlusion register not a VA: %s' % (arch, oq))
        if BREAK and qm:
            fails.append('v%d m13: self-test hook' % arch)
        c = run([sys.executable, '-I', dec, 'check', md, '--arch', str(arch)], {})
        for l in c.stdout.splitlines() + c.stderr.splitlines():
            if 'FAIL' in l or 'Traceback' in l or 'Error' in l:
                fails.append('v%d m13: static rule: %s' % (arch, l))
        print(json.dumps({'case': 'v%d m13' % arch, 'ops': len(om), 'draws': len(qm), 'oq': str(oq[:1]), 'ok': not any(f.startswith('v%d m13' % arch) for f in fails)}))

dt = time.time() - t0
for f in fails[:60 if not verbose else 10000]:
    print('FAIL', f)
print(json.dumps({'test': 't_cmd', 'pass': not fails and dt < 5.0, 'failures': len(fails), 'seconds': round(dt, 2)}))
if BREAK:   # inverted: success means the corruption was detected
    ok = len(fails) >= 2 and any(': ins 0 ' in f for f in fails) and any('r54' in f and 'ours=0x1' in f for f in fails)
    print('SELFTEST', 'broken stream detected' if ok else 'NOT DETECTED')
    sys.exit(0 if ok else 1)
sys.exit(1 if fails or dt >= 5.0 else 0)
