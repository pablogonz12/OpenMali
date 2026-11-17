# SPDX-License-Identifier: MIT
"""mali-farm shared library: state dir, device table, job files, result comparison. Python 3.11+, stdlib only.
Devices are referred to by alias only; serials/addresses/paths come from the devices.toml in the user's config dir."""
import collections, datetime, fcntl, json, os, re, subprocess, tempfile, time, tomllib

SUITES = ("smoke", "daily", "bench", "cases")
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


def list_jobs():
    d = os.path.join(state_dir(), "jobs")
    out = []
    for n in sorted(os.listdir(d)):
        if n.endswith(".json"):
            try:
                out.append(read_json(os.path.join(d, n)))
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


def new_job(ref, commit, dirty, suite, need, env, repo_name, cases=None, repeat=1):
    base = time.strftime("%Y%m%d-%H%M%S") + "-" + commit[:8]
    jid, n = base, 1
    while os.path.exists(job_path(jid)):
        n += 1
        jid = f"{base}-{n}"
    job = {"id": jid, "ref": ref, "commit": commit, "dirty": dirty, "suite": suite, "env": env, "repo": repo_name,
           "created": now(), "seq": time.time_ns(), "so_sha256": None, "build": None,
           "parts": {c: {"state": "pending", "attempts": 0, "device": None} for c in need}}
    if suite == "cases":                                  # `farm submit --suite cases --case X --repeat N`: only these cases, N times
        job["cases"], job["repeat"] = list(cases or []), int(repeat)
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
class PhoneLock:
    def __init__(self, dev, fake=False):
        if fake:
            d = os.path.join(state_dir(), "fake-locks")
            os.makedirs(d, exist_ok=True)
            self.path = os.path.join(d, dev["alias"] + ".lock")
        else:
            self.path = f"/tmp/qsafe-{dev['lock']}.lock"
        self.fd = None

    def acquire(self):
        """non-blocking: True if we hold it now (an agent's qsafe then prints PHONE BUSY)"""
        fd = open(self.path, "a")
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            fd.close()
            return False
        self.fd = fd
        return True

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
