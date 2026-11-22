#!/usr/bin/env python3 -I
# SPDX-License-Identifier: MIT
"""Shell-user route of the phone farm (devices.toml: route = "shell"): smoke and dEQP daily on a phone where only `adb shell` works (no guest).
Called by farmd under the phone lock (QSAFE_HELD=<lock>); the transport comes from ANDROID_SERIAL.
  shellroute.py smoke --lock L --tree T --sha S --run ID [--env "A=1 B=2"] [--loop 20]    bionic t_vk_draw/compute/meta, JSON lines like valguest.sh
  shellroute.py daily --lock L --tree T --sha S --run ID --list F --out TSV [--chunk 100] [--wall 250] [--env ..]
Integrity: the job's .so (S = its sha256) goes to a per-run path on the phone and is verified ON THE PHONE before every chunk/test.
Exit: 0 done, 73 PHONE BUSY (lock not held), 75 gated by the health gate / owner (resume later), 76/77 .so on the phone is not the job's build
(daily/smoke), 78 phone not staged, 79 phone unreachable, 81 crash bursts keep coming back (run unstable, discard). The daily resumes from the TSV (case<TAB>status<TAB>, same as qdeqp)."""
import argparse, json, glob, hashlib, os, re, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import farmlib as L  # noqa: E402
import json  # noqa: E402
import gate  # noqa: E402

G = "/data/local/tmp/g"                    # staged once (DEVICE-<phone>.md): sysroot libs, ld.so, shim, deqp-vk, vulkan/ data
V = "/data/local/tmp/farm"                 # per-run smoke dirs
ONLY_DRAW = "many_draws share triangle two depth rot idx16 idx32 scissor viewport multivp clearatt ring pushfs ubo descupd texture mrt2 mrt4".split()
ONLY_META = "buf fill img copyimg blit clearc cleard resolve event ts".split()
ONLY_FEAT = ["storfmt_nf", "ycbcr", "spec", "linerast", "divisor"]               # t_vk_feat cases of the smoke set (Steam-session features); linerast/divisor: pixel tests of the line-rasterization and vertex-divisor patches (skip until advertised); same list as FEAT in vk/tests/valguest.sh
PASSLIKE = {"Pass", "QualityWarning", "CompatibilityWarning"}
SERIAL = os.environ.get("ANDROID_SERIAL", "")

# --- resource guards (2026-11-19: a dEQP sweep on the CMF ran as uid shell with oom_score_adj=-1000, inherited from adbd; the kernel could
# not kill it and panicked with "Out of memory and no killable processes", the farm retried after the reboot and the phone looped for hours)
MEM_KB = int(os.environ.get("SHELLROUTE_MEM_KB", "3145728"))       # 3 GiB hard cap per deqp-vk launch: ulimit -v (or cgroup memory.max)
CHUNK_MAX = int(os.environ.get("SHELLROUTE_CHUNK_MAX", "40"))      # cases per deqp-vk launch: a shard cannot grow its heap without bound
BOOT_DRIFT = 5.0                                                   # s of uptime slack: /proc/uptime is read around the adb round trip
OOM_RE = re.compile(r"out of memory|Out of memory|bad_alloc|Cannot allocate memory|cannot allocate memory|memory exhausted")


def adb(*a, timeout=60):
    try:
        p = subprocess.run(["adb", "-s", SERIAL, *a], capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        return 124, "timeout"


def device_identity():
    """(boot id, uptime s) of the phone in ONE adb call; (None, None) when the phone cannot be read"""
    rc, o = adb("shell", "cat /proc/sys/kernel/random/boot_id; cat /proc/uptime", timeout=30)
    bid, up = None, None
    for l in o.splitlines():
        t = l.strip()
        if not t:
            continue
        if re.fullmatch(r"[0-9a-fA-F-]{32,40}", t):
            bid = t.lower()
        else:
            f = t.split()
            if len(f) >= 2:
                try:
                    up = float(f[0])
                except ValueError:
                    pass
    return bid, up


def load_identity(path):
    """(boot id, uptime) of the run this result file belongs to; (None, None) when there is no record"""
    try:
        with open(path) as fh:
            bid, up = fh.read().split()[:2]
        return (None if bid in ("", "-") else bid), (None if float(up) < 0 else float(up))
    except (OSError, ValueError):
        return None, None


def save_identity(path, ident):
    try:
        with open(path, "w") as fh:
            fh.write(f"{ident[0] or '-'} {ident[1] if ident[1] is not None else -1}\n")
    except OSError:
        pass


class BootWatch:
    """The boot a resumable run belongs to, persisted next to its results (so a resume in a later boot is not mistaken
    for the same run) and advanced on every successful observation, so an uptime drop is caught even when the boot id
    cannot be read (600 s -> 20 s is a reboot, not a slow run)."""

    def __init__(self, path):
        self.path = path
        self.id, self.up = load_identity(path)
        self.resumed = self.id is not None or self.up is not None
        if not self.resumed:                                   # a fresh run takes the boot now, before the first chunk
            self.id, self.up = device_identity()
            save_identity(self.path, (self.id, self.up))

    def check(self):
        bid, up = device_identity()
        if bid is None and up is None:
            return ""                                          # unreadable phone: the caller's reachability check decides
        why = reboot_reason((self.id, self.up), (bid, up))
        if why:
            return why
        if bid:
            self.id = bid
        if up is not None:
            self.up = up
        save_identity(self.path, (self.id, self.up))
        return ""


def reboot_reason(before, after):
    """'' when the phone is the same boot, else why it is not (a new boot id, or uptime that went backwards).
    Missing reads are not a reboot: a phone that cannot answer is handled by the caller's own reachability check."""
    if not before or not after:
        return ""
    b_id, b_up = before
    a_id, a_up = after
    if b_id and a_id and b_id != a_id:
        return f"boot id changed ({b_id[:8]} -> {a_id[:8]})"
    if b_up is not None and a_up is not None and a_up + BOOT_DRIFT < b_up:
        return f"uptime went backwards ({b_up:.0f}s -> {a_up:.0f}s)"
    return ""


def oom_kill_count(run):
    """cgroup v2 oom_kill counter of this run's cgroup, -1 when it cannot be read"""
    rc, o = adb("shell", f"sed -n 's/^oom_kill //p' /sys/fs/cgroup/mali-farm-{run}/memory.events 2>/dev/null", timeout=15)
    try:
        return int((o or "").strip())
    except ValueError:
        return -1


def limit_preamble(run, mem_kb=None):
    """device-side shell snippet, run in the SAME shell that forks deqp-vk, so the child inherits it:
      * oom_score_adj -> 0. adbd gives its shell children -1000 (unkillable); a batch process must never be OOM-protected.
      * cgroup v2 memory.max when the shell user can create one, else ulimit -v (address space). uid shell on the CMF/Poco cannot write
        /sys/fs/cgroup (root-owned, no CAP_SYS_ADMIN), so the ulimit branch is the bound that actually applies there; the cgroup branch
        is used where the phone allows it (su/root). 3 GiB default keeps headroom on an 8-12 GB phone; a 40-case shard needs far less.
    Prints one `LIMITS ...` line so the bound in force is visible in the farm log."""
    mem_kb = MEM_KB if mem_kb is None else int(mem_kb)
    cg = f"/sys/fs/cgroup/mali-farm-{run}"
    return (f"echo 0 > /proc/self/oom_score_adj 2>/dev/null; cg_ok=0; "
            f"if mkdir -p {cg} 2>/dev/null && echo $$ > {cg}/cgroup.procs 2>/dev/null "
            f"&& echo {mem_kb * 1024} > {cg}/memory.max 2>/dev/null "
            f"&& [ \"$(cat {cg}/memory.max 2>/dev/null)\" = {mem_kb * 1024} ]; then cg_ok=1; fi; "
            f"if [ $cg_ok = 1 ]; then echo LIMITS cgroup {cg} {mem_kb}KB; "
            f"elif ulimit -v {mem_kb} 2>/dev/null; then echo LIMITS ulimit-v {mem_kb}KB; "
            f"else echo LIMITS none; exit 84; fi")


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


def crash_note(qpa_text, out_tail):
    """one line for the notes tsv of a case that died or timed out: what dEQP printed last (stdout/stderr tail) and the last QPA bytes"""
    q = " ".join(qpa_text[-300:].split())
    o = " ".join(out_tail.split())[-400:]
    return f"CRASH out: {o} | qpa: {q}"


BURST_STREAK = 5              # this many chunks in a row that each ended in a crash = a burst, not five bad cases (a normal daily has 0-11 crashes in 1381)
BURST_MAX = 3                 # bursts tolerated per run; one more and the run is unstable (exit 81, farmd: invalid)


def BURST_PAUSE():
    return float(os.environ.get("SHELLROUTE_BURST_PAUSE", "90"))


class BurstGuard:
    """A case that crashes the first time is not written (not in the TSV, not in `done`): it is held back and runs once more. A clean chunk after
    it ends the streak (release(): the crashes were isolated, second run at once). `streak` consecutive crashing chunks is a burst (phone/adb/OOM/
    thermal trouble, 2026-11-18: 134 Crash, 78 on the flaky list): the caller pauses and re-checks the device first (start_pause/end_pause).
    A case that crashes again on its second run is `retried`: a real Crash, written at once, no longer feeds the streak."""
    def __init__(self, streak=BURST_STREAK, max_bursts=BURST_MAX):
        self.n, self.max_bursts = streak, max_bursts
        self.streak, self.bursts = 0, 0
        self.held, self.retried = {}, set()

    def holds(self, case):
        return case not in self.retried

    def chunk_crashed(self, cases):
        for c in cases:
            self.held[c] = True
        if cases:
            self.streak += 1

    def release(self):
        """held cases -> second run; returns them"""
        out = list(self.held)
        self.retried |= set(out)
        self.held.clear()
        self.streak = 0
        return out

    def burst(self):
        return self.streak >= self.n

    def start_pause(self):
        self.bursts += 1
        return list(self.held)

    def end_pause(self):
        self.release()


def push_so(src, sha, dst):
    rc, o = adb("push", src, dst, timeout=180)
    if rc != 0:
        print("push failed: " + o.strip()[-200:], flush=True)
        sys.exit(79)
    if sha_phone(dst) == sha:
        return True
    print("sha256 mismatch after push; re-pushing once", flush=True)       # farm owns the check: one automatic re-push before giving up
    adb("shell", f"rm -f {dst}", timeout=30)
    rc, o = adb("push", src, dst, timeout=180)
    if rc != 0:
        print("re-push failed: " + o.strip()[-200:], flush=True)
        return False                                                       # the caller reports the driver mismatch (exit 76 path)
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
    boot = BootWatch(a.out + ".boot")                          # the boot this result file belongs to (persisted: survives a resume)
    if boot.id is None and boot.up is None:
        print("WARN: cannot read boot id/uptime; a device reboot mid-run cannot be detected on this phone", flush=True)
    elif boot.resumed:
        print(f"RESUME in boot {boot.id[:8] if boot.id else '?'} (last uptime {boot.up if boot.up is not None else -1:.0f}s): "
              "the same boot as the earlier chunks", flush=True)
    chunk = max(1, min(a.chunk, CHUNK_MAX))
    if chunk != a.chunk:
        print(f"CHUNK {a.chunk} capped to {chunk}: a shard must not be able to exhaust the phone", flush=True)
    guard = BurstGuard(int(os.environ.get("SHELLROUTE_BURST_STREAK", BURST_STREAK)), int(os.environ.get("SHELLROUTE_BURST_MAX", BURST_MAX)))
    while True:
        todo = [c for c in cases if c not in done and c not in guard.held]
        if not todo:
            if not guard.release():                            # nothing left but held crashes: they get their second run now
                break
            continue
        if gated(a.lock):
            sys.exit(75)                                       # held (not yet written) crashes are dropped: a resume runs them again
        if sha_phone(icd) != a.sha:                            # before every chunk, ON THE PHONE
            print("SO MISMATCH on the phone", flush=True)
            sys.exit(76)
        why = boot.check()                                     # the phone rebooted under us: the boot is not this run's any more
        if why:
            print(f"verdict: device_reboot {why}; aborting this run, never retrying it blindly", flush=True)
            sys.exit(82)
        batch = todo[:chunk]
        cf, qf = f"{G}/c.{a.run}.cases", f"{G}/c.{a.run}.qpa"
        open(os.path.join(tmp, "c.cases"), "w").write("\n".join(batch) + "\n")
        if adb("push", os.path.join(tmp, "c.cases"), cf)[0] != 0:
            fails += 1
            if fails > 2:
                sys.exit(79)
            time.sleep(10)
            continue
        cmd = (f"cd {G}; rm -f {qf}; {limit_preamble(a.run)}; {envs} MALI_ICD={icd} timeout {a.wall} lib/ld-linux-aarch64.so.1 --library-path {G}/lib "
               f"./deqp-vk --deqp-caselist-file={cf} --deqp-log-filename={qf} --deqp-log-images=disable --deqp-log-shader-sources=disable "
               "--deqp-surface-type=fbo --deqp-gl-config-name=rgba8888d24s8 --deqp-visibility=hidden --deqp-shadercache=disable "
               f"--deqp-watchdog=enable --deqp-log-flush=enable >c.{a.run}.out 2>&1; echo rc=$?")
        oom0 = oom_kill_count(a.run)                            # cgroup kills are invisible in the output when the tail is empty
        rc, o = adb("shell", cmd, timeout=a.wall + 40)
        for l in o.splitlines():
            if l.startswith("LIMITS "):
                print(f"chunk{len(done)} {l.strip()}", flush=True)
        if "LIMITS none" in o or rc == 84:                      # the preamble refused to fork unbounded
            print("verdict: memory_cap no usable memory bound (cgroup and ulimit -v both refused); refusing to run unbounded", flush=True)
            sys.exit(83)
        m_rc = re.search(r"rc=(\d+)", o)                      # the child's own status, next to adb's (which only says adb worked)
        child = int(m_rc.group(1)) if m_rc else None
        if rc == 124:
            adb("shell", "pkill -f deqp-vk", timeout=15)
        pr = os.path.join(tmp, "c.qpa")
        if os.path.exists(pr):
            os.unlink(pr)
        adb("pull", qf, pr, timeout=90)
        text = open(pr, errors="replace").read() if os.path.exists(pr) else ""
        if rc not in (0, 124) or ("rc=" not in o and rc != 124):        # adb itself failed: not the cases' fault
            why = boot.check()
            if why:
                print(f"verdict: device_reboot {why}; aborting this run (the phone rebooted under this chunk)", flush=True)
                sys.exit(82)
            eff = child if (child is not None and rc in (0, 124)) else rc
            if eff in (134, 135, 136, 137):                         # killed by the memory bound (ulimit -v: bad_alloc/abort, cgroup/kernel: SIGKILL)
                tail = adb("shell", f"tail -c 2000 {G}/c.{a.run}.out 2>/dev/null", timeout=20)[1]
                oom1 = oom_kill_count(a.run)
                if OOM_RE.search(tail) or (oom0 >= 0 and oom1 > oom0):   # counter moved even when the tail is empty
                    print(f"verdict: memory_cap rc={rc} cap={MEM_KB}KB: the run hit its memory bound; shard smaller (SHELLROUTE_CHUNK_MAX) rather than retrying", flush=True)
                    sys.exit(83)
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
            tail = adb("shell", f"tail -c 600 {G}/c.{a.run}.out", timeout=20)[1]
            with open(os.path.join(L.state_dir(), "logs", f"{a.run}-notes.tsv"), "a") as nf:
                nf.write(f"{unfinished}\t{crash_note(text, tail)}\n")
        if not got:
            tail = adb("shell", f"tail -c 400 {G}/c.{a.run}.out", timeout=20)[1].strip().replace("\n", " | ")
            if OOM_RE.search(tail):
                print(f"verdict: memory_cap cap={MEM_KB}KB: no result and the run ran out of memory; shard smaller instead of retrying: {tail[-200:]}", flush=True)
                sys.exit(83)
            print(f"NOPARSE {batch[0]} rc-line={o.strip()[-30:]!r} out: {tail}", flush=True)       # diagnostics for the farm log
            got[batch[0]] = "Crash"                            # nothing parsed: first case counts as Crash so we always progress
        crashed = [c for c in batch if got.get(c) == "Crash"]
        for c in batch:
            if c in got and not (got[c] == "Crash" and guard.holds(c)):
                done[c] = got[c]
                f.write(f"{c}\t{got[c]}\t\n")
        if crashed:
            guard.chunk_crashed([c for c in crashed if guard.holds(c)])      # first-time crashes wait here until a clean chunk shows they were not a burst
        else:
            guard.release()                                    # a clean chunk ends the streak: the held crashes were isolated, they get their second run
        f.flush()
        print(f"# chunk: done {len(done)}/{len(cases)} [{int(time.time() - t0)}s]", flush=True)
        temps(a.run, len(done))
        if guard.burst():
            n = guard.start_pause()
            print(f"BURST: {guard.streak} chunks in a row ended in a crash (cases like {n[0]}); pause {BURST_PAUSE()} s, re-check the device, rerun them "
                  f"(burst {guard.bursts}/{guard.max_bursts})", flush=True)
            if guard.bursts > guard.max_bursts:
                print("UNSTABLE: crash bursts keep coming back; results of the held cases are discarded", flush=True)
                sys.exit(81)
            time.sleep(BURST_PAUSE())
            if gated(a.lock):
                sys.exit(75)
            if adb("shell", "echo alive", timeout=30)[1].strip() != "alive":
                print("phone unreachable after a burst pause", flush=True)
                sys.exit(79)
            guard.end_pause()
    adb("shell", f"rm -f {G}/c.{a.run}.cases {G}/c.{a.run}.qpa {G}/c.{a.run}.out", timeout=20)
    print(f"SO_ON_PHONE {icd}", flush=True)                    # farmd verifies it once more, then removes it
    print("DONE", flush=True)


def smoke(a):
    # before anything is staged: an explicit selection with no case names must not wipe the run dir, push artifacts or
    # start a boot record (a push failure would then be reported instead of the input error)
    if a.only is not None and not any(c for c in a.only.split(",")):
        print("--only given with no case names; refusing to report an empty run as a pass", flush=True)
        return 2
    d = f"{V}/{a.run}"
    adb("shell", f"rm -rf {d}; mkdir -p {d}/s", timeout=30)
    out = os.path.join(a.tree, "vk", "out", "a64-bionic")
    if not push_so(os.path.join(out, "libvulkan_mali.so"), a.sha, f"{d}/libvulkan_mali.so"):
        print("SO MISMATCH after push", flush=True)
        sys.exit(77)
    for t in ("t_vk_draw", "t_vk_compute", "t_vk_meta", "t_vk_feat"):
        if adb("push", os.path.join(out, t), f"{d}/{t}")[0] != 0:
            sys.exit(79)
    adb("shell", f"chmod +x {d}/t_vk_*", timeout=20)
    for sp in glob.glob(os.path.join(a.tree, "vk", "shader", "*.spv")):
        adb("push", sp, f"{d}/s/{os.path.basename(sp)}", timeout=60)
    envs = " ".join(a.env.split())
    ok, tmp = True, tempfile.mkdtemp(prefix="shellroute-")
    runs = []
    boot = BootWatch(os.path.join(L.state_dir(), f"{a.run}-smoke.boot"))
    # a t_vk_feat case runs only if this tree's binary knows it (the route may merge ahead of the cases)
    try:
        feat_bin = open(os.path.join(out, "t_vk_feat"), "rb").read()
    except OSError:
        feat_bin = b""
    feat = [c for c in ONLY_FEAT if c.encode() + b"\0" in feat_bin]
    # the draw cases are static, but a tree built before a case existed has no CASE_ON for it: ask the binary
    try:
        draw_bin = open(os.path.join(out, "t_vk_draw"), "rb").read()
    except OSError:
        draw_bin = b""
    draw_cases = [c for c in ONLY_DRAW if c.encode() + b"\0" in draw_bin] or ONLY_DRAW
    for n, extra in (("t_vk_draw", f"--loop {a.loop}"), ("t_vk_compute", "--loop 5"), ("t_vk_meta", ""), ("t_vk_feat", "")):
        if a.only is None:
            if n == "t_vk_feat":
                runs.extend((n, f"--only {c}", c) for c in feat)
            else:
                runs.append((n, extra, ""))
            if n == "t_vk_draw":
                runs.append((n, "--only many_draws", "many_draws"))   # 3k/10k/50k draws in one frame (arena overflow, was DEVICE_LOST)
            continue
        for c in a.only.split(","):                                  # --only: one run per case this binary knows, binaries without a match are skipped
            if c in (draw_cases if n == "t_vk_draw" else feat if n == "t_vk_feat" else ONLY_META):
                runs.append((n, f"{extra} --only {c}", c))
    if a.only is not None:   # a requested case this route does not schedule must be reported, not silently skipped
        ran = {oc for _, _, oc in runs if oc}
        missing = [c for c in a.only.split(",") if c and c not in ran]
        if missing:
            print("unscheduled --only case(s): " + ",".join(re.sub(r"[\r\n]", "?", c) for c in missing), flush=True)
            # farmd counts failures from JSON records: a plain line would leave the run reported as clean
            print(json.dumps({"test": "schedule", "missing": missing, "ok": False}), flush=True)
            ok = False
    for n, extra, oc in runs:
        if gated(a.lock):
            sys.exit(75)
        if sha_phone(f"{d}/libvulkan_mali.so") != a.sha:
            print("SO MISMATCH on the phone", flush=True)
            sys.exit(77)
        why = boot.check()
        if why:
            print(f"verdict: device_reboot {why}; aborting this smoke run, never retrying it blindly", flush=True)
            sys.exit(82)
        sp = "" if n == "t_vk_meta" else f"--spv-dir {d}/s"
        rc, o = adb("shell", f"cd {d}; {limit_preamble(a.run)}; {envs} timeout 120 ./{n} --direct {d}/libvulkan_mali.so {sp} {extra} >{n}.log 2>&1; echo rc=$?", timeout=170)
        m = re.search(r"rc=(\d+)", o)
        if not m:
            if "LIMITS none" in o:
                print("verdict: memory_cap no usable memory bound (cgroup and ulimit -v both refused); refusing to run unbounded", flush=True)
                sys.exit(83)
            print("phone unreachable: " + o.strip()[-120:], flush=True)
            sys.exit(79)
        lp = os.path.join(tmp, n + ".log")
        adb("pull", f"{d}/{n}.log", lp, timeout=60)
        log = open(lp, errors="replace").read() if os.path.exists(lp) else ""
        fl = [x for x in log.splitlines() if '"fail"' in x]
        for x in log.splitlines()[-200:]:                            # kept by farmd (farm log)
            print(json.dumps({"log": n + (":" + oc if oc else ""), "line": x[:400]}), flush=True)
        r = int(m.group(1))
        if r == 134 and OOM_RE.search(log):                      # the memory bound, not a QPA crash: never let BurstGuard retry it
            print("verdict: memory_cap rc=134 in %s: the smoke run hit its memory bound; shard smaller" % n, flush=True)
            sys.exit(83)
        ok = ok and r == 0
        if oc and ('"test":"%s"' % oc) not in log:   # --env MALI_SKIP_CASES=<case> (and a feature-gated skip) prints no record
            print(json.dumps({"test": "schedule", "not_run": oc, "ok": False}), flush=True)
            ok = False
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
    ap.add_argument("--only", default=None)   # None = the whole suite; "" is an explicit (empty) selection
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
    nodim = bool(os.environ.get("SHELLROUTE_NODIM"))           # smoke fallback on a locked guest-route phone: its screen/settings are not touched at all
    if not nodim:
        dim()
    try:
        return {"smoke": smoke, "daily": daily}[a.mode](a)
    finally:
        if not nodim and not os.environ.get("SHELLROUTE_KEEP_DIM"):            # the idle sweep keeps the screen dark between chunks; farmd undims when it stops
            undim()


if __name__ == "__main__":
    sys.exit(main() or 0)
