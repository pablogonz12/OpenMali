# SPDX-License-Identifier: MIT
"""Command-stream check of MALI_PRELOAD_INTERSECT on the fake GPUs.   t_preload_stream.py OUT_DIR SPV_DIR [GPU...]

Runs t_vk_draw cases with the switch unset / 0 / 1 (MALI_FAKE_DUMP), renders the dumps with golden.py and checks:
  (a) load_colour_argb8 (colour LOAD + STORE, no depth): with the switch on, the LOAD frame's FBD runs pre-frame shader 0 in
      mode INTERSECT (2) from a Draw descriptor whose pointers all lie in live BOs (golden.py R-VA), Clean Tile Write is off on
      the RT, and the frame has one RUN_IDVS less (the preload draw is gone);
  (b) the mandatory-ALWAYS cases (load_depth / load_stencil: colour + depth/stencil LOAD; msaa_load_resolve: 4x + resolve)
      stay ALWAYS: the stream with the switch on is identical to the switch off, and the counters name the reason;
  (c) switch 0 renders the same stream as the switch unset (the goldens cover the default build)."""
import os, re, subprocess, sys

OUT, SPV = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
W = os.path.join(OUT, 'preload_stream'); os.makedirs(W, exist_ok=True)
fails = 0


def check(ok, msg):
    global fails
    print(('ok    ' if ok else 'FAIL  ') + msg)
    if not ok: fails += 1


def run(gpu, case, sw, extra=None):
    tag = '%s-%s-%s' % (gpu, case, sw)
    js, txt = os.path.join(W, tag + '.jsonl'), os.path.join(W, tag + '.txt')
    for x in (js, txt):   # the fake backend appends to an existing dump
        if os.path.exists(x): os.remove(x)
    env = dict(os.environ, MALI_BACKEND='fake', MALI_FAKE_GPU=gpu, MALI_FAKE_DUMP=js, MALI_PRELOAD_STATS='1')
    env.pop('MALI_PRELOAD_INTERSECT', None)
    if sw != 'unset': env['MALI_PRELOAD_INTERSECT'] = sw
    env.update(extra or {})
    p = subprocess.run(['timeout', '120', os.path.join(OUT, 't_vk_draw'), '--direct', os.path.join(OUT, 'libvulkan_mali.so'),
                        '--spv-dir', SPV, '--only', case], env=env, capture_output=True, text=True)
    check(p.returncode == 0 and '"pass":true' in p.stdout, '%s: t_vk_draw exit %d' % (tag, p.returncode))
    g = subprocess.run(['timeout', '120', sys.executable, '-I', os.path.join(HERE, 'golden.py'), js, txt],
                       env=dict(os.environ, GOLDEN_ARCH='13' if gpu == 'v13' else '11'), capture_output=True, text=True)
    check(g.returncode == 0 and ' 0 violations' in g.stdout + g.stderr, '%s: golden.py rules (%s)' % (tag, (g.stdout + g.stderr).strip().splitlines()[-1][-60:] if (g.stdout + g.stderr).strip() else "-"))
    st = re.search(r'mali_preload: switch=\w+ (.*)', p.stderr)
    stats = dict((k, int(v)) for k, v in re.findall(r'(\w+)=(\d+)', st.group(1))) if st else {}
    return open(txt).read() if os.path.exists(txt) else '', stats


for gpu in sys.argv[3:] or ('v10', 'v11', 'v13'):
    off, s_off = run(gpu, 'load_colour_argb8', '0')
    uns, _ = run(gpu, 'load_colour_argb8', 'unset')
    on, s_on = run(gpu, 'load_colour_argb8', '1')
    check(off == uns and off, '%s (c) switch 0 == unset (byte-identical stream)' % gpu)
    check('frame shaders' not in off, '%s (c) switch off: no frame shader in any FBD' % gpu)
    fs = re.findall(r'frame shaders pre0=(\d) pre1=(\d) post=(\d) DCDs = (\S+)', on)
    check(len(fs) == 1 and fs[0][:3] == ('2', '0', '0') and fs[0][3].startswith('BO'),
          '%s (a) one LOAD frame with pre-frame 0 = INTERSECT, DCDs in a BO: %s' % (gpu, fs))
    n_off, n_on = len(re.findall(r'^ +\d+ RUN_IDVS2? ', off, re.M)), len(re.findall(r'^ +\d+ RUN_IDVS2? ', on, re.M))
    check(n_on == n_off - 1, '%s (a) one RUN_IDVS less: the preload draw is gone (%d -> %d)' % (gpu, n_off, n_on))
    m = re.search(r'frame shaders .*\n    DCD0\n((?:      .*\n){4})', on)
    dcd = m.group(1).split('\n') if m else []
    check(len(dcd) >= 4 and len(re.findall(r'BO\d+', dcd[3])) == 4,
          '%s (a) DCD0 words 24..31 = resources, shader, thread storage, FAU in live BOs: %s' % (gpu, dcd[3].strip() if len(dcd) > 3 else '-'))
    fr = on[on.index('frame shaders') - 1200:on.index('frame shaders')] if fs else ''
    rtd = re.findall(r'    RTD\n      (\S+) (\S+)', fr)
    check(bool(rtd) and not (int(rtd[-1][1], 16) >> 31), '%s (a) RT Clean Tile Write Enable off: %s' % (gpu, rtd[-1] if rtd else '-'))
    check(s_on.get('intersect') == 1 and s_off.get('intersect') == 1, '%s counters: intersect=1 with the switch on and off (%s)' % (gpu, s_on))
    for case, extra, reason in (('load_depth', None, 'always_zs_load'), ('load_stencil', None, 'always_zs_load'),
                                ('msaa_load_resolve', {'MALI_EXPERIMENTAL_MSAA': '1'}, None)):
        a, sa = run(gpu, case, '0', extra)
        b, sb = run(gpu, case, '1', extra)
        check(a == b and a, '%s (b) %s: switch on == off (stays ALWAYS, byte-identical)' % (gpu, case))
        always = sum(v for k, v in sb.items() if k.startswith('always_'))
        check(sb.get('intersect', 0) == 0 and always >= 1 and (reason is None or sb.get(reason, 0) >= 1),
              '%s (b) %s counters: %s' % (gpu, case, sb))

print('FAILED (%d)' % fails if fails else 'OK')
sys.exit(1 if fails else 0)
