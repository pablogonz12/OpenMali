#!/usr/bin/env python3 -I
# SPDX-License-Identifier: MIT
"""Shell-user route of the phone farm (devices.toml: route = "shell"): smoke and dEQP daily on a phone where only `adb shell` works (no guest).
Called by farmd under the phone lock (QSAFE_HELD=<lock>); the transport comes from ANDROID_SERIAL.
  shellroute.py smoke --lock L --tree T --sha S --run ID [--env "A=1 B=2"] [--loop 20]    bionic t_vk_draw/compute/meta, JSON lines like valguest.sh
  shellroute.py daily --lock L --tree T --sha S --run ID --list F --out TSV [--chunk 100] [--wall 250] [--env ..]
Integrity: the job's .so (S = its sha256) goes to a per-run path on the phone and is verified ON THE PHONE before every chunk/test.
Exit: 0 done, 73 PHONE BUSY (lock not held), 75 gated by the health gate / owner (resume later), 76/77 .so on the phone is not the job's build
(daily/smoke), 78 phone not staged, 79 phone unreachable. The daily resumes from the TSV (case<TAB>status<TAB>, same as qdeqp)."""
import argparse, json, glob, hashlib, os, re, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import farmlib as L  # noqa: E402
import gate  # noqa: E402

G = "/data/local/tmp/g"                    # staged once (DEVICE-<phone>.md): sysroot libs, ld.so, shim, deqp-vk, vulkan/ data
V = "/data/local/tmp/farm"                 # per-run smoke dirs
ONLY_DRAW = "triangle two depth rot idx16 idx32 scissor viewport multivp clearatt ring pushfs ubo descupd texture".split()
ONLY_META = "buf fill img copyimg blit clearc cleard resolve event ts".split()
PASSLIKE = {"Pass", "QualityWarning", "CompatibilityWarning"}
SERIAL = os.environ.get("ANDROID_SERIAL", "")


def adb(*a, timeout=60):
    try:
        p = subprocess.run(["adb", "-s", SERIAL, *a], capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        return 124, "timeout"


def require_lock(lock):
    import fcntl
    fd = open(f"/tmp/qsafe-{lock}.lock", "a")
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return fd                                              # we took it (hand-run); keep it until exit
    except OSError:
        fd.close()
        if os.environ.get("QSAFE_HELD") == lock:
            return None                                        # held by our parent (farmd)
        print(f"PHONE BUSY: /tmp/qsafe-{lock}.lock is held by another process; refusing to touch the phone", file=sys.stderr)
        sys.exit(73)


def cpu_busy_pct(text):
    """two `cpu ...` lines of /proc/stat (1 s apart) -> busy % over the interval, None if unparsable"""
    rows = [l.split() for l in text.splitlines() if l.startswith("cpu ")]
    try:
        a, b = ([int(x) for x in r[1:9]] for r in rows[:2])
        tot, idle = sum(b) - sum(a), (b[3] + b[4]) - (a[3] + a[4])
        return None if tot <= 0 else 100.0 * (tot - idle) / tot
    except (ValueError, IndexError):
        return None


def gated(lock):
    """True = stop (health gate or owner switch). Same single-source gate as the rest of the farm."""
    if gate.disabled(lock):
        print("PHONE DISABLED by owner", flush=True)
        return True
    st, why = gate.check(lock, gate.read_adb(SERIAL), gate.thresholds(os.environ.get("SHELLROUTE_SERIAL")))
    if st != "ok":
        print(f"gated ({st}): {why}", flush=True)
    if st == "ok":                                      # another workload (game/benchmark app) on the GPU makes dEQP crash: wait for it
        la = adb("shell", "cat /proc/loadavg", timeout=20)[1].split()
        try:
            if float(la[0]) > float(os.environ.get("SHELLROUTE_MAX_LOAD", "8")):
                # a high load average alone is not proof: HyperOS (poco-x4) idles at load 12 with 500 % CPU idle (D-state threads)
                busy = cpu_busy_pct(adb("shell", "head -1 /proc/stat; sleep 1; head -1 /proc/stat", timeout=20)[1])
                if busy is None or busy >= float(os.environ.get("SHELLROUTE_MAX_CPU", "50")):
                    print(f"gated (busy): phone load average {la[0]} (another app is using the CPU/GPU)", flush=True)
                    return True
        except (ValueError, IndexError):
            pass
    return st != "ok"


DIM = f"{V}/dim.state"                      # phone side: original brightness/mode/wakefulness while dimmed (so a killed run can still be undone)


def dim():
    """runs that do not need the display (fbo, hidden): minimum brightness + screen off, original values kept on the phone. Idempotent."""
    if "dim.state" in adb("shell", f"ls {V} 2>/dev/null", timeout=20)[1]:
        return
    b = adb("shell", "settings get system screen_brightness; settings get system screen_brightness_mode; dumpsys power | grep -m1 -o 'mWakefulness=[A-Za-z]*'", timeout=30)[1].split()
    if len(b) < 3:
        return
    adb("shell", f"mkdir -p {V}; echo '{b[0]} {b[1]} {b[2].split('=')[-1]}' > {DIM}", timeout=20)
    adb("shell", "settings put system screen_brightness_mode 0; settings put system screen_brightness 1" + ("; input keyevent 223" if b[2].endswith("Awake") else ""), timeout=30)


def undim():
    o = adb("shell", f"cat {DIM} 2>/dev/null", timeout=20)[1].split()
    if len(o) < 3:
        return
    adb("shell", f"settings put system screen_brightness {o[0]}; settings put system screen_brightness_mode {o[1]}" + ("; input keyevent 224" if o[2] == "Awake" else "") + f"; rm -f {DIM}", timeout=30)


def temps(run, n):
    """per-chunk temperature line (battery + the hottest gpu/cpu thermal zones) -> farm state logs/temps-<lock>.log, so heating can be traced to a chunk"""
    o = adb("shell", "dumpsys battery | grep -m1 temperature; for z in /sys/class/thermal/thermal_zone*; do echo $(cat $z/type) $(cat $z/temp); done", timeout=30)[1].splitlines()
    vals = {"batt": None}
    for l in o:
        p = l.split()
        if l.strip().startswith("temperature") and len(p) > 1 and p[-1].isdigit():
            vals["batt"] = int(p[-1]) / 10
        elif len(p) == 2 and p[1].lstrip("-").isdigit():
            k = "gpu" if "gpu" in p[0] else "cpu" if re.search(r"cpu|big|mid|little|soc", p[0]) else None
            v = int(p[1]) / (1000 if abs(int(p[1])) > 1000 else 1)
            if k and v > (vals.get(k) or 0):
                vals[k] = v
    line = f"{L.now()} {run} chunk{n} " + " ".join(f"{k}={v}" for k, v in vals.items())
    print("TEMP " + line, flush=True)
    try:
        with open(os.path.join(L.state_dir(), "logs", f"temps-{os.environ.get('SHELLROUTE_LOCK', 'phone')}.log"), "a") as f:
            f.write(line + "\n")
    except OSError:
        pass


def sha_local(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()


def sha_phone(path):
    rc, o = adb("shell", f"sha256sum {path} 2>/dev/null", timeout=40)
    f = o.split()
    return f[0] if rc == 0 and f else None


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


def parse_notes(text):
    """QPA -> {case: first line of the <Result> text} for every case that did not pass (dEQP's own failure message, e.g. "Image comparison failed")"""
    out, begun = {}, None
    for m in re.finditer(r"#beginTestCaseResult (\S+)|<Result StatusCode=\"(\w+)\">(.*?)</Result>", text, re.S):
        if m.group(1):
            begun = m.group(1)
        elif begun and m.group(2) not in PASSLIKE:
            out[begun] = " ".join(m.group(3).split())[:200]
    return out


def push_so(src, sha, dst):
    rc, o = adb("push", src, dst, timeout=180)
    if rc != 0:
        print("push failed: " + o.strip()[-200:], flush=True)
        sys.exit(79)
    return sha_phone(dst) == sha


def daily(a):
    if "ld-linux-aarch64" not in adb("shell", f"ls {G}/lib", timeout=30)[1] or "deqp-vk" not in adb("shell", f"ls {G}", timeout=30)[1]:
        print(f"phone not staged: {G} needs lib/ld-linux-aarch64.so.1 + deqp-vk (see its DEVICE-*.md, 'Stage (once)')", flush=True)
        sys.exit(78)
    icd = f"{G}/icd.{a.run}.so"                                # per-run ICD path: concurrent runs cannot swap each other's driver
    so = os.path.join(a.tree, "vk", "out", "a64-glibc", "libvulkan_mali.so")
    if not push_so(so, a.sha, icd):
        print("SO MISMATCH after push", flush=True)
        sys.exit(76)
    cases = [l.strip() for l in open(a.list) if l.strip()]
    done = {}
    if os.path.exists(a.out):
        for l in open(a.out):
            p = l.rstrip("\n").split("\t")
            if len(p) >= 2:
                done[p[0]] = p[1]
    f = open(a.out, "a")
    envs = " ".join(a.env.split())
    t0, fails = time.time(), 0
    tmp = tempfile.mkdtemp(prefix="shellroute-")
    while True:
        todo = [c for c in cases if c not in done]
        if not todo:
            break
        if gated(a.lock):
            sys.exit(75)
        if sha_phone(icd) != a.sha:                            # before every chunk, ON THE PHONE
            print("SO MISMATCH on the phone", flush=True)
            sys.exit(76)
        batch = todo[:a.chunk]
        cf, qf = f"{G}/c.{a.run}.cases", f"{G}/c.{a.run}.qpa"
        open(os.path.join(tmp, "c.cases"), "w").write("\n".join(batch) + "\n")
        if adb("push", os.path.join(tmp, "c.cases"), cf)[0] != 0:
            fails += 1
            if fails > 2:
                sys.exit(79)
            time.sleep(10)
            continue
        cmd = (f"cd {G} && rm -f {qf} && {envs} MALI_ICD={icd} timeout {a.wall} lib/ld-linux-aarch64.so.1 --library-path {G}/lib "
               f"./deqp-vk --deqp-caselist-file={cf} --deqp-log-filename={qf} --deqp-log-images=disable --deqp-log-shader-sources=disable "
               "--deqp-surface-type=fbo --deqp-gl-config-name=rgba8888d24s8 --deqp-visibility=hidden --deqp-shadercache=disable "
               f"--deqp-watchdog=enable --deqp-log-flush=enable >c.{a.run}.out 2>&1; echo rc=$?")
        rc, o = adb("shell", cmd, timeout=a.wall + 40)
        if rc == 124:
            adb("shell", "pkill -f deqp-vk", timeout=15)
        pr = os.path.join(tmp, "c.qpa")
        if os.path.exists(pr):
            os.unlink(pr)
        adb("pull", qf, pr, timeout=90)
        text = open(pr, errors="replace").read() if os.path.exists(pr) else ""
        if rc not in (0, 124) or ("rc=" not in o and rc != 124):        # adb itself failed: not the cases' fault
            fails += 1
            if fails > 2:
                print("phone unreachable: " + o.strip()[-120:], flush=True)
                sys.exit(79)
            time.sleep(15)
            continue
        fails = 0
        res, unfinished = parse_qpa(text)
        notes = parse_notes(text)
        if notes:                                              # dEQP failure messages: the tsv has no room, keep them next to the farm logs
            with open(os.path.join(L.state_dir(), "logs", f"{a.run}-notes.tsv"), "a") as nf:
                nf.writelines(f"{c}\t{n}\n" for c, n in notes.items() if c in batch)
        got = {}
        for c, s in res:
            if c in batch and c not in got:
                got[c] = norm(s)
        if unfinished and unfinished in batch and unfinished not in got:
            got[unfinished] = "Crash"                          # died or timed out inside this case
        if not got:
            tail = adb("shell", f"tail -c 400 {G}/c.{a.run}.out", timeout=20)[1].strip().replace("\n", " | ")
            print(f"NOPARSE {batch[0]} rc-line={o.strip()[-30:]!r} out: {tail}", flush=True)       # diagnostics for the farm log
            got[batch[0]] = "Crash"                            # nothing parsed: first case counts as Crash so we always progress
        for c in batch:
            if c in got:
                done[c] = got[c]
                f.write(f"{c}\t{got[c]}\t\n")
        f.flush()
        print(f"# chunk: done {len(done)}/{len(cases)} [{int(time.time() - t0)}s]", flush=True)
        temps(a.run, len(done))
    adb("shell", f"rm -f {G}/c.{a.run}.cases {G}/c.{a.run}.qpa {G}/c.{a.run}.out", timeout=20)
    print(f"SO_ON_PHONE {icd}", flush=True)                    # farmd verifies it once more, then removes it
    print("DONE", flush=True)


def smoke(a):
    d = f"{V}/{a.run}"
    adb("shell", f"rm -rf {d}; mkdir -p {d}/s", timeout=30)
    out = os.path.join(a.tree, "vk", "out", "a64-bionic")
    if not push_so(os.path.join(out, "libvulkan_mali.so"), a.sha, f"{d}/libvulkan_mali.so"):
        print("SO MISMATCH after push", flush=True)
        sys.exit(77)
    for t in ("t_vk_draw", "t_vk_compute", "t_vk_meta"):
        if adb("push", os.path.join(out, t), f"{d}/{t}")[0] != 0:
            sys.exit(79)
    adb("shell", f"chmod +x {d}/t_vk_*", timeout=20)
    for sp in glob.glob(os.path.join(a.tree, "vk", "shader", "*.spv")):
        adb("push", sp, f"{d}/s/{os.path.basename(sp)}", timeout=60)
    envs = " ".join(a.env.split())
    ok, tmp = True, tempfile.mkdtemp(prefix="shellroute-")
    runs = []
    for n, extra in (("t_vk_draw", f"--loop {a.loop}"), ("t_vk_compute", "--loop 5"), ("t_vk_meta", "")):
        if not a.only:
            runs.append((n, extra, ""))
            continue
        for c in a.only.split(","):                                  # --only: one run per case this binary knows, binaries without a match are skipped
            if c in (ONLY_DRAW if n == "t_vk_draw" else ONLY_META):
                runs.append((n, f"{extra} --only {c}", c))
    for n, extra, oc in runs:
        if gated(a.lock):
            sys.exit(75)
        if sha_phone(f"{d}/libvulkan_mali.so") != a.sha:
            print("SO MISMATCH on the phone", flush=True)
            sys.exit(77)
        sp = "" if n == "t_vk_meta" else f"--spv-dir {d}/s"
        rc, o = adb("shell", f"cd {d} && {envs} timeout 120 ./{n} --direct {d}/libvulkan_mali.so {sp} {extra} >{n}.log 2>&1; echo rc=$?", timeout=170)
        m = re.search(r"rc=(\d+)", o)
        if not m:
            print("phone unreachable: " + o.strip()[-120:], flush=True)
            sys.exit(79)
        lp = os.path.join(tmp, n + ".log")
        adb("pull", f"{d}/{n}.log", lp, timeout=60)
        log = open(lp, errors="replace").read() if os.path.exists(lp) else ""
        fl = [x for x in log.splitlines() if '"fail"' in x]
        for x in log.splitlines()[-200:]:                            # kept by farmd (farm log)
            print(json.dumps({"log": n + (":" + oc if oc else ""), "line": x[:400]}), flush=True)
        r = int(m.group(1))
        ok = ok and r == 0
        print('{"test":"%s","rc":%d,"fail_lines":%d,"pass_lines":%d}' % (n, r, len(fl), log.count('"pass":true')), flush=True)
    print('{"test":"summary","ok":%s}' % ("true" if ok else "false"), flush=True)
    print(f"SO_ON_PHONE {d}/libvulkan_mali.so", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=("smoke", "daily", "undim"))
    ap.add_argument("--lock", required=True)
    ap.add_argument("--tree", default="")
    ap.add_argument("--sha", default="")
    ap.add_argument("--run", default="undim")
    ap.add_argument("--list")
    ap.add_argument("--only", default="")
    ap.add_argument("--out")
    ap.add_argument("--chunk", type=int, default=100)
    ap.add_argument("--wall", type=int, default=250)
    ap.add_argument("--loop", type=int, default=20)
    ap.add_argument("--env", default="")
    a = ap.parse_args()
    fd = require_lock(a.lock)                                  # noqa: F841 (kept open until exit)
    if not SERIAL:
        print("ANDROID_SERIAL not set", file=sys.stderr)
        sys.exit(79)
    os.environ["SHELLROUTE_LOCK"] = a.lock
    if a.mode == "undim":
        undim()
        return
    dim()
    try:
        {"smoke": smoke, "daily": daily}[a.mode](a)
    finally:
        if not os.environ.get("SHELLROUTE_KEEP_DIM"):            # the idle sweep keeps the screen dark between chunks; farmd undims when it stops
            undim()


if __name__ == "__main__":
    main()
