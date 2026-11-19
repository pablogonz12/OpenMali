# SPDX-License-Identifier: MIT
"""mali-farm shared library: state dir, device table, job files, result comparison. Python 3.11+, stdlib only.
Devices are referred to by alias only; serials/addresses/paths come from the devices.toml in the user's config dir."""
import collections, datetime, fcntl, json, os, re, subprocess, tempfile, time, tomllib

SUITES = ("smoke", "daily", "bench", "cases", "menu")      # + "sweep": internal, never submitted
PASSLIKE = {"Pass", "QualityWarning", "CompatibilityWarning"}


def state_dir():
    d = os.environ.get("MALI_FARM_STATE") or os.path.expanduser("~/.local/state/mali-farm")
    for s in ("jobs", "runs", "work", "artifacts", "logs"):
        os.makedirs(os.path.join(d, s), exist_ok=True)
    return d


def devices_path():
    return os.environ.get("MALI_FARM_DEVICES") or os.path.expanduser("~/.config/mali-farm/devices.toml")


def load_devices():
    """-> (cfg dict, {alias: device dict}). Raises SystemExit with a clear message when the file is missing."""
    p = devices_path()
    if not os.path.exists(p):
        raise SystemExit(f"farm: no device table at {p} (see tools/farm/README.md)")
    cfg = tomllib.load(open(p, "rb"))
    devs = {}
    for d in cfg.get("device", []):
        d.setdefault("lock", d["alias"])
        d.setdefault("baseline", d["lock"])
        devs[d["alias"]] = d
    return cfg, devs


def load_config():
    """~/.config/mali-farm/farm.toml: quick_root (scripts/quick + tools), main_checkout (holds third_party, vk/deqp/out, lists)"""
    p = os.environ.get("MALI_FARM_CONFIG") or os.path.expanduser("~/.config/mali-farm/farm.toml")
    c = tomllib.load(open(p, "rb")) if os.path.exists(p) else {}
    return {k: os.path.expanduser(v) for k, v in c.items() if isinstance(v, str)}


def adb_live():
    """-> {transport id: state} from `adb devices` (ids: plain serial, adb-<serial>-x._adb-tls-connect._tcp, ip:port)"""
    rc, out = sh(["adb", "devices"], 15)
    live = {}
    for l in out.splitlines()[1:]:
        f = l.split()
        if len(f) >= 2:
            live[f[0]] = f[1]
    return live


_ROUTE_MISS = {}


def wireless_routes(dev):
    out = []
    for k in ("wireless", "address"):
        w = dev.get(k)
        out += [] if not w else ([w] if isinstance(w, str) else list(w))
    return out


_SERIALNO = {}


def serial_of(tid):
    """the phone's real serial (`getprop ro.serialno`) behind a transport id, cached 5 min (hits) / 1 min (misses); hard 5 s timeout so
    one hung route cannot block the others. None = unknown."""
    c = _SERIALNO.get(tid)
    if c and time.time() - c[1] < (300 if c[0] else 60):
        return c[0]
    rc, o = sh(["adb", "-s", tid, "shell", "getprop ro.serialno"], 5)
    f = o.split() if rc == 0 else []
    s = f[0] if f and "error" not in o.lower() and "offline" not in o.lower() else None
    _SERIALNO[tid] = (s, time.time())
    return s


def resolve(dev, connect=False):
    """alias -> live transport id, identified by the phone's SERIAL, never by the transport name. Order: USB serial; mDNS entry containing
    the serial; a configured ip:port route that is listed; any other listed ip:port whose `ro.serialno` equals the serial (the phone
    changed address); then (connect=True) `adb connect <route>`. `transport = "usb"`: USB only. `transport = "wireless"`: ignores
    the plain serial. A stale/unreachable route is a soft miss (6 s connect timeout, remembered 2 min). None = not reachable."""
    usb_only = dev.get("transport", "usb+wireless") == "usb"
    routes = [] if usb_only else wireless_routes(dev)
    want = dev["serial"]

    def pick():
        live = {i: s for i, s in adb_live().items() if s == "device"}
        if want in live and dev.get("transport") != "wireless":
            return want
        if usb_only:
            return None
        t = next((i for i in live if want in i and i != want), None)
        if t:
            return t
        cands = [r for r in routes if r in live] + [i for i in live if i not in routes and i != want and ":" in i and not i.startswith("adb-")]
        for i in cands:
            s = serial_of(i)
            if s == want or (s is None and i in routes):
                return i
        return None
    t = pick()
    if t or not connect:
        return t
    for r in routes:
        if time.time() - _ROUTE_MISS.get(r, 0) < 120:        # unreachable stored route (e.g. the phone moved to another subnet): soft miss
            continue
        rc, o = sh(["adb", "connect", r], 6)
        if rc != 0 or not re.search(r"connected to", o or ""):
            _ROUTE_MISS[r] = time.time()
        t = pick()
        if t:
            return t
    return None


def run_watched(cmd, timeout, alive, grace=45, poll=5, **kw):
    """like sh() but kills the whole process group and returns rc 125 if alive() stays False for `grace` seconds (phone unplugged:
    never hang on a dead transport). rc 124 = timeout."""
    import signal
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True, **kw)
    out = []
    import threading
    th = threading.Thread(target=lambda: out.append(p.stdout.read()), daemon=True)
    th.start()
    t0, lost = time.time(), None
    rc = None
    while rc is None:
        try:
            rc = p.wait(poll)
        except subprocess.TimeoutExpired:
            if time.time() - t0 > timeout:
                rc = 124
            elif (st := alive()) == "stop":
                rc = 125
            elif st:
                lost = None
            else:
                lost = lost or time.time()
                if time.time() - lost > grace:
                    rc = 125
            if rc in (124, 125):
                try:
                    os.killpg(p.pid, signal.SIGKILL)
                except OSError:
                    pass
                p.wait()
    th.join(5)
    txt = (out[0] if out else b"").decode(errors="replace")
    return rc, txt + ("\nDEVICE-LOST" if rc == 125 else "\nTIMEOUT" if rc == 124 else "")


def store_route(alias, addr, path=None):
    """keep the stored `<ip>:5555` route of device `alias` in devices.toml equal to `addr` (the phone's CURRENT wlan0 address): an old
    `ip:5555` entry in `wireless`/`address` is replaced, other routes (other ports) stay, none yet -> a `wireless` line is added.
    Text edit (comments and other entries untouched), atomic. -> True if the file changed."""
    path = path or devices_path()
    lines = open(path).read().split("\n")
    start = next((i for i, l in enumerate(lines) if re.match(r'\s*alias\s*=\s*"%s"' % re.escape(alias), l)), None)
    if start is None:
        return False
    b0 = max((i for i in range(start + 1) if lines[i].lstrip().startswith("[[")), default=0)
    b1 = next((i for i in range(start + 1, len(lines)) if lines[i].lstrip().startswith("[[")), len(lines))
    changed, seen = False, False
    for i in range(b0, b1):
        m = re.match(r'(\s*(wireless|address)\s*=\s*)(\[[^\]]*\]|"[^"]*")(\s*#.*)?$', lines[i])
        if not m or not any(x.endswith(":5555") for x in re.findall(r'"([^"]*)"', m.group(3))):
            continue
        vals = re.findall(r'"([^"]*)"', m.group(3))
        keep = [x for x in vals if not x.endswith(":5555")]
        new = keep + ([] if seen else [addr])
        seen = True
        if new == vals:
            continue
        v = ('["' + '", "'.join(new) + '"]') if m.group(2) == "wireless" or len(new) != 1 else f'"{new[0]}"'
        lines[i] = m.group(1) + v + (m.group(4) or "")
        changed = True
    if not seen:
        wl = next((i for i in range(b0, b1) if re.match(r"\s*wireless\s*=", lines[i])), None)
        if wl is not None:
            m = re.match(r'(\s*wireless\s*=\s*)(\[[^\]]*\]|"[^"]*")(\s*#.*)?$', lines[wl])
            vals = re.findall(r'"([^"]*)"', m.group(2)) if m else []
            lines[wl] = (m.group(1) if m else "wireless = ") + '["' + '", "'.join(vals + [addr]) + '"]' + ((m.group(3) or "") if m else "")
        else:
            last = max(i for i in range(b0, b1) if lines[i].strip())
            lines.insert(last + 1, f'wireless = ["{addr}"]   # added by farmd (adb tcpip route; follows the phone\'s wlan0 IP)')
        changed = True
    if not changed:
        return False
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".tmp-")
    with os.fdopen(fd, "w") as f:
        f.write("\n".join(lines))
    os.replace(tmp, path)
    return True


def disabled_dir():
    d = os.path.join(state_dir(), "disabled")
    os.makedirs(d, exist_ok=True)
    return d


def now():
    return datetime.datetime.now().astimezone().isoformat(timespec="seconds")


def write_json(path, obj):
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".tmp-")
    with os.fdopen(fd, "w") as f:
        json.dump(obj, f, indent=1, sort_keys=True)
    os.replace(tmp, path)


def read_json(path):
    with open(path) as f:
        return json.load(f)


def job_path(jid):
    return os.path.join(state_dir(), "jobs", jid + ".json")


def run_path(jid, cls):
    return os.path.join(state_dir(), "runs", f"{jid}-{cls}.json")


def list_jobs(sweep=False):
    """all jobs by submission order; the internal idle-sweep jobs (suite "sweep") only with sweep=True"""
    d = os.path.join(state_dir(), "jobs")
    out = []
    for n in sorted(os.listdir(d)):
        if n.endswith(".json"):
            try:
                j = read_json(os.path.join(d, n))
                if sweep or j.get("suite") != "sweep":
                    out.append(j)
            except (ValueError, OSError):
                pass
    return sorted(out, key=lambda j: j.get("seq", 0))


def find_job(jid):
    """accepts a unique prefix"""
    allj = list_jobs()
    m = [j for j in allj if j["id"] == jid] or [j for j in allj if j["id"].startswith(jid)]
    if len(m) != 1:
        raise SystemExit(f"farm: job {jid!r} " + ("not found" if not m else "is ambiguous"))
    return m[0]


def find_passed(jobs, commit, suite, need, env, only="", cases=None, repeat=1):
    """idempotent submit: an existing job of the same commit/suite/inputs whose every wanted class is done+pass (non-dirty) -> that job, else None"""
    for j in reversed(jobs):
        if (j.get("commit") != commit or j.get("suite") != suite or j.get("dirty") or (j.get("env") or "") != (env or "")
                or (j.get("only") or "") != (only or "") or list(j.get("cases") or []) != list(cases or []) or (j.get("repeat") or 1) != (repeat or 1)):
            continue
        ps = j.get("parts", {})
        if need and {c for c, p in ps.items() if p.get("state") != "cancelled"} == set(need) and all(ps[c].get("state") == "done" and ps[c].get("verdict") == "pass" for c in need):
            return j
    return None


SINGLE_CASE_PRIO = 5          # between gates (0) and bench/menu (10): a lane's one-case diagnostic goes before everything but gates


def find_inflight(jobs, commit, suite, need, env, only="", cases=None, repeat=1):
    """idempotent submit, in flight: a job of the same commit/suite/inputs with the same classes whose parts are all pending/running -> it"""
    for j in reversed(jobs):
        if (j.get("commit") != commit or j.get("suite") != suite or (j.get("env") or "") != (env or "")
                or (j.get("only") or "") != (only or "") or list(j.get("cases") or []) != list(cases or []) or (j.get("repeat") or 1) != (repeat or 1)):
            continue
        ps = {c: p for c, p in j.get("parts", {}).items() if p.get("state") != "cancelled"}
        if set(ps) == set(need) and any(p.get("state") in ("pending", "running") for p in ps.values()) and all(p.get("state") in ("pending", "running", "done") for p in ps.values()):
            return j
    return None


STALE_COMMITS = 5


def job_verdict(job):
    """pending while any part is not finished; then error > fail > pass"""
    parts = [p for p in job["parts"].values() if p["state"] != "cancelled"]
    if not parts:
        return "cancelled"
    if any(p["state"] in ("pending", "running") for p in parts):
        return "pending"
    vs = {p.get("verdict", "error") for p in parts}
    for v in ("error", "invalid", "fail"):
        if v in vs:
            return v
    return "pass"


# scheduling priority, lower runs first: merge gates (smoke/daily of a branch head) < bench/menu < exploratory `cases` runs < idle sweep (always last)
SUITE_PRIO = {"smoke": 0, "daily": 0, "bench": 10, "menu": 10, "cases": 20, "sweep": 30}
GATE_SUITES = ("smoke", "daily")
SUPERSEDE_SUITES = ("smoke", "daily", "cases", "bench", "menu")   # never sweep


def job_prio(job):
    """the job's priority: its `prio` field (farm submit --prio / farm prio), else the default of its suite (jobs stored before the field existed)"""
    if job.get("prio") is not None:
        return int(job["prio"])
    return SUITE_PRIO.get(job.get("suite"), 0)


def stable_ref(ref):
    """a plain branch/tag name: not empty, not HEAD, not a batch, not a revision expression (HEAD~1, main^, @{u}, a commit hash) that moves or is not a ref"""
    return bool(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._/-]*", ref or "")) and ref != "HEAD" and not ref.startswith("HEAD") \
        and not re.fullmatch(r"[0-9a-f]{7,40}", ref)


def supersede_old_gates(new, jobs, is_ancestor):
    """a new gate job (smoke/daily) obsoletes the pending parts of older gate jobs of the same line of commits: same repo, suite, env and
    --only, an older commit that is an ancestor of the new one (is_ancestor(old, new)), and only the classes the new job covers. Running and
    finished parts stay. Jobs of OTHER suites (cases/bench/menu/daily vs smoke...) of the same repo and the same named ref (not HEAD, not a
    batch) at an older ancestor commit are superseded too: that head is dead, its pending diagnostics are moot (a tip that changed is
    never what the lane wants measured). Marks the parts cancelled in the stored jobs and returns [(job id, old commit, [classes])]"""
    out = []
    if new.get("suite") not in GATE_SUITES or new.get("auto") or new.get("keep_old"):
        return out
    for j in jobs:
        same = (j.get("suite") == new["suite"] and (j.get("env") or {}) == (new.get("env") or {})
                and (j.get("only") or "") == (new.get("only") or ""))
        ref = new.get("ref") or ""
        sibling = (not same and j.get("suite") != new["suite"] and j.get("suite") in SUPERSEDE_SUITES and stable_ref(ref) and j.get("ref") == ref)
        if (j["id"] == new["id"] or not (same or sibling) or j.get("repo") != new.get("repo") or j.get("auto")
                or j["commit"] == new["commit"] or j.get("seq", 0) > new.get("seq", 0) or not is_ancestor(j["commit"], new["commit"])):
            continue
        cs = [c for c, p in j["parts"].items() if p["state"] == "pending" and c in new["parts"]]
        if not cs:
            continue
        try:                                              # re-read right before the write: a part farmd moved to running since the snapshot must not be reset
            fresh = read_json(job_path(j["id"]))
            cs = [c for c in cs if fresh["parts"].get(c, {}).get("state") == "pending"]
            j = fresh
        except (OSError, ValueError, KeyError):
            pass
        if not cs:
            continue
        for c in cs:
            j["parts"][c]["state"] = "cancelled"
        write_json(job_path(j["id"]), j)
        out.append((j["id"], j["commit"], cs))
    return out


def queue_view(jobs, order):
    """-> ({class: (running, pending)}, {(job id, class): 1-based position among the pending parts of that class in scheduling order}).
    `order` sorts the jobs (benchlib.order_jobs); a pending part pinned to a phone is counted in its class like the others"""
    by_cls, pos = {}, {}
    for j in jobs:
        for c, p in j["parts"].items():
            r, q = by_cls.get(c, (0, 0))
            by_cls[c] = (r + (p["state"] == "running"), q)
    for j in order(jobs):
        for c, p in j["parts"].items():
            if p["state"] == "pending":
                r, q = by_cls[c]
                by_cls[c] = (r, q + 1)
                pos[(j["id"], c)] = q + 1
    return by_cls, pos


def new_job(ref, commit, dirty, suite, need, env, repo_name, cases=None, repeat=1, only="", auto=False, pin="", prio=None, keep_old=False):
    base = time.strftime("%Y%m%d-%H%M%S") + "-" + commit[:8]
    jid, n = base, 1
    while os.path.exists(job_path(jid)):
        n += 1
        jid = f"{base}-{n}"
    job = {"id": jid, "ref": ref, "commit": commit, "dirty": dirty, "suite": suite, "env": env, "repo": repo_name,
           "created": now(), "seq": time.time_ns(), "so_sha256": None, "build": None,
           "prio": int(prio) if prio is not None else SUITE_PRIO.get(suite, 0),
           "parts": {c: {"state": "pending", "attempts": 0, "device": None} for c in need}}
    if suite == "cases":                                  # `farm submit --suite cases --case X --repeat N`: only these cases, N times
        job["cases"], job["repeat"] = list(cases or []), int(repeat)
    if auto:                                              # queued by farmd itself (bench triggers): lower priority than explicit jobs
        job["auto"] = True
    if keep_old:                                          # farm submit --keep-old: do not cancel pending gate jobs of older commits
        job["keep_old"] = True
    if pin:                                               # runs only on this phone alias (idle-triggered bench)
        job["pin"] = pin
    if only:                                              # `--suite smoke --only a,b`: only these test cases (t_vk_draw/compute/meta --only)
        job["only"] = only
    write_json(job_path(jid), job)
    return job


# ---- case-by-case comparison against a baseline TSV (case<TAB>status<TAB>note)
def read_tsv(path):
    rows = {}
    with open(path) as f:
        for l in f:
            x = l.rstrip("\n").split("\t")
            if len(x) >= 2 and x[0]:
                rows[x[0]] = "Pass" if x[1] in PASSLIKE else x[1]
    return rows


def merge_baselines(branch, ref):
    """branch/ref: (tsv dict, commit8) or None. A case is Pass if it is Pass in either; otherwise the branch's status (else ref's).
    -> (merged dict, text naming the commit each came from and how many cases only the reference passes)"""
    if not branch or not ref:
        one = branch or ref
        return one[0], f"baseline {'branch' if branch else 'main'} {one[1]}"
    merged = dict(ref[0])
    merged.update(branch[0])
    only_ref = 0
    for c, s in ref[0].items():
        if s == "Pass" and branch[0].get(c) != "Pass":
            merged[c] = "Pass"
            only_ref += 1
    return merged, f"baseline branch {branch[1]} + main {ref[1]} ({only_ref} cases Pass only on main)"


def load_flaky():
    """known-flaky cases (tools/farm/known-flaky.txt: `case  # evidence`): not counted as new failures, still reported as flaky_ignored"""
    try:
        return {l.split("#")[0].strip() for l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "known-flaky.txt")) if l.split("#")[0].strip()}
    except OSError:
        return set()


FLAKY_RERUNS = 3


MAX_LOST_SHARE = 0.03          # a daily where more than 3 % of the cases crashed or have no result, though the baseline has one, is invalid (2026-11-18 v10: 9 %)
FLAKY_RECORD_MAX = 25          # more newly-flaky cases than this in one run is a burst, not flakiness: they do not go to the flaky list


def lost_cases(base, cur, cases):
    """cases whose result is Crash or missing now although the baseline has a real result for them (a baseline Crash is not news)"""
    return [c for c in cases if cur.get(c, "Missing") in ("Crash", "Missing") and base.get(c) not in (None, "Crash", "Missing")]


def flaky_path():
    return os.path.join(state_dir(), "flaky.tsv")


def load_flaky_db():
    """flaky list (<state>/flaky.tsv: case, gpu version, fails, runs, first seen, last seen) -> {(case, gpu): [fails, runs, first, last]}"""
    out = {}
    try:
        for l in open(flaky_path()):
            x = l.rstrip("\n").split("\t")
            if len(x) >= 6 and x[0] and not x[0].startswith("#"):
                out[(x[0], x[1])] = [int(x[2]), int(x[3]), x[4], x[5]]
    except (OSError, ValueError):
        pass
    return out


def write_flaky_db(db):
    with open(flaky_path() + ".lock", "a") as lk:
        fcntl.flock(lk, fcntl.LOCK_EX)
        tmp = flaky_path() + ".tmp"
        with open(tmp, "w") as f:
            f.write("# case\tgpu\tfails\truns\tfirst_seen\tlast_seen\n")
            for (c, g), (a, b, t0, t1) in sorted(db.items()):
                f.write(f"{c}\t{g}\t{a}\t{b}\t{t0}\t{t1}\n")
        os.replace(tmp, flaky_path())


def record_flaky(entries, gpu, when=None, seed=False):
    """entries: [(case, fails, runs)]. Adds the counts to the list (seed=True: replaces counts only for new entries, so a re-seed is idempotent)."""
    when = when or now()
    with open(flaky_path() + ".lock", "a") as lk:
        fcntl.flock(lk, fcntl.LOCK_EX)
        db = load_flaky_db()
        for case, fails, runs in entries:
            e = db.get((case, gpu))
            if e is None:
                db[(case, gpu)] = [fails, runs, when, when]
            elif not seed:
                e[0] += fails
                e[1] += runs
                e[3] = when
        tmp = flaky_path() + ".tmp"
        with open(tmp, "w") as f:
            f.write("# case\tgpu\tfails\truns\tfirst_seen\tlast_seen\n")
            for (c, g), (a, b, t0, t1) in sorted(db.items()):
                f.write(f"{c}\t{g}\t{a}\t{b}\t{t0}\t{t1}\n")
        os.replace(tmp, flaky_path())


def flip_scan(days=3, since=None):
    """cases that flipped between Pass and Fail on the SAME build across stored dailies of the last `days` days: same commit (clean tree) or the same driver binary (so_sha256), same env,
    or main's dailies pooled per class (main moves: only cases that changed status at least twice in time order count). -> {(case, class): [fails, runs, first, last]}"""
    cutoff = since or (datetime.datetime.now().astimezone() - datetime.timedelta(days=days)).isoformat(timespec="seconds")
    groups = {}                                                   # (class, key) -> [(started, {case: status})]
    for j in list_jobs():
        if j.get("suite") != "daily":
            continue
        for cls, p in j["parts"].items():
            rp = run_path(j["id"], cls)
            run = read_json(rp) if os.path.exists(rp) else None
            if not run or run.get("verdict") in ("error", "invalid") or (run.get("started") or "") < cutoff:
                continue
            f = os.path.join(state_dir(), "logs", f"{j['id']}-{p.get('device')}-daily.tsv")
            if not os.path.exists(f):
                continue
            tsv = read_tsv(f)
            if not tsv:
                continue
            keys = {("so", run["so_sha256"], run.get("env") or "")} if run.get("so_sha256") else set()      # the identical driver binary (dirty trees too)
            if not run.get("dirty"):
                keys.add(("commit", j["commit"], run.get("env") or ""))
            if j.get("ref") == "main" and not run.get("dirty"):
                keys.add(("main",))
            for key in keys:
                groups.setdefault((cls, key), []).append((run.get("started") or "", tsv))
    out = {}
    for (cls, key), runs in groups.items():
        if len(runs) < 2:
            continue
        seen = {}
        for t, tsv in runs:
            for c, st in tsv.items():
                if st in ("NotSupported", "Missing"):
                    continue
                seen.setdefault(c, []).append((t, st == "Pass"))
        for c, v in seen.items():
            v.sort()
            npass = sum(1 for _, ok in v if ok)
            flips = sum(1 for a, b in zip(v, v[1:]) if a[1] != b[1])
            if 0 < npass < len(v) and (key != ("main",) or flips >= 2):       # on main one Pass->Fail (or Fail->Pass) change is a regression/fix, not noise
                e = out.setdefault((c, cls), [0, 0, v[0][0], v[0][0]])
                if e[1] < len(v):                              # keep the largest group's counts for a case seen via both keys
                    e[0], e[1] = len(v) - npass, len(v)
                e[2], e[3] = min(e[2], min(t for t, _ in v)), max(e[3], max(t for t, _ in v))
    return out


def ns_to_fail(base, cur, cases=None):
    """cases that were NotSupported in the baseline and now run and fail (Fail/Crash/...): invisible to new_failures, which only lists
    baseline passes. -> [[case, status]]"""
    cases = cases if cases is not None else sorted(set(base) | set(cur))
    return [[c, cur[c]] for c in cases if base.get(c) == "NotSupported" and cur.get(c) not in (None, "NotSupported", "Pass", "Missing")]


def compare(base, cur, cases=None):
    """base/cur: {case: status}. new_failures = passed in base, not Pass now (incl. not run); newly_passing = the reverse.
    cases: the list that was requested (cases absent from cur count as 'Missing')."""
    cases = cases if cases is not None else sorted(set(base) | set(cur))
    counts = collections.Counter()
    newfail, newpass = [], []
    for c in cases:
        s = cur.get(c, "Missing")
        counts[s] += 1
        b = base.get(c)
        if b == "Pass" and s != "Pass":
            newfail.append([c, s])
        elif b is not None and b != "Pass" and s == "Pass":
            newpass.append(c)
    counts["total"] = len(cases)
    return dict(counts), newfail, newpass


def parse_env(s):
    """'K=V K2=V2' -> validated string (kept as text; passed to valrun as one argument)"""
    s = (s or "").strip()
    for tok in s.split():
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*=[A-Za-z0-9_.,:/+-]*", tok):
            raise SystemExit(f"farm: bad --env token {tok!r} (K=V, no spaces/quotes in values)")
    return s


# ---- locking (the qsafe lock: flock on /tmp/qsafe-<lock>.lock; fake mode uses a private dir)
def lock_holders(path, proc="/proc"):
    """[(pid, comm)] of processes holding an open fd on `path` (same inode)"""
    try:
        ino = os.stat(path).st_ino
    except OSError:
        return []
    out = []
    for pid in os.listdir(proc):
        if not pid.isdigit():
            continue
        try:
            for fd in os.listdir(f"{proc}/{pid}/fd"):
                try:
                    if os.stat(f"{proc}/{pid}/fd/{fd}").st_ino == ino and os.path.realpath(f"{proc}/{pid}/fd/{fd}") == os.path.realpath(path):
                        out.append((int(pid), open(f"{proc}/{pid}/comm").read().strip()))
                        break
                except OSError:
                    pass
        except OSError:
            pass
    return out


class PhoneLock:
    STALE_AFTER = 600          # s a lock may stay busy before a pure-adb holder counts as a leaked fd
    on_stale = None            # callback(msg): farmd sets it to its alert()
    _busy_since = {}

    def __init__(self, dev, fake=False):
        if fake:
            d = os.path.join(state_dir(), "fake-locks")
            os.makedirs(d, exist_ok=True)
            self.path = os.path.join(d, dev["alias"] + ".lock")
        else:
            self.path = f"/tmp/qsafe-{dev['lock']}.lock"
        self.fd = None

    def _try(self):
        fd = open(self.path, "a")                 # python fds are O_CLOEXEC: children (adb, its server) never inherit the lock
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            fd.close()
            return False
        self.fd = fd
        return True

    def acquire(self, now=None, proc="/proc"):
        """non-blocking: True if we hold it now (an agent's qsafe then prints PHONE BUSY).
        Busy > STALE_AFTER and every holder is an adb process (leaked fd in an adb server): STALE-LOCK alert, re-create the file, retry."""
        if self._try():
            PhoneLock._busy_since.pop(self.path, None)
            return True
        now = time.time() if now is None else now
        since = PhoneLock._busy_since.setdefault(self.path, now)
        if now - since > self.STALE_AFTER:
            h = lock_holders(self.path, proc)
            if h and all(c == "adb" for _, c in h):
                if self.on_stale:
                    self.on_stale(f"STALE-LOCK {self.path} held {int(now - since)}s only by adb pid(s) {[p for p, _ in h]}: leaked fd; lock file re-created")
                try:
                    os.unlink(self.path)
                except OSError:
                    pass
                PhoneLock._busy_since.pop(self.path, None)
                return self._try()
        return False

    def release(self):
        if self.fd:
            fcntl.flock(self.fd, fcntl.LOCK_UN)
            self.fd.close()
            self.fd = None


def sh(cmd, timeout, **kw):
    """subprocess with hard timeout; returns (rc, text). rc 124 = timeout."""
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout, **kw)
        return p.returncode, p.stdout.decode(errors="replace") + p.stderr.decode(errors="replace")
    except subprocess.TimeoutExpired as e:
        return 124, ((e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else "") + "\nTIMEOUT"
    except FileNotFoundError as e:
        return 127, str(e)


RECOVER_HINT = "recover: over usb run `adb tcpip 5555`, then `adb connect <phone-ip>:5555` (ip: phone Settings > Wi-Fi, or `adb shell ip route`)"
