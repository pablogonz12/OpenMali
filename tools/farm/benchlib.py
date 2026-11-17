# SPDX-License-Identifier: MIT
"""benchlib: bookkeeping and scheduling policy of the farm `bench` suite (pure functions, unit-tested in test_farm.py).

Policy: explicit jobs > auto bench > idle sweep. Auto bench jobs are queued (a) when main gets a commit that touches the driver
(vk/src, libkbase) for every phone that is available, (b) for a phone that has been idle > IDLE_SECS with its health gate ok and no
bench row for the current main. Rows live in <state>/bench/results.jsonl (one per phone/driver/variant, with ref + commit + job)."""
import json, os, re

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
    return dev.get("route") != "shell" and dev.get("bench", True) is not False


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
    """scheduling order of queued jobs: explicit (submitted by a human/agent) first, then auto bench; each by submission order"""
    return sorted(jobs, key=lambda j: (1 if j.get("auto") else 0, j.get("seq", 0)))


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
