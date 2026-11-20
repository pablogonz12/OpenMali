#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Normalise the raw benchmark logs into docs/bench.jsonl: displayed fps only.

    tools/bench/import.py            rewrite docs/bench.jsonl and docs/rejected.jsonl
    tools/bench/import.py --check    exit 1 if either committed file would change

The store's contract (see docs/MEASURE.md):

  fps        displayed frames per second the user could see: menubench `screen.fps`
             (SurfaceFlinger present timestamps). Render-side and producer-side rates
             are stored in `render_fps` / `presented_fps` and are never plotted,
             compared or used for "best".
  metric     always "displayed_fps".
  profile    "default" | "uncapped" | other. Uncapped runs are skipped: not comparable.
  panel_hz   the panel's refresh rate at the time; fps > panel_hz + 2 is instrument
             error (rejected, never clamped).
  reps       number of visible reps in the same run group (device/stack/version/
             workload/profile/panel/day). "best" needs >= 3.

A row is skipped, never guessed, when `ok != true`, `profile == "uncapped"`, there is
no displayed-fps source, or fps exceeds the panel. Every skipped source row lands in
docs/rejected.jsonl with its reason and the value that was observed.

Python 3.11+, stdlib only, deterministic.
"""
import json
import os
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "docs/bench.jsonl"
REJ = ROOT / "docs/rejected.jsonl"
MEDIADECK = Path(os.environ.get("MEDIADECK", Path.home() / "Documents/scripts/MediaDeck"))
MENUBENCH = MEDIADECK / "results/menubench.jsonl"
BENCH2 = ROOT / "bench/results.jsonl"
MENU = ROOT / "bench/menu.jsonl"
# Driver-mode WSI smoke lanes (vkcube under wayland / xcb), one line per rep. These are the only
# source of `stack: mali-open` displayed-fps points: no APK, no menubench, just the driver.
DRIVER = ROOT / "bench/driver.jsonl"
# Offscreen producer rates (t_bench_gpu / t_bench_cpu). They are `metric: render_fps`, a
# different kind from displayed_fps: never plotted as a displayed rate, never compared to a
# panel, and stored in their own file so the two kinds cannot be confused at the source.
OFFSCREEN = ROOT / "bench/offscreen.jsonl"
# The archived seed and the owner's hand-typed rows are kept as evidence only: a line in
# FINDINGS.md or an owner note is not a measurement and cannot carry panel_hz or reps.
SEED = ROOT / "docs/bench-archive/2026-11-20-invalid/bench-seed.jsonl"

DEVICE = {"poco-x8": "x8", "x8": "x8", "cmf": "cmf", "poco-x4": "x4", "x4": "x4"}
STACK = {"bridge": "bridge", "ours": "mali-open", "mali-open": "mali-open"}
WORKLOAD = {"triangle": "triangles", "cube": "cards", "menu": "steam-menu",
            "menubench": "steam-menu"}
# Driver-mode lanes: vkcube WSI smoke, plus the Steam menu as our driver renders it.
DRIVER_WORKLOADS = ("vkcube-wayland", "vkcube-xcb", "steam-menu")
PANEL = {"x8": 120, "cmf": 120, "x4": 144}  # fallback only when the log omits panel_hz
FIELDS = ("ts", "device", "stack", "version", "driver_sha", "workload", "metric", "profile",
          "fps", "panel_hz", "reps", "brightness_mean", "status", "source",
          "render_fps", "presented_fps", "off_cadence_pct", "run",
          "temp_c", "cpu_idle_pct", "gpu_busy_pct", "note")


def num_or_none(v):
    return round(float(v), 1) if isinstance(v, (int, float)) and not isinstance(v, bool) else None


def read_jsonl(path):
    if not Path(path).is_file():
        return []
    out = []
    for line in Path(path).read_text().splitlines():
        if line.strip():
            out.append(json.loads(line))
    return out


def p_version(text):
    """'malideck p25 (...)' -> v25; '0.7.1 (vc 27)' -> vc27; 'p22 ...' -> p22."""
    t = str(text)
    if re.search(r"\bp25\b|\bvc\s*25\b", t):
        return "v25"
    m = re.search(r"p(\d+)\b", t)
    if m:
        return "p" + m.group(1)
    m = re.search(r"\bvc\s*(\d+)\b", t)
    if m:
        return "vc" + m.group(1)
    m = re.search(r"\b(v\d+)\b", t)
    return m.group(1) if m else "unknown"


def mali_version(text):
    t = str(text or "").split("-dirty")[0]
    return t[:9] if re.fullmatch(r"[0-9a-f]{7,40}", t) else "unknown"


def commit_of(text):
    """Short driver hash when the build string names one, else None."""
    m = re.search(r"\b(?:at|commit)\s+([0-9a-f]{7,40})", str(text or ""))
    return m.group(1)[:9] if m else None


def day(ts, date):
    return ts or (date or "1970-01-01") + "T12:00:00+01:00"


class Sink:
    def __init__(self):
        self.rows = []
        self.rejected = []

    def reject(self, reason, detail, r, **extra):
        self.rejected.append({
            "ts": r.get("ts") or r.get("date"),
            "device": extra.pop("device", None) or r.get("device") or r.get("alias") or r.get("phone"),
            "workload": extra.pop("workload", None) or r.get("workload") or r.get("bench"),
            "reason": reason,
            "detail": detail,
            "observed_fps": extra.get("observed_fps"),
            "panel_hz": extra.get("panel_hz"),
            "source": extra.get("source") or "",
        })


def from_menubench(sink):
    """menubench: screen.fps is displayed, render.avg_fps is the render producer."""
    for r in read_jsonl(MENUBENCH):
        dev = DEVICE.get(r.get("alias"))
        panel = r.get("panel_hz")
        panel = round(float(panel)) if panel is not None else PANEL.get(dev)
        src = f"menubench ({r.get('ts')}, scenario={r.get('scenario')}, rep={r.get('rep')})"
        if dev is None:
            sink.reject("unknown_device", f"alias={r.get('alias')}", r, source=src,
                        workload="steam-menu")
            continue
        if not r.get("ok"):
            sink.reject("not_ok", str(r.get("status"))[:120], r, source=src, panel_hz=panel,
                        workload="steam-menu")
            continue
        if str(r.get("profile")) == "uncapped":
            scr = r.get("screen") or {}
            ren = (r.get("render") or {}).get("avg_fps")
            sink.reject("uncapped_profile",
                        f"not comparable with default; screen.fps={scr.get('fps')}, "
                        f"render.avg_fps={ren}", r, source=src,
                        observed_fps=scr.get("fps"), panel_hz=panel, workload="steam-menu")
            continue
        scr = r.get("screen") or {}
        fps = scr.get("fps")
        if not isinstance(fps, (int, float)):
            sink.reject("no_displayed_fps", "screen.fps absent", r, source=src, panel_hz=panel,
                        workload="steam-menu")
            continue
        if panel is None:
            sink.reject("no_panel_hz", "panel_hz absent; ceiling not checkable", r, source=src,
                        workload="steam-menu")
            continue
        if fps > panel + 2:
            sink.reject("above_panel_ceiling", f"{fps} fps on a {panel} Hz panel",
                        r, source=src, observed_fps=fps, panel_hz=panel, workload="steam-menu")
            continue
        b = r.get("brightness_mean")
        status = "black" if (fps < 5 or (isinstance(b, (int, float)) and b <= 20)) else "visible"
        sink.rows.append({
            "ts": day(r.get("ts"), None),
            "device": dev,
            "stack": "bridge",
            "version": p_version(f"{r.get('app_version') or ''} (vc {r.get('version_code')})"),
            "driver_sha": commit_of(r.get("build")),
            "workload": "steam-menu",
            "metric": "displayed_fps",
            "profile": str(r.get("profile") or "unknown"),
            "fps": round(float(fps), 2),
            "panel_hz": panel,
            "reps": 1,
            "brightness_mean": b if isinstance(b, (int, float)) else None,
            "status": status,
            "source": src,
            "render_fps": round(float((r.get("render") or {}).get("avg_fps")), 2)
                          if (r.get("render") or {}).get("avg_fps") is not None else None,
            "presented_fps": None,
            "note": f"screen.fps; render.avg_fps={((r.get('render') or {}).get('avg_fps'))} "
                    f"is render-side and not used",
        })


def from_bench2(sink):
    """bench/results.jsonl: presented.fps is the app's present rate, not a displayed fps."""
    for r in read_jsonl(BENCH2):
        sink.reject("presented_fps_not_displayed",
                    f"presented.fps={(r.get('presented') or {}).get('fps')} is producer-side",
                    {"ts": (r.get("date") or "") + "T00:00:00",
                     "device": DEVICE.get(r.get("phone"), r.get("phone")),
                     "workload": WORKLOAD.get(r.get("bench"), r.get("bench"))},
                    observed_fps=(r.get("presented") or {}).get("fps"),
                    source=f"bench/results.jsonl ({r.get('bench')}/{r.get('variant')})")


def from_menu(sink):
    """bench/menu.jsonl: avg_fps is the render average, not screen.fps."""
    for r in read_jsonl(MENU):
        sink.reject("render_avg_not_displayed",
                    f"avg_fps={r.get('avg_fps')} is render-side; no screen.fps in this file",
                    {"ts": day(r.get("ts"), r.get("date")),
                     "device": DEVICE.get(r.get("phone"), r.get("phone")),
                     "workload": "steam-menu"},
                    observed_fps=r.get("avg_fps"), panel_hz=r.get("panel_hz"),
                    source=f"bench/menu.jsonl ({r.get('date')})")


def from_driver(sink):
    """bench/driver.jsonl: driver-mode vkcube runs. One line per rep, displayed fps on screen.

    The lane has no APK and no SurfaceFlinger trace: the displayed rate is the number the
    vkcube present counter shows on the panel, with diagnostics env cleared. The importer
    applies the same contract as every other source (named displayed-fps source, default
    profile, panel ceiling) and never invents a rep: `reps` is recomputed from the visible
    reps that actually exist, so a lane that only delivered 2 usable reps stays at reps=2.
    """
    for r in read_jsonl(DRIVER):
        dev = DEVICE.get(r.get("device"))
        wl = r.get("workload")
        panel = r.get("panel_hz")
        src = str(r.get("source") or "")
        if dev is None:
            sink.reject("unknown_device", f"device={r.get('device')}", r, source=src)
            continue
        if wl not in DRIVER_WORKLOADS:
            sink.reject("unknown_workload", f"workload={wl} is not a WSI smoke lane", r,
                        source=src, workload=wl)
            continue
        if str(r.get("profile")) == "uncapped":
            sink.reject("uncapped_profile", "not comparable with default", r, source=src,
                        observed_fps=r.get("fps"), panel_hz=panel, workload=wl)
            continue
        fps = r.get("fps")
        if not isinstance(fps, (int, float)):
            sink.reject("no_displayed_fps", "fps absent", r, source=src, panel_hz=panel,
                        workload=wl)
            continue
        panel = round(float(panel)) if panel is not None else PANEL.get(dev)
        if panel is None:
            sink.reject("no_panel_hz", "panel_hz absent; ceiling not checkable", r, source=src,
                        workload=wl)
            continue
        if fps > panel + 2:
            sink.reject("above_panel_ceiling", f"{fps} fps on a {panel} Hz panel",
                        r, source=src, observed_fps=fps, panel_hz=panel, workload=wl)
            continue
        b = r.get("brightness_mean")
        if r.get("status") in ("visible", "black", "crash", "not_run", "unverified"):
            status = str(r.get("status"))
        else:
            status = "black" if (fps < 5 or (isinstance(b, (int, float)) and b <= 20)) else "visible"
        sink.rows.append({
            "ts": day(r.get("ts"), None),
            "device": dev,
            "stack": "mali-open" if str(r.get("stack")) != "bridge" else "bridge",
            "version": str(r.get("version") or (
                "unknown" if str(r.get("stack")) == "bridge" else
                mali_version(r.get("driver_sha")))),
            # A bridge row runs the Arm proprietary driver; there is no mali-open sha to record.
            "driver_sha": (None if str(r.get("stack")) == "bridge"
                           else mali_version(r.get("driver_sha"))),
            "workload": wl,
            "metric": "displayed_fps",
            "profile": str(r.get("profile") or "unknown"),
            "fps": round(float(fps), 2),
            "panel_hz": panel,
            "reps": 1,
            "brightness_mean": b if isinstance(b, (int, float)) else None,
            "status": status,
            "source": src,
            "render_fps": None,
            "presented_fps": None,
            "off_cadence_pct": (round(float(r["off_cadence_pct"]), 2)
                                if isinstance(r.get("off_cadence_pct"), (int, float)) else None),
            "run": str(r.get("run") or ""),
            # System load, when the lane measured it. null = not measured (printed "n/a"),
            # never 0, which would read as "measured and idle".
            "temp_c": num_or_none(r.get("temp_c")),
            "cpu_idle_pct": num_or_none(r.get("cpu_idle_pct")),
            "gpu_busy_pct": num_or_none(r.get("gpu_busy_pct")),
            "note": str(r.get("note") or ""),
        })


def from_offscreen(sink):
    """bench/offscreen.jsonl: offscreen producer rates. stored as render_fps, never displayed.

    There is no panel in front of an offscreen draw, so `panel_hz` stays null and no ceiling
    applies: 680 fps on a 120 Hz phone is not instrument error, it is a different metric.
    """
    for r in read_jsonl(OFFSCREEN):
        dev = DEVICE.get(r.get("device"))
        wl = str(r.get("workload") or "")
        src = str(r.get("source") or "")
        if dev is None:
            sink.reject("unknown_device", f"device={r.get('device')}", r, source=src)
            continue
        if str(r.get("profile")) == "uncapped":
            sink.reject("uncapped_profile", "not comparable with default", r, source=src,
                        observed_fps=r.get("fps"), workload=wl)
            continue
        fps = r.get("fps")
        if not isinstance(fps, (int, float)):
            sink.reject("no_displayed_fps", "offscreen fps absent", r, source=src, workload=wl)
            continue
        sink.rows.append({
            "ts": day(r.get("ts"), None),
            "device": dev,
            "stack": "bridge" if str(r.get("stack")) == "bridge" else "mali-open",
            "version": str(r.get("version") or mali_version(r.get("driver_sha"))),
            "driver_sha": (None if str(r.get("stack")) == "bridge"
                           else mali_version(r.get("driver_sha"))),
            "workload": wl,
            "metric": "render_fps",
            "profile": str(r.get("profile") or "unknown"),
            "fps": round(float(fps), 2),
            "panel_hz": None,          # offscreen: there is no panel to cap this
            "reps": 1,
            "brightness_mean": None,   # nothing on screen, so nothing to verify
            "status": str(r.get("status") or "visible"),
            "source": src,
            "render_fps": round(float(fps), 2),
            "presented_fps": None,
            "off_cadence_pct": None,
            "run": str(r.get("run") or ""),
            "temp_c": num_or_none(r.get("temp_c")),
            "cpu_idle_pct": None,
            "gpu_busy_pct": None,
            "note": str(r.get("note") or ""),
        })


def from_seed(sink):
    """FINDINGS.md / owner notes: a claim with no displayed-fps source, panel_hz or reps."""
    for r in read_jsonl(SEED):
        sink.reject("doc_claim_not_measurable",
                    "no screen.fps, no panel_hz, no reps; re-run under docs/MEASURE.md",
                    r, observed_fps=r.get("fps"), source=str(r.get("source"))[:120])


def assign_reps(rows):
    """reps = visible reps in the same (device, stack, version, workload, profile, panel, day, run).

    Two separate sessions of one build on one day are two groups: a run that replaces an earlier
    one (same sha, better pacing) does not silently turn into a 6-rep group and move the median.
    """
    groups = {}
    for r in rows:
        key = (r["device"], r["stack"], r["version"], r["workload"], r["profile"],
               r["panel_hz"], r["ts"][:10], r.get("run") or "")
        groups.setdefault(key, []).append(r)
    for group in groups.values():
        visible = sum(1 for r in group if r["status"] == "visible" and r["fps"] and r["fps"] > 0)
        for r in group:
            r["reps"] = visible
    return rows


def build():
    sink = Sink()
    from_menubench(sink)
    from_bench2(sink)
    from_menu(sink)
    from_driver(sink)
    from_offscreen(sink)
    from_seed(sink)
    rows = assign_reps(sink.rows)
    rows.sort(key=lambda r: (r["ts"], r["device"], r["stack"], r["workload"], r["version"],
                             r["fps"], r["source"]))
    seen, out = set(), []
    for r in rows:
        key = (r["ts"], r["device"], r["stack"], r["version"], r["workload"], r["profile"],
               round(r["fps"], 2))
        if key in seen:
            continue
        seen.add(key)
        out.append({k: r.get(k) for k in FIELDS})
    rej = sorted(sink.rejected, key=lambda r: (str(r["ts"]), str(r["device"]),
                                               str(r["workload"]), r["reason"]))
    return out, rej


def main():
    check = "--check" in sys.argv[1:]
    rows, rejected = build()
    text = "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in rows)
    rtext = "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in rejected)
    if check:
        stale = []
        if (OUT.read_text() if OUT.is_file() else "") != text:
            stale.append(str(OUT.relative_to(ROOT)))
        if (REJ.read_text() if REJ.is_file() else "") != rtext:
            stale.append(str(REJ.relative_to(ROOT)))
        if stale:
            print("bench: stale: " + ", ".join(stale) + "; run make bench", file=sys.stderr)
            return 1
        print(f"bench: {OUT.relative_to(ROOT)} and {REJ.relative_to(ROOT)} up to date "
              f"({len(rows)} accepted, {len(rejected)} rejected)")
        return 0
    OUT.write_text(text)
    REJ.write_text(rtext)
    counts = Counter(r["reason"] for r in rejected)
    n_disp = sum(1 for r in rows if r["metric"] == "displayed_fps")
    n_rend = len(rows) - n_disp
    print(f"bench: wrote {OUT.relative_to(ROOT)} ({n_disp} displayed-fps, {n_rend} render_fps "
          f"rows) and "
          f"{REJ.relative_to(ROOT)} ({len(rejected)} rejected)")
    for reason, n in counts.most_common():
        print(f"bench:   rejected {n:4d} {reason}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
