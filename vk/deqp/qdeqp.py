"""qdeqp: run dEQP-VK case lists on a phone guest and summarise. Python 3 stdlib, run with -I via scripts/quick/qdeqp.
  qdeqp <cmf|poco|serial> <case-list> [--icd ours|panvk] [--chunk 500] [--case-timeout 60] [--limit N] [--keep] [--tag NAME]
Results (resumable): runs/qdeqp/<phone>-<icd>-<list>.tsv  (case, status, secs-of-chunk-share, note); summary .json + .csv next to it.
Status: Pass / Fail / NotSupported / Crash (incl. hang and QPA without result) / Other (QualityWarning etc. count as Pass).
Crashed chunk: the case after the last finished one is retried ALONE once; if it dies again it is recorded as Crash, chunk restarts behind it.
Guest only (proot): the glibc deqp-vk of driver/vk/deqp/build.sh. ICD ours = driver/vk/out/a64-glibc/libvulkan_mali.so (pushed),
panvk = the oracle $BL_VKBRIDGE_FALLBACK_ICD (DroidDeck's imported PanVK-kbase)."""
import json, os, re, subprocess, sys, time, datetime, hashlib, tarfile, collections

QD = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.realpath(os.path.join(QD, "..", "..", ".."))
if not os.path.isdir(os.path.join(ROOT, "scripts", "quick")):   # driver/ may be a symlink: fall back to the cwd checkout
    ROOT = os.getcwd()
sys.path.insert(0, os.path.join(ROOT, "scripts", "quick"))
import quick                      # noqa: E402  (Phone, ENVV, helpers; its main() is not run on import)

BUILD = os.path.join(QD, "out", "a64-glibc")
DL = "/storage/emulated/0/Download/qdeqp"
PASSLIKE = {"Pass", "QualityWarning", "CompatibilityWarning"}
IDLE = 180   # watchdog: seconds without QPA growth
STRESS = re.compile(r"object_management\.(alloc_callback_fail|max_|multithreaded)|many")
OKFOCUS = re.compile(r"com\.mi\.android\.globallauncher|com\.android\.settings|DroidTest|com\.ludashi\.benchmark")


def norm(s):
    return "Pass" if s in PASSLIKE else s if s in ("Fail", "NotSupported", "Crash", "ResourceHog") else "Other:" + s


def parse_qpa(text):
    """-> (list[(case,status)] finished, last_unfinished_case|None)"""
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


def stage_binaries(p, icd):
    exe = os.path.join(BUILD, "external", "vulkancts", "modules", "vulkan", "deqp-vk")
    data = os.path.join(BUILD, "external", "vulkancts", "modules", "vulkan")
    if not os.path.exists(exe):
        raise SystemExit(f"no {exe}; build with driver/vk/deqp/build.sh")
    tar = f"/tmp/qdeqp-{os.getpid()}.tar"
    with tarfile.open(tar, "w") as t:
        t.add(exe, "deqp-vk")
        for d in ("vulkan", "gles2", "gles3"):
            if os.path.isdir(os.path.join(data, d)):
                t.add(os.path.join(data, d), d)
    h = hashlib.sha1(open(exe, "rb").read()).hexdigest()[:10] + str(os.path.getsize(tar))
    stamp = f"{DL}/stage.stamp"
    cur = p.adb("shell", f"cat {stamp} 2>/dev/null").strip()
    p.adb("shell", f"mkdir -p {DL}")
    if cur != h:
        p.adb("push", tar, f"{DL}/deqp.tar", timeout=600)
        p.adb("shell", f"echo {h} > {stamp}")
        p.res["pushed_binaries"] = True
    os.unlink(tar)
    if icd == "ours":
        so = os.path.join(ROOT, "driver", "vk", "out", "a64-glibc", "libvulkan_mali.so")
        p.adb("push", so, f"{DL}/libvulkan_mali.so", timeout=120)
    return h


def guest(p, script, timeout):
    g = p.ctl("guest", "--timeout", str(timeout), "--", "sh", "-c", script, timeout=timeout + 30)
    so = g.get("stdout", "") if isinstance(g, dict) else ""
    return so.get("text", "") if isinstance(so, dict) else so


def run_chunk(p, cases, tag, icd, case_timeout, h, wall=540):
    idle = max(45, case_timeout)
    """Runs one chunk in the guest; returns (finished list, unfinished case|None, raw tail)"""
    cl = f"/tmp/qdeqp_{tag}.cases"
    local = f"/tmp/qdeqp-{p.serial}-{os.getpid()}.cases"
    open(local, "w").write("\n".join(cases) + "\n")
    p.adb("push", local, f"{DL}/{tag}.cases")
    os.unlink(local)
    qpa = f"{DL}/{tag}.qpa"
    p.adb("shell", f"rm -f {qpa}")
    if icd == "ours":
        icdsetup = (f"cp {DL}/libvulkan_mali.so /tmp/qd/libvulkan_mali.so; printf '%s' "
                    "'{\"file_format_version\":\"1.0.1\",\"ICD\":{\"library_path\":\"/tmp/qd/libvulkan_mali.so\",\"api_version\":\"1.3.0\"}}' "
                    "> /tmp/qd/icd.json; I=/tmp/qd/icd.json")
    else:
        icdsetup = "I=$BL_VKBRIDGE_FALLBACK_ICD"
    wall = min(wall, 570)                                   # droiddeckctl guest allows at most 600 s per call
    s = (f"mkdir -p /tmp/qd; [ -x /tmp/qd/deqp-vk ] && [ \"$(cat /tmp/qd/stamp 2>/dev/null)\" = {h} ] || "
         f"(rm -rf /tmp/qd/vulkan; tar -C /tmp/qd -xf {DL}/deqp.tar && echo {h} > /tmp/qd/stamp); chmod +x /tmp/qd/deqp-vk; {icdsetup}; "
         f"cd /tmp/qd; VK_DRIVER_FILES=$I VK_ICD_FILENAMES=$I VK_LOADER_LAYERS_DISABLE='~all~' BL_OPEN_DRIVER=1 "
         f"timeout {wall} ./deqp-vk --deqp-caselist-file={DL}/{tag}.cases --deqp-log-filename={qpa} --deqp-log-images=disable "
         f"--deqp-log-shader-sources=disable --deqp-surface-type=fbo --deqp-gl-config-name=rgba8888d24s8 --deqp-visibility=hidden "
         f"--deqp-shadercache=disable --deqp-watchdog=enable --deqp-log-flush=enable >/tmp/qd/{tag}.out 2>&1 & P=$!; "
         f"L=0; T=$(date +%s); LT=$T; "          # watchdog: kill on low RAM / high load / QPA idle 3 min
         f"while kill -0 $P 2>/dev/null; do sleep 4; N=$(date +%s); Z=$(stat -c %s {qpa} 2>/dev/null || echo 0); "
         f"[ \"$Z\" != \"$L\" ] && L=$Z && LT=$N; MA=$(awk '/MemAvailable/{{print int($2/1024)}}' /proc/meminfo); "
         f"LD=$(cut -d. -f1 /proc/loadavg); "
         f"if [ $MA -lt 1500 ] || [ $((N-LT)) -gt {idle} ] || {{ [ $LD -gt 40 ] && [ $((N-T)) -gt 60 ]; }}; then "
         f"echo __HOG mem=$MA load=$LD idle=$((N-LT)); pkill -KILL -x deqp-vk; kill -KILL $P; break; fi; done; "
         f"wait $P; echo __rc=$?; tail -c 400 /tmp/qd/{tag}.out")
    out = guest(p, s, wall + 25)
    local_q = f"/tmp/qdeqp-{p.serial}-{os.getpid()}.qpa"
    p.adb("pull", qpa, local_q, timeout=120)
    txt = open(local_q, errors="replace").read() if os.path.exists(local_q) else ""
    if os.path.exists(local_q):
        os.unlink(local_q)
    fin, unf = parse_qpa(txt)
    return fin, unf, out[-300:], "__HOG" in out


def calm(p, maxwait=120):
    """wait until the phone's load1 < 40 (phone idles ~20 with Steam up) and MemAvailable > 2 GB (previous process fully gone)"""
    for _ in range(maxwait // 5):
        o = p.adb("shell", "cat /proc/loadavg; grep MemAvailable /proc/meminfo; pgrep -x deqp-vk", timeout=20).split()
        try:
            if float(o[0]) < 40 and int(o[o.index("MemAvailable:") + 1]) > 2_000_000 and "deqp-vk" not in o:
                return True
        except Exception:
            pass
        time.sleep(5)
    return False


def focus_ok(p):
    return bool(OKFOCUS.search(p.adb("shell", "dumpsys window | grep mCurrentFocus", timeout=15)))


def summarize(rows, listcases):
    byg = collections.OrderedDict()
    for c in listcases:
        g = ".".join(c.split(".")[:4])
        d = byg.setdefault(g, collections.Counter())
        d["total"] += 1
        st = rows.get(c)
        d["pending" if st is None else st.split(":")[0]] += 1
    return byg


def csv_out(path, byg):
    cols = []
    with open(path, "w") as f:
        f.write("group,total,pass,fail,notsupported,crash,hog,other,notrun,pass_pct_of_total,pass_pct_of_supported\n")
        tot = collections.Counter()
        for g, d in list(byg.items()) + [("TOTAL", None)]:
            if d is None:
                d = tot
            else:
                tot.update(d)
            sup = d["total"] - d["NotSupported"] - d["pending"]
            f.write(f"{g},{d['total']},{d['Pass']},{d['Fail']},{d['NotSupported']},{d['Crash']},{d['ResourceHog']},{d['Other']},{d['pending']},"
                    f"{100 * d['Pass'] / max(1, d['total']):.1f},{100 * d['Pass'] / max(1, sup):.1f}\n")


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    phone, listf, o = argv[0], argv[1], {"icd": "ours", "chunk": 500, "case-timeout": 60, "limit": 0, "keep": False, "tag": "", "stress": 0}
    it = iter(argv[2:])
    for a in it:
        if a == "--keep":
            o["keep"] = True
        elif a.startswith("--") and a[2:] in o:
            o[a[2:]] = type(o[a[2:]])(next(it))
        else:
            print("unknown arg", a)
            return 2
    p = quick.Phone(phone)
    cases = [l.strip() for l in open(listf) if l.strip()]
    if not o["stress"]:                                       # allocate-until-failure / threaded cases go to the stress list
        st = [c for c in cases if STRESS.search(c)]
        cases = [c for c in cases if not STRESS.search(c)]
        if st:
            open(os.path.join(QD, "lists", "stress.txt"), "w").write("\n".join(sorted(set(st))) + "\n")
            print(f"# {len(st)} stress cases excluded (lists/stress.txt; --stress 1 to run them)", file=sys.stderr)
    if o["limit"]:
        cases = cases[:o["limit"]]
    lname = os.path.splitext(os.path.basename(listf))[0] + (f"-{o['tag']}" if o["tag"] else "")
    outd = os.path.join(ROOT, "runs", "qdeqp")
    os.makedirs(outd, exist_ok=True)
    base = os.path.join(outd, f"{p.name}-{o['icd']}-{lname}")
    rows = {}
    if os.path.exists(base + ".tsv"):                         # resume
        for l in open(base + ".tsv"):
            f = l.rstrip("\n").split("\t")
            if len(f) >= 2:
                rows[f[0]] = f[1]
    p.res.update({"icd": o["icd"], "list": listf, "cases": len(cases), "resumed": len(rows)})
    if not p.ensure_adb():
        return p.finish("deqp", "PROBLEM adb missing", False)
    waited = 0
    while not focus_ok(p) and waited < 600:
        time.sleep(15)
        waited += 15
    if not focus_ok(p):
        return p.finish("deqp", "FAIL focus not launcher/Settings/DroidTest after 10 min", False)
    st, ready = p.start_steam()
    if not ready:
        return p.finish("deqp", f"FAIL session not READY; fix: scripts/quick/qrestore {p.name}", False)
    h = stage_binaries(p, o["icd"])
    todo = [c for c in cases if c not in rows]
    tsv = open(base + ".tsv", "a")
    t0, n_chunks = time.time(), 0
    tag = f"{p.serial[:6]}"

    def rec(c, s, note=""):
        rows[c] = s
        tsv.write(f"{c}\t{s}\t{note}\n")
        tsv.flush()

    while todo:
        chunk, fails = todo[:o["chunk"]], 0
        calm(p)
        fin, unf, tail, hog = run_chunk(p, chunk, tag, o["icd"], o["case-timeout"], h)
        n_chunks += 1
        seen = set()
        for c, s in fin:
            if c in chunk and c not in seen:
                rec(c, norm(s))
                seen.add(c)
        left = [c for c in chunk if c not in seen]
        if unf in left:                                       # deqp runs in tree order, not list order: blame the case that was running
            left.remove(unf)
            left.insert(0, unf)
        if left and not fin and unf is None:                  # nothing parsed: loader or binary problem
            p.res["tail"] = tail
            if n_chunks == 1:
                tsv.close()
                return p.finish("deqp", f"FAIL deqp-vk produced no QPA: {tail.strip()[-160:]!r}", False)
        if left and hog:                                      # watchdog fired: resource hog, not a crash
            rec(left[0], "ResourceHog", "watchdog: " + tail.strip()[:80].replace("\n", " "))
            p.stop()
            time.sleep(20)
            p.start_steam()
            fails += 1
        elif left and unf == left[0]:  # deterministic crash/hang while running this case: no retry
            rec(left[0], "Crash", "died while running (no retry)")
            fails += 1
        elif left:
            c = left[0]                                       # the case that was running (or the next one)
            calm(p, 60)
            time.sleep(4)                                     # let the dead process release kbase/proot state
            f2, u2, _, hg = run_chunk(p, [c], tag + "r", o["icd"], o["case-timeout"], h, wall=o["case-timeout"] + 30)   # retry alone once
            if not (f2 and f2[0][0] == c):
                time.sleep(8)
                f2, u2, _, hg = run_chunk(p, [c], tag + "r", o["icd"], o["case-timeout"], h, wall=o["case-timeout"] + 30)
            if f2 and f2[0][0] == c:
                rec(c, norm(f2[0][1]), "retry")
            else:
                rec(c, "Crash", "hang/crash, retried alone")
            fails += 1
        todo = [c for c in todo if c not in rows]
        print(f"# chunk {n_chunks}: done {len(rows)}/{len(cases)} [{time.time() - t0:.0f}s]", file=sys.stderr, flush=True)
    tsv.close()
    byg = summarize(rows, cases)
    csv_out(base + ".csv", byg)
    tot = collections.Counter()
    for d in byg.values():
        tot.update(d)
    sup = tot["total"] - tot["NotSupported"]
    p.res.update({"counts": dict(tot), "tsv": os.path.relpath(base + ".tsv", ROOT), "csv": os.path.relpath(base + ".csv", ROOT),
                  "chunks": n_chunks})
    if not o["keep"]:
        p.stop()
    line = (f"{o['icd']} {os.path.basename(listf)} {tot['total']} cases: pass {tot['Pass']} fail {tot['Fail']} "
            f"NS {tot['NotSupported']} crash {tot['Crash']} other {tot['Other']} ({100 * tot['Pass'] / max(1, sup):.1f}% of supported)")
    ok = tot["Crash"] == 0 and tot["Fail"] == 0 and tot["Other"] == 0
    p.finish("deqp", line, ok)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
