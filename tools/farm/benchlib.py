# SPDX-License-Identifier: MIT
"""benchlib: bookkeeping and scheduling policy of the farm `bench` suite (pure functions, unit-tested in test_farm.py).

Policy: explicit jobs > auto bench > idle sweep. Auto bench jobs are queued (a) when main gets a commit that touches the driver
(vk/src, libkbase) for every phone that is available, (b) for a phone that has been idle > IDLE_SECS with its health gate ok and no
bench row for the current main. Rows live in <state>/bench/results.jsonl (one per phone/driver/variant, with ref + commit + job)."""
import json, os, re, subprocess, time
import menulib as M

IDLE_SECS = 900                                  # a phone idle this long (gate ok) runs the oldest missing bench
RELEVANT = re.compile(r"^(vk/src/|libkbase/)")   # a main commit touching these changes the driver's performance
UNAVAILABLE = ("absent", "missing", "down", "disabled", "unknown")
DEVICE_SUITE_VARIANT = "a64-glibc-wl"            # bench jobs load the WSI (Wayland) variant: the non-WSI one cannot present
SHOW = (("triangle-3k", "tri CPU", "cpu_ms"), ("cube-720p", "cube GPU", "gpu_ms"))   # digest: offscreen (uncapped) numbers


def bench_dir(state):
    d = os.path.join(state, "bench")
    os.makedirs(d, exist_ok=True)
    return d


def results_path(state):
    return os.path.join(bench_dir(state), "results.jsonl")


def load_rows(state):
    p = results_path(state)
    rows = []
    if os.path.exists(p):
        for l in open(p):
            try:
                rows.append(json.loads(l))
            except ValueError:
                pass
    return rows


def append_rows(state, rows, job, alias):
    """stamp rows with the farm job / ref / full commit / device alias and append them -> number written"""
    with open(results_path(state), "a") as f:
        for r in rows:
            f.write(json.dumps(dict(r, phone=alias, job=job["id"], ref=job["ref"], commit_full=job["commit"])) + "\n")
    return len(rows)


def has_row(rows, alias, commit):
    return any(r.get("phone") == alias and r.get("commit_full") == commit for r in rows)


def touches_driver(paths):
    return any(RELEVANT.match(p) for p in paths)


def eligible(dev):
    """a phone the bench suite can use: an in-app (guest) route and not opted out (`bench = false` in devices.toml)"""
    return (dev.get("route") != "shell" or dev.get("guest") is True) and dev.get("bench", True) is not False


def available(devs, dstate, disabled):
    """aliases of phones to queue auto bench for: eligible, not disabled by the owner, and seen recently (not absent/missing/down)"""
    return [a for a, d in devs.items() if eligible(d) and a not in disabled and dstate.get(a, {}).get("status", "unknown") not in UNAVAILABLE]


def plan_trigger(last, tip, changed_paths, avail_classes):
    """main moved from `last` to `tip` -> (new_last, classes to queue a bench job for).
    First sight of main (last None) only records it: no storm of jobs for history."""
    if not last:
        return tip, []
    if last == tip:
        return last, []
    return tip, (sorted(avail_classes) if touches_driver(changed_paths) else [])


def queued(jobs, commit, cls=None, alias=None):
    """is there an auto bench job for this commit that is not cancelled and not finished with an error (errors are retried by the idle rule only after the idle clock restarts)?"""
    for j in jobs:
        if j.get("suite") != "bench" or j.get("commit") != commit or not j.get("auto"):
            continue
        for c, p in j["parts"].items():
            if p["state"] == "cancelled" or (cls and c != cls):
                continue
            if alias and j.get("pin") and j["pin"] != alias:
                continue
            return True
    return False


def idle_candidates(now, idle_since, devs, dstate, rows, jobs, tip, disabled, idle_secs=IDLE_SECS, running=()):
    """phones that should get an auto bench for main's tip now: idle >= idle_secs, health gate ok (status up), no row for `tip`,
    nothing queued for it. Longest idle first (= oldest missing)."""
    out = []
    for a in available(devs, dstate, disabled):
        if a in running or dstate.get(a, {}).get("status") != "up":
            continue
        if now - idle_since.get(a, now) < idle_secs:
            continue
        if has_row(rows, a, tip) or queued(jobs, tip, devs[a]["class"], a):
            continue
        out.append(a)
    return sorted(out, key=lambda a: idle_since.get(a, now))


def order_jobs(jobs):
    """scheduling order of queued jobs: by priority (farmlib.job_prio: gates 0, bench/menu 10, exploratory cases 20), at equal priority explicit
    (submitted by a human/agent) before auto, each by submission order"""
    import farmlib
    return sorted(jobs, key=lambda j: (farmlib.job_prio(j), 1 if j.get("auto") else 0, j.get("seq", 0)))


def pin_ok(job, alias):
    return not job.get("pin") or job["pin"] == alias


def latest_by_phone(rows):
    """-> {phone: {"commit": full, "ref": ref, "date": date, "vals": {(variant, driver): row}}} for each phone's newest tested commit"""
    out = {}
    for r in rows:                                         # file order = time order
        ph = r.get("phone")
        if ph is None or r.get("commit_full") is None:
            continue
        cur = out.get(ph)
        if cur is None or cur["commit"] != r["commit_full"]:
            cur = out[ph] = {"commit": r["commit_full"], "ref": r.get("ref"), "date": r.get("date"), "vals": {}}
        cur["vals"][(r.get("variant"), r.get("driver"))] = r
    return out


def digest_lines(rows):
    """one line per phone: latest bench, triangle CPU ms and cube GPU ms, ours vs bridge (offscreen, uncapped)"""
    out = []
    for ph, e in sorted(latest_by_phone(rows).items()):
        seg = []
        for variant, tag, key in SHOW:
            v = []
            for drv in ("ours", "bridge"):
                r = e["vals"].get((variant, drv))
                x = (r or {}).get("offscreen", {}).get(key)
                v.append("n/a" if x is None else f"{x:.2f}")
            seg.append(f"{tag} ours {v[0]} / bridge {v[1]} ms")
        out.append(f"bench {ph:8} {str(e['ref'])[:12]}@{e['commit'][:7]} {e.get('date') or ''}: " + "; ".join(seg))
    return out


REGRESS_PCT = 10.0                                # a newest-vs-previous move worse than this is flagged


def regressions(rows, pct=REGRESS_PCT):
    """newest bench row of each (phone, driver, variant) vs the previous row of a different commit -> list of text lines.
    Flags offscreen fps dropping, or offscreen cpu_ms rising, by more than pct percent. Rows lacking a number are skipped."""
    hist = {}
    for r in rows:                                         # file order = time order
        if r.get("commit_full") is None or not isinstance(r.get("offscreen"), dict):
            continue
        h = hist.setdefault((r.get("phone"), r.get("driver"), r.get("variant")), [])
        if h and h[-1]["commit_full"] == r["commit_full"]:
            h[-1] = r                                      # re-run of the same commit replaces it
        else:
            h.append(r)
    out = []
    for (ph, drv, var), h in sorted(hist.items(), key=lambda kv: tuple(str(x) for x in kv[0])):
        if len(h) < 2:
            continue
        old, new = h[-2]["offscreen"], h[-1]["offscreen"]
        for key, worse_if_up in (("fps", False), ("cpu_ms", True)):
            a, b = old.get(key), new.get(key)
            if not isinstance(a, (int, float)) or not isinstance(b, (int, float)) or a <= 0:
                continue
            d = (b - a) / a * 100.0
            if (d > pct) if worse_if_up else (d < -pct):
                out.append(f"REGRESSION {ph} {drv} {var} offscreen {key} {a:g} -> {b:g} ({d:+.0f}%) "
                           f"{h[-2]['commit_full'][:7]}..{h[-1]['commit_full'][:7]}")
    return out


def menu_digest_lines(state):
    return M.digest_lines(M.load_rows(M.menu_path(state)))


# ---------- publishing: farm rows -> bench/results.jsonl + bench/RESULTS.md on origin/main, from a clone the farm owns
PUB_RETRIES = 5


def row_key(r):
    """one published row per phone, driver, version, variant and mali-open commit (so every version keeps its own block)"""
    return (r.get("phone"), r.get("driver"), r.get("version"), r.get("variant"), r.get("commit_full") or r.get("commit"))


def row_shape_ok(r):
    return not (r.get("version") == "fake" or "presented" not in r or "offscreen" not in r)


def publishable(r, peers=()):
    """fake rows and implausible ones (the invalid-winner symptom: ours cube with no real work) are never published.
    "Implausible" = ours cube offscreen GPU time below half of the bridge's for the same phone, variant and commit (`peers` = rows to look the
    bridge up in); with no bridge row to compare with, below 2 ms. The fixed 2 ms alone dropped every valid ours cube row of the Poco X8
    (G925: bridge 1.32 ms, ours 1.40-1.53 ms), so RESULTS.md never had one."""
    if not row_shape_ok(r):
        return False
    if r.get("bench") == "cube" and r.get("driver") == "ours":
        g = (r["offscreen"] or {}).get("gpu_ms")
        if g is None:
            return True
        ref = [p["offscreen"]["gpu_ms"] for p in peers if p.get("driver") == "bridge" and p.get("bench") == "cube" and p.get("phone") == r.get("phone")
               and p.get("variant") == r.get("variant") and (p.get("commit_full") or p.get("commit")) == (r.get("commit_full") or r.get("commit"))
               and isinstance(p.get("offscreen"), dict) and p["offscreen"].get("gpu_ms")]
        return g >= (0.5 * min(ref) if ref else 2.0)
    return True


def merge_rows(existing, new):
    """-> merged list: later rows replace earlier ones with the same key (and move to the end = newest); relative order otherwise kept"""
    d = {}
    # the bridge row of a commit may already be published (ours-only job): without it the absolute 2 ms floor dropped valid X8 rows.
    # An existing row that a new same-key row replaces is not a peer (its value is gone after the merge); a new row only replaces it when it passes.
    repl = {row_key(x) for x in new if row_shape_ok(x) and not (x.get("bench") == "cube" and x.get("driver") == "ours")}
    peers = [e for e in existing if row_key(e) not in repl] + list(new)
    for r in existing + [x for x in new if publishable(x, peers)]:
        k = row_key(r)
        d.pop(k, None)
        d[k] = r
    return list(d.values())


def _git(clone, *a, timeout=120):
    r = subprocess.run(["git", "-C", clone, *a], capture_output=True, text=True, timeout=timeout)
    return r.returncode, (r.stdout + r.stderr).strip()


def publish(state, remote, clone=None, log=print, retries=PUB_RETRIES, report_cmd=None, branch="main", perf_cmd=None):
    """merge <state>/bench/results.jsonl into bench/results.jsonl of a clone owned by the farm, regenerate RESULTS.md, commit
    "bench: <phones> rows for <short commit>" and push to `remote` <branch>. Never touches any other checkout. A rejected push
    (somebody ff-merged meanwhile) is retried from a fresh fetch. -> {"status": "pushed|nothing|error", "n": rows changed, "msg": ...}"""
    allrows = load_rows(state)
    rows = [r for r in allrows if row_shape_ok(r)]          # the plausibility check runs in merge_rows, where the already published bridge rows are peers
    mrows = [r for r in M.load_rows(M.menu_path(state)) if M.publishable(r)]
    if not rows and not mrows:
        return {"status": "nothing", "n": 0, "msg": "no publishable rows in the farm state"}
    clone = clone or os.path.join(bench_dir(state), "publish-clone")
    if not os.path.isdir(os.path.join(clone, ".git")):
        os.makedirs(os.path.dirname(clone), exist_ok=True)
        r = subprocess.run(["git", "clone", "-q", "--branch", branch, remote, clone], capture_output=True, text=True, timeout=300)
        if r.returncode:
            return {"status": "error", "n": 0, "msg": "clone failed: " + r.stderr.strip()[-200:]}
    msg = ""
    for attempt in range(retries):
        for step in (("fetch", "-q", "origin", branch), ("checkout", "-q", "-B", branch, "origin/" + branch), ("reset", "-q", "--hard", "origin/" + branch)):
            rc, o = _git(clone, *step)
            if rc:
                return {"status": "error", "n": 0, "msg": f"git {step[0]}: {o[-200:]}"}
        jl = os.path.join(clone, "bench", "results.jsonl")
        old = [json.loads(l) for l in open(jl) if l.strip()] if os.path.exists(jl) else []
        new = merge_rows(old, rows)
        changed = [r for r in new if r not in old]
        ml = os.path.join(clone, "bench", "menu.jsonl")                       # Steam menu rows: own file, same push
        mold = M.load_rows(ml)
        mnew = M.merge_rows(mold, mrows)
        mchanged = [r for r in mnew if r not in mold]
        if not changed and not mchanged:
            return {"status": "nothing", "n": 0, "msg": "already published"}
        os.makedirs(os.path.dirname(jl), exist_ok=True)
        if changed:
            with open(jl, "w") as f:
                f.write("".join(json.dumps(r) + "\n" for r in new))
        if mchanged:
            with open(ml, "w") as f:
                f.write("".join(json.dumps(r) + "\n" for r in mnew))
        rep = report_cmd or ["python3", "-I", os.path.join(clone, "tools", "bench2"), "--report"]
        r = subprocess.run(rep, cwd=clone, capture_output=True, text=True, timeout=120)
        if r.returncode:
            return {"status": "error", "n": 0, "msg": "report failed: " + (r.stdout + r.stderr).strip()[-200:]}
        if changed:                                                             # charts + README table: best effort, a plotting failure never blocks publishing
            pc = perf_cmd or ["python3", "-I", os.path.join(clone, "bench", "plot.py"), "--root", clone]
            try:
                pr = subprocess.run(pc, cwd=clone, capture_output=True, text=True, timeout=120)
                ok, why = pr.returncode == 0, (pr.stdout + pr.stderr).strip()[-200:]
            except Exception as e:                                              # missing interpreter, timeout, ...
                ok, why = False, str(e)
            if not ok:                                                          # keep README + charts exactly as on origin: never stage a half-written report
                log("publish: plot failed (ignored): " + why)
                _git(clone, "checkout", "-q", "--", "README.md", "bench/charts")
                _git(clone, "clean", "-qfd", "--", "bench/charts")
        if changed:
            phones = ",".join(sorted({r["phone"] for r in changed}))
            short = (changed[-1].get("commit_full") or changed[-1].get("commit") or "?")[:9]
            subject = f"bench: {phones} rows for {short}"
        else:                                                                   # menu-only: keyed by MediaDeck version, not by mali-open commit
            phones = ",".join(sorted({r["phone"] for r in mchanged}))
            short = "vc" + ",".join(str(v) for v in sorted({r["version_code"] for r in mchanged}))
            subject = f"bench: Steam menu rows {phones} for MediaDeck {short}"
        for f in ("bench/results.jsonl", "bench/menu.jsonl", "bench/RESULTS.md", "README.md", "bench/charts"):
            if os.path.exists(os.path.join(clone, f)):                          # one pathspec per call: a missing file must not abort the add
                _git(clone, "add", f)
        rc, o = _git(clone, "commit", "-q", "-m", subject)
        if rc:
            return {"status": "error", "n": 0, "msg": "commit: " + o[-200:]}
        rc, o = _git(clone, "push", "-q", "origin", f"HEAD:{branch}")
        if rc == 0:
            return {"status": "pushed", "n": len(changed) + len(mchanged), "msg": f"{phones} @ {short}"}
        msg = o[-200:]
        log(f"publish: push rejected (attempt {attempt + 1}/{retries}), refetching: {msg[-80:]}")
        time.sleep(0 if retries <= 1 else min(2 * attempt, 6))
    return {"status": "error", "n": 0, "msg": "push rejected " + str(retries) + " times: " + msg}
