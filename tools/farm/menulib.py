# SPDX-License-Identifier: MIT
"""menulib: bookkeeping and policy of the farm `menu` suite (the Steam menu benchmark of MediaDeck, scripts/quick/menubench).
Pure functions, unit-tested in test_farm.py. The menu runs on the Mali bridge, not on our driver: numbers change with the MediaDeck
build (versionCode on the phone), not with mali-open commits. One row per (phone, versionCode, profile) in <state>/bench/menu.jsonl,
published as bench/menu.jsonl and as the "Steam menu" section of bench/RESULTS.md.

Policy: queued (auto, same level as the auto bench) for a phone that has been idle > IDLE_SECS, health gate ok, when its installed
versionCode differs from the last one the farm queued a menu run for. Never run while the owner uses the phone."""
import datetime, json, os, re, statistics

PROFILES = ("default", "uncapped")
PKG_DEFAULT = "com.ludashi.benchmark"            # menubench's DROIDDECK_PACKAGE default: the MediaDeck app


def menu_path(state):
    d = os.path.join(state, "bench")
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, "menu.jsonl")


def load_rows(path):
    rows = []
    if os.path.exists(path):
        for l in open(path):
            try:
                rows.append(json.loads(l))
            except ValueError:
                pass
    return rows


def row_key(r):
    return (r.get("phone"), r.get("version_code"), r.get("profile"))


def merge_rows(existing, new):
    """later rows replace earlier ones with the same (phone, versionCode, profile) and move to the end (= newest), EXCEPT that an
    invalid row (self-check failed, no usable rep) never replaces a valid one"""
    d = {}
    for r in list(existing) + list(new):
        k = row_key(r)
        old = d.get(k)
        if old is not None and old.get("valid") and not r.get("valid"):
            continue
        d.pop(k, None)
        d[k] = r
    return list(d.values())


def append_rows(state, new):
    """merge `new` into <state>/bench/menu.jsonl -> number of rows stored (rewritten atomically)"""
    p = menu_path(state)
    rows = merge_rows(load_rows(p), new)
    with open(p + ".tmp", "w") as f:
        f.write("".join(json.dumps(r) + "\n" for r in rows))
    os.replace(p + ".tmp", p)
    return len(new)


def publishable(r):
    return r.get("bench") == "menu" and r.get("app_version") != "fake" and r.get("version_code") is not None


def _med(v):
    v = [x for x in v if x is not None]
    return round(statistics.median(v), 2) if v else None


def aggregate(recs, alias, cls, job_id, since=None):
    """menubench.jsonl records (dicts) -> one row per (versionCode, profile) for `alias` newer than `since` (iso ts).
    A rep counts as ok when menubench says ok and its self-checks list is empty. The row is valid with >= 1 ok rep (values = median
    over the ok reps); with none it is invalid and carries the reason."""
    groups = {}
    for r in recs:
        if r.get("alias") != alias or r.get("profile") not in PROFILES or r.get("version_code") is None:
            continue
        if since and _ts(r.get("ts")) < _ts(since):
            continue
        groups.setdefault((r["version_code"], r["profile"]), []).append(r)
    out = []
    for (vc, prof), reps in groups.items():
        good = [r for r in reps if r.get("ok") and not r.get("selfcheck")]
        bad = [r for r in reps if r not in good]
        ref = (good or reps)[-1]
        rd = lambda k: [(r.get("render") or {}).get(k) for r in good]
        row = {"bench": "menu", "phone": alias, "class": cls, "version_code": vc, "app_version": ref.get("app_version"),
               "bridge_version": ref.get("bridge_version"), "profile": prof, "valid": bool(good), "selfcheck_ok": bool(good) and not bad,
               "reps_ok": len(good), "reps_failed": len(bad),
               "avg_fps": _med(rd("avg_fps")), "median_fps": _med(rd("median_fps")), "low1_fps": _med(rd("low1_fps")), "p99_ms": _med(rd("p99_ms")),
               "panel_hz": next((r.get("panel_hz") for r in reversed(good or reps) if r.get("panel_hz")), None),
               "date": (ref.get("ts") or "")[:10], "ts": ref.get("ts"), "job": job_id}
        if not good:
            row["invalid_reason"] = "; ".join(sorted({s for r in bad for s in (r.get("selfcheck") or [r.get("status") or "failed"])}))[:200]
        elif bad:
            row["failed_reps"] = "; ".join(sorted({s for r in bad for s in (r.get("selfcheck") or [r.get("status") or "failed"])}))[:200]
        out.append(row)
    return sorted(out, key=lambda r: (r["version_code"], r["profile"]))


def _ts(s):
    try:
        t = datetime.datetime.fromisoformat(s)
        return t.timestamp() if t.tzinfo else t.replace(tzinfo=datetime.timezone.utc).timestamp()
    except (TypeError, ValueError):
        return 0.0


# ---------- owner in use
def foreground_pkg(out):
    """package of the resumed activity from `dumpsys activity activities` text ('topResumedActivity=ActivityRecord{.. u0 com.x/.Main t12}')"""
    m = re.search(r"(?:topResumedActivity|mResumedActivity|ResumedActivity)[^\n]*?\s(?:u\d+\s+)?([A-Za-z][\w.]*)/", out or "")
    return m.group(1) if m else None


def is_launcher(fg):
    """home-screen package (com.mi.android.globallauncher, com.android.launcher3, com.miui.home ...)"""
    return bool(fg) and ("launcher" in fg.lower() or fg.endswith(".home"))


LAUNCHER_ONLY = "launcher only"


def owner_in_use(power_out, activity_out, pkg=PKG_DEFAULT, launch=False):
    """-> (in_use, why). In use = screen awake with a foreground app that is not MediaDeck (home screen / lock screen count). Unreadable
    output counts as in use: never run blind. launch=True (devices.toml `foreground_launch = true`, our own test phone only): the bare home
    screen is not the owner, the farm brings MediaDeck to the front itself (why starts with LAUNCHER_ONLY); any other app still is."""
    m = re.search(r"mWakefulness=(\w+)", power_out or "")
    if not m:
        return True, "unreadable power state"
    if m.group(1) != "Awake":
        return False, f"screen {m.group(1).lower()}"
    fg = foreground_pkg(activity_out)
    if fg is None:
        return True, "screen on, foreground app unreadable"
    if fg == pkg:
        return False, "MediaDeck in the foreground"
    if launch and is_launcher(fg):
        return False, f"{LAUNCHER_ONLY} ({fg})"
    return True, f"screen on, foreground {fg}"


# ---------- triggers
def installed_vc(out):
    m = re.search(r"versionCode=(\d+)", out or "")
    return int(m.group(1)) if m else None


def should_queue(installed, last_queued, queued_now):
    """installed versionCode differs from the last one a menu run was queued for, and nothing queued now"""
    return installed is not None and not queued_now and installed != last_queued


UNAVAILABLE = ("absent", "missing", "down", "disabled", "unknown")


def available(devs, dstate, disabled):
    """phones to consider for an auto menu run: not opted out (`menu = false` in devices.toml), not disabled by the owner, seen recently.
    Unlike the driver bench this works on shell-route phones too (the menu is the bridge's own session)."""
    return [a for a, d in devs.items() if d.get("menu", True) is not False and a not in disabled and dstate.get(a, {}).get("status", "unknown") not in UNAVAILABLE]


def queued(jobs, alias):
    """an auto/explicit menu job for this phone (pinned to it, or unpinned for its class) still pending or running"""
    for j in jobs:
        if j.get("suite") != "menu" or (j.get("pin") and j["pin"] != alias):
            continue
        if any(p["state"] in ("pending", "running") for p in j["parts"].values()):
            return True
    return False


# ---------- digest and RESULTS.md
def _f(v, p=1):
    return "n/a" if v is None else f"{v:.{p}f}"


def latest_by_phone(rows):
    """-> {phone: (version_code, {profile: row})}: the newest version the phone has rows for (file order)"""
    out = {}
    for r in rows:
        ph, vc = r.get("phone"), r.get("version_code")
        if ph is None or vc is None:
            continue
        if ph not in out or out[ph][0] != vc:
            out[ph] = (vc, {})
        out[ph][1][r.get("profile")] = r
    return out


def digest_lines(rows):
    out = []
    for ph, (vc, profs) in sorted(latest_by_phone(rows).items()):
        seg = []
        for p in PROFILES:
            r = profs.get(p)
            if r is None:
                continue
            if not r.get("valid"):
                seg.append(f"{p} INVALID ({r.get('invalid_reason') or 'self-check failed'})"[:90])
            else:
                seg.append(f"{p} avg {_f(r['avg_fps'])} med {_f(r['median_fps'])} 1%low {_f(r['low1_fps'])} p99 {_f(r['p99_ms'])} ms @{r.get('panel_hz') or '?'} Hz")
        out.append(f"menu  {ph:8} vc {vc}: " + "; ".join(seg))
    return out


def report_lines(rows):
    """the 'Steam menu' section of RESULTS.md: one block per MediaDeck version, newest first (last seen in the file first)"""
    out = ["## Steam menu (realistic metric)", "",
           "Steam Big Picture menu in the DroidDeck guest: scripted D-pad scrolling left/right through the library "
           "(`scripts/quick/menubench` in the MediaDeck repo, run by the farm `menu` suite). Render fps from the web helper's present events over "
           "30 s, medians over the reps that passed the self-checks; `default` = as shipped, `uncapped` = frame limits off. It runs on the Mali bridge, not on "
           "mali-open: numbers change with the MediaDeck build, not with our commits. An **invalid** row (self-check failed) never replaces a valid one.", ""]
    order = []
    for r in rows:
        vc = r.get("version_code")
        if vc in order:
            order.remove(vc)
        order.append(vc)
    if not order:
        return out + ["No menu run yet.", ""]
    for vc in reversed(order):
        blk = [r for r in rows if r.get("version_code") == vc]
        name = next((r["app_version"] for r in reversed(blk) if r.get("app_version")), "")
        out += [f"### MediaDeck vc {vc}{' (' + name + ')' if name else ''} ({max((r.get('date') or '') for r in blk)})", "",
                "| phone | class | profile | avg fps | median fps | 1% low | p99 ms | panel Hz | reps ok/failed | self-checks | date |",
                "|---|---|---|---|---|---|---|---|---|---|---|"]
        for r in sorted(blk, key=lambda x: (x.get("phone") or "", PROFILES.index(x["profile"]) if x.get("profile") in PROFILES else 9)):
            chk = "passed" if r.get("selfcheck_ok") else ("passed (some reps failed)" if r.get("valid") else "INVALID: " + (r.get("invalid_reason") or "failed")[:80])
            out.append(f"| {r['phone']} | {r.get('class') or ''} | {r['profile']} | {_f(r.get('avg_fps'))} | {_f(r.get('median_fps'))} | {_f(r.get('low1_fps'))} | "
                       f"{_f(r.get('p99_ms'), 2)} | {r.get('panel_hz') or 'n/a'} | {r.get('reps_ok', 0)}/{r.get('reps_failed', 0)} | {chk} | {r.get('date') or ''} |")
        out.append("")
    return out
