"""Rule self-tests: take a good dump written by the fake backend, damage it the way the real bug did, and require that exactly the
named rule fires (and that the undamaged dump is clean). A rule that cannot be made to fire does not belong in check.sh.
usage: selftest.py <vk_draw-v11.jsonl> <vk_draw-v13.jsonl> <scratch dir>"""
import os, struct, sys
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import golden as g

def qwords(e): return list(struct.unpack('<%dQ' % (len(e['hex']) // 16), bytes.fromhex(e['hex'])))
def put(e, q): e['hex'] = struct.pack('<%dQ' % len(q), *q).hex()
def moves(q, reg): return [i for i, w in enumerate(q) if w >> 56 == 1 and (w >> 48) & 0xff == reg]

def m_va(d, e):          # SRT pointer -> inside the fake VA window but in no BO
    if e.get('done'): return
    q = qwords(e); mv = moves(q, 0)
    if mv: q[mv[0]] = (q[mv[0]] & ~((1 << 48) - 1)) | 0x5fffff0000; put(e, q); e['done'] = 1
def m_set(d, e):         # the driver descriptor set shrinks to one entry (794a571)
    q = qwords(e)
    for i in moves(q, 0):
        va = q[i] & ((1 << 48) - 1) & ~15
        if d.mem.read(va, 16) != bytes(16):
            b = bytearray(d.mem.read(va & ~4095, 4096)); o = va & 4095
            struct.pack_into('<I', b, o + 8, 32); d.mem.add(va & ~4095, bytes(b)); return
def m_heap(d, e):        # the heap descriptor page is rewritten every frame (c66eb3e)
    q = qwords(e)
    for i in moves(q, 40):
        t = q[i] & ((1 << 48) - 1) & ~63
        c = d.mem.read(t, 128); h = (struct.unpack_from('<I', c, 24)[0] | struct.unpack_from('<I', c, 28)[0] << 32) & ((1 << 48) - 1)
        if h:
            b = bytearray(d.mem.read(h & ~4095, 4096)); b[16] ^= 0xff; d.mem.add(h & ~4095, bytes(b)); return
def m_cs(d, e):          # v11 RUN_IDVS without the fragment SRT select bit (38) / v13 RUN_IDVS2 without malloc_enable (33)
    if e.get('done'): return
    q = qwords(e)
    for i, w in enumerate(q):
        if w >> 56 in (6, 12): q[i] = w & ~(1 << (38 if w >> 56 == 6 else 33)); put(e, q); e['done'] = 1; return

def run(path, arch, hook, tmp):
    os.environ['GOLDEN_ARCH'] = str(arch)
    return g.process(path, os.path.join(tmp, 'st.txt'), hook)[1]

def head(path, tmp, nrings=3):   # the first kicks are enough (the full dumps are rule-checked by the goldens themselves)
    o = os.path.join(tmp, 'st-' + os.path.basename(path)); n = 0
    with open(path) as f, open(o, 'w') as w:
        for l in f:
            w.write(l)
            if '"ev":"ring"' in l:
                n += 1
                if n == nrings: break
    return o

if __name__ == '__main__':
    v11, v13, tmp = sys.argv[1:4]
    v11, v13 = head(v11, tmp), head(v13, tmp)
    bad = 0
    for name, path, arch, hook in (('R-VA', v11, 11, m_va), ('R-VA', v13, 13, m_va), ('R-SET', v11, 11, m_set), ('R-SET', v13, 13, m_set),
                                   ('R-HEAP', v11, 11, m_heap), ('R-HEAP', v13, 13, m_heap), ('R-CS', v11, 11, m_cs), ('R-CS', v13, 13, m_cs)):
        clean = run(path, arch, None, tmp)
        if clean: print('FAIL  %s: undamaged dump already violates: %s' % (name, clean[0])); bad = 1; continue
        v = run(path, arch, hook, tmp)
        if any(name in x for x in v): print('ok    rule self-test %s fires on v%d (%s)' % (name, arch, v[0][:90]))
        else: print('FAIL  rule self-test %s did NOT fire on v%d (got %s)' % (name, arch, v[:2])); bad = 1
    sys.exit(bad)
