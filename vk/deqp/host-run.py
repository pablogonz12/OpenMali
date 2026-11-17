#!/usr/bin/env python3 -I
# SPDX-License-Identifier: MIT
"""Host x86-64 dEQP-VK against the native fake-backend ICD (ASan+UBSan driver), both fake GPUs.
Only three outcomes are compared with the stored baseline: Crash, Timeout, NotSupported (everything else is "Other": the fake GPU draws
nothing, so Pass/Fail carry no information). Driven by host-run.sh; see there for usage."""
import argparse, os, re, signal, subprocess, sys, time

H = os.path.dirname(os.path.abspath(__file__))


def parse_qpa(path):
    """-> ({case: status}, in_flight_case_or_None)"""
    done, cur = {}, None
    try:
        text = open(path, errors='replace').read()
    except OSError:
        return done, None
    for m in re.finditer(r'#beginTestCaseResult (\S+)|#endTestCaseResult|<Result StatusCode="(\w+)"', text):
        if m.group(1): cur, st = m.group(1), None
        elif m.group(2): st = m.group(2); res = st
        else:
            if cur: done[cur] = res if st else 'Crash'
            cur = None
    return done, cur


def classify(st):
    return st if st in ('NotSupported', 'Crash', 'Timeout') else 'Other'


def signature(err, rc, timed_out):
    if timed_out: return 'timeout'
    m = re.search(r'ERROR: (?:Address|Undefined|Leak)Sanitizer: ([\w-]+)', err)
    if m:
        fr = re.search(r'#\d+ 0x[0-9a-f]+ in (\S+) [^\n]*libvulkan_mali', err) or re.search(r'#\d+ 0x[0-9a-f]+ in (\S+) ', err)
        return 'asan:%s in %s' % (m.group(1), fr.group(1) if fr else '?')
    m = re.search(r'runtime error: ([^\n]{0,90})', err)
    if m: return 'ubsan:' + m.group(1).strip()
    m = re.search(r'(?:Assertion|assertion) [`\'"]?([^\n]{0,100})', err)
    if m: return 'assert:' + m.group(1)
    if rc is not None and rc < 0:
        try: return 'signal:' + signal.Signals(-rc).name
        except ValueError: return 'signal:%d' % -rc
    return 'exit:%s' % rc


def run_gpu(gpu, cases, a, out):
    res_path = os.path.join(out, 'results-%s.tsv' % gpu)
    results = {}
    if os.path.exists(res_path) and not a.fresh:
        for l in open(res_path):
            p = l.rstrip('\n').split('\t')
            if len(p) >= 2: results[p[0]] = (p[1], p[2] if len(p) > 2 else '')
    elif os.path.exists(res_path): os.remove(res_path)
    rf = open(res_path, 'a')

    def rec(c, st, sig=''):
        results[c] = (st, sig); rf.write('%s\t%s\t%s\n' % (c, st, sig)); rf.flush()

    env = dict(os.environ, VK_DRIVER_FILES=a.icd, VK_ICD_FILENAMES=a.icd, MALI_BACKEND='fake', MALI_FAKE_GPU=gpu,
               LD_PRELOAD=a.asan, ASAN_OPTIONS='detect_leaks=0:handle_segv=1:fast_unwind_on_malloc=0:symbolize=1',
               UBSAN_OPTIONS='print_stacktrace=1')
    todo = [c for c in cases if c not in results]
    t0 = time.time()
    while todo:
        chunk = todo[:a.chunk]
        cl, qpa, errf = [os.path.join(out, 'chunk-%s.%s' % (gpu, x)) for x in ('cases', 'qpa', 'err')]
        open(cl, 'w').write('\n'.join(chunk) + '\n')
        if os.path.exists(qpa): os.remove(qpa)
        cmd = ['nice', '-n', '15', 'timeout', '-k', '10', str(a.timeout), a.deqp, '--deqp-caselist-file=' + cl, '--deqp-log-filename=' + qpa,
               '--deqp-log-images=disable', '--deqp-log-shader-sources=disable', '--deqp-log-flush=enable']
        with open(errf, 'w') as ef:
            rc = subprocess.call(cmd, cwd=os.path.dirname(a.deqp), env=env, stdout=subprocess.DEVNULL, stderr=ef, stdin=subprocess.DEVNULL)
        err = open(errf, errors='replace').read()
        done, inflight = parse_qpa(qpa)
        for c in chunk:
            if c in done and c != inflight: rec(c, classify(done[c]))
        # the case that was running when the process died / was killed
        bad = inflight if inflight in chunk else None
        if bad is None and len([c for c in chunk if c in results]) < len(chunk):
            bad = next(c for c in chunk if c not in results)   # died before writing anything for it
        if bad is not None:
            to = rc in (124, 137)
            rec(bad, 'Timeout' if to else 'Crash', signature(err, rc, to))
        todo = [c for c in cases if c not in results]
        n = len(cases) - len(todo)
        print('  %s: %d/%d  (%ds)%s' % (gpu, n, len(cases), time.time() - t0, '  %s %s' % (results[bad][0], bad) if bad else ''), flush=True)
    return {c: results[c] for c in cases}


def load_base(path):
    b = {}
    if os.path.exists(path):
        for l in open(path):
            if l.startswith('#') or not l.strip(): continue
            g, n, st, sig = (l.rstrip('\n').split('\t') + ['', '', '', ''])[:4]
            b[(g, n)] = (st, sig)
    return b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--deqp'); ap.add_argument('--icd'); ap.add_argument('--asan'); ap.add_argument('--list'); ap.add_argument('--out')
    ap.add_argument('--baseline'); ap.add_argument('--gpus', default='v11,v13'); ap.add_argument('--chunk', type=int, default=100)
    ap.add_argument('--timeout', type=int, default=300, help='seconds per batch of --chunk cases'); ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--bless', action='store_true'); ap.add_argument('--fresh', action='store_true')
    a = ap.parse_args()
    cases = [l.strip() for l in open(a.list) if l.strip().startswith('dEQP-VK.')]
    if a.limit: cases = cases[:a.limit]
    os.makedirs(a.out, exist_ok=True)
    cur = {}
    for g in a.gpus.split(','):
        print('== %s: %d cases' % (g, len(cases)), flush=True)
        for c, (st, sig) in run_gpu(g, cases, a, a.out).items():
            if st in ('Crash', 'Timeout', 'NotSupported'): cur[(g, c)] = (st, sig)
    # the current Crash/Timeout/NotSupported set, sorted
    new_path = os.path.join(a.out, 'current.tsv')
    with open(new_path, 'w') as f:
        for (g, c), (st, sig) in sorted(cur.items()): f.write('%s\t%s\t%s\t%s\n' % (g, c, st, sig))
    cnt = {}
    for (g, c), (st, sig) in cur.items(): cnt[(g, st)] = cnt.get((g, st), 0) + 1
    for g in a.gpus.split(','):
        print('%s: %d cases, Crash %d, Timeout %d, NotSupported %d' % (g, len(cases), cnt.get((g, 'Crash'), 0), cnt.get((g, 'Timeout'), 0), cnt.get((g, 'NotSupported'), 0)))
    sigs = {}
    for (g, c), (st, sig) in cur.items():
        if st in ('Crash', 'Timeout'): sigs[sig] = sigs.get(sig, 0) + 1
    for sig, n in sorted(sigs.items(), key=lambda x: -x[1])[:8]: print('  %4d  %s' % (n, sig))
    if a.bless:
        with open(a.baseline, 'w') as f:
            f.write('# host dEQP-VK vs fake backend: Crash/Timeout/NotSupported only. gpu, case, status, signature. ./host-run.sh --bless\n')
            for (g, c), (st, sig) in sorted(cur.items()): f.write('%s\t%s\t%s\t%s\n' % (g, c, st, sig))
        print('blessed %s (%d entries)' % (a.baseline, len(cur)))
        return 0
    base = load_base(a.baseline)
    if a.limit or a.gpus != 'v11,v13':   # partial run: only compare what was run
        ran = {(g, c) for g in a.gpus.split(',') for c in cases}
        base = {k: v for k, v in base.items() if k in ran}
    bad = 0
    for k in sorted(cur):
        st, sig = cur[k]; bs = base.get(k, ('', ''))[0]
        if st in ('Crash', 'Timeout') and bs not in ('Crash', 'Timeout'): print('NEW %s  %s %s  [%s]' % (st.upper(), k[0], k[1], sig)); bad = 1
        elif st == 'NotSupported' and bs != 'NotSupported': print('NEW NotSupported  %s %s' % k); bad = 1
    for k in sorted(base):
        if k not in cur: print('fixed (was %s)  %s %s' % (base[k][0], k[0], k[1]))
    print('host-run: %s' % ('NEW Crash/Timeout/NotSupported vs baseline' if bad else 'no new Crash/Timeout/NotSupported vs baseline'))
    return bad


sys.exit(main())
