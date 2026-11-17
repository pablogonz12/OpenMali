#!/usr/bin/env python3 -I
# SPDX-License-Identifier: MIT
"""Shell-user dEQP route (no MaliDeck guest): glibc deqp-vk run from /data/local/tmp/g with the sysroot's ld.so, our glibc ICD
reached through a 20-line libvulkan.so.1 shim (vkshim.c: vkGetInstanceProcAddr -> vk_icdGetInstanceProcAddr of $MALI_ICD).
Usage: shell-deqp.py <alias> <case-list> <out.tsv> [--chunk 100] [--wall 250]
Every phone access goes through quick/qsafe (per-phone lock + health gate; exit 126 = gated, we wait). Resumes from <out.tsv>.
Output format = qdeqp's tsv (case<TAB>status<TAB>), so it compares directly with ours-daily-sample-*.tsv."""
import os, re, subprocess, sys, time

PASSLIKE = {"Pass", "QualityWarning", "CompatibilityWarning"}
G = "/data/local/tmp/g"
QSAFE = os.path.expanduser("~/Projects/mali-gaming/scripts/quick/qsafe")
ENV = dict(os.environ, PATH=os.path.expanduser("~/.local/bin") + os.pathsep + os.environ["PATH"])


def norm(s):
    return "Pass" if s in PASSLIKE else s if s in ("Fail", "NotSupported", "Crash", "ResourceHog") else "Other:" + s


def parse_qpa(text):
    res, cur, begun = [], None, None
    for m in re.finditer(r"#beginTestCaseResult (\S+)|#endTestCaseResult|<Result StatusCode=\"(\w+)\"", text):
        if m.group(1):
            begun, cur = m.group(1), None
        elif m.group(2):
            cur = m.group(2)
        else:
            if begun and cur:
                res.append((begun, cur))
            begun, cur = None, None
    return res, begun


def serial(alias):
    # live adb transport (wireless phones are listed under an mDNS name), resolved like qsafe does
    q = os.path.expanduser("~/Projects/mali-gaming/scripts/quick/quick.py")
    return subprocess.run(["python3", "-I", q, "serial", alias], capture_output=True, text=True, env=ENV).stdout.strip()


def run_chunk(alias, ser, cases, wall):
    open("/tmp/x4g/c.cases", "w").write("\n".join(cases) + "\n")
    lock = f"/tmp/qsafe-{alias}.lock"
    subprocess.run(["flock", "-w", "120", lock, "adb", "-s", ser, "push", "/tmp/x4g/c.cases", f"{G}/c.cases"], env=ENV, capture_output=True)
    cmd = (f"cd {G} && rm -f c.qpa && MALI_ICD={G}/lib/libvulkan_mali.so timeout {wall} lib/ld-linux-aarch64.so.1 --library-path {G}/lib "
           f"./deqp-vk --deqp-caselist-file={G}/c.cases --deqp-log-filename={G}/c.qpa --deqp-log-images=disable --deqp-log-shader-sources=disable "
           "--deqp-surface-type=fbo --deqp-gl-config-name=rgba8888d24s8 --deqp-visibility=hidden --deqp-shadercache=disable "
           "--deqp-watchdog=enable --deqp-log-flush=enable >c.out 2>&1; echo rc=$?")
    while True:
        r = subprocess.run([QSAFE, alias, str(wall + 30), cmd], env=ENV, capture_output=True, text=True)
        if r.returncode == 126:           # health-gated: wait
            print("gated, waiting", flush=True); time.sleep(120); continue
        if r.returncode == 125:           # phone busy (farm): retry
            time.sleep(30); continue
        break
    subprocess.run(["flock", "-w", "120", lock, "adb", "-s", ser, "pull", f"{G}/c.qpa", "/tmp/x4g/c.qpa"], env=ENV, capture_output=True)
    try:
        return parse_qpa(open("/tmp/x4g/c.qpa", errors="replace").read())
    except OSError:
        return [], None


def main():
    alias, lst, out = sys.argv[1:4]
    chunk = int(sys.argv[sys.argv.index("--chunk") + 1]) if "--chunk" in sys.argv else 100
    wall = int(sys.argv[sys.argv.index("--wall") + 1]) if "--wall" in sys.argv else 250
    ser = serial(alias)
    cases = [l.strip() for l in open(lst) if l.strip()]
    done = {}
    if os.path.exists(out):
        for l in open(out):
            p = l.rstrip("\n").split("\t")
            done[p[0]] = p[1]
    f = open(out, "a")
    t0 = time.time()
    while True:
        todo = [c for c in cases if c not in done]
        if not todo:
            break
        batch = todo[:chunk]
        res, unfinished = run_chunk(alias, ser, batch, wall)
        got = {}
        for c, s in res:
            if c in batch and c not in got:
                got[c] = norm(s)
        if unfinished and unfinished in batch and unfinished not in got:
            got[unfinished] = "Crash"          # died or timed out inside this case
        if not got:                            # nothing parsed: first case counts as Crash so we always progress
            got[batch[0]] = "Crash"
        for c in batch:
            if c in got:
                done[c] = got[c]; f.write(f"{c}\t{got[c]}\t\n")
        f.flush()
        print(f"{len(done)}/{len(cases)} t={int(time.time()-t0)}s", flush=True)
    cnt = {}
    for s in done.values():
        cnt[s] = cnt.get(s, 0) + 1
    print("DONE", cnt, flush=True)


main()
