#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Coverage matrix: which device x stack x version x workload cells have a *valid* displayed
fps point (>= 3 reps, same profile), which ran but produced none, and which are missing.

  tools/bench/gaps.py                write docs/bench-gaps.json, print the next cells to fill
  tools/bench/gaps.py --check        exit 1 if the committed docs/bench-gaps.json is stale
  tools/bench/gaps.py --text         only print the summary

Reads only the strict schema in docs/bench.jsonl (docs/MEASURE.md): `metric` must be
"displayed_fps", `reps` and `panel_hz` must be present, and a point with `reps < 3` is not a
measurement anyone may quote. Nothing here invents a number: a missing cell names the runbook
that would produce one, and a run that produced no visible frame stays "failed".
"""
import json
import sys
from collections import OrderedDict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ROWS = ROOT / "docs/bench.jsonl"
OUT = ROOT / "docs/bench-gaps.json"

DEVICES = OrderedDict([("x8", "Poco X8 Pro Max (v13, G925)"),
                       ("cmf", "CMF Phone 1 (v11, G615)"),
                       ("x4", "POCO X4 (v10, G610; adb 192.168.3.115:5555, model 22041216G)")])
STACKS = OrderedDict([("bridge", "MaliBridge (MediaDeck)"), ("mali-open", "mali-open ICD")])
WORKLOADS = OrderedDict([
    ("cards", "cube/cards (bench2 --bench cube)"),
    ("triangles", "triangles (bench2 --bench triangle)"),
    ("steam-menu", "Steam Big Picture menu (menubench)"),
    ("megabonk", "Megabonk (bench/megabonk-compare.sh)"),
    ("glclear", "glclear readback (mali-gaming kopperglc)"),
])
PANEL_HZ = {"x8": 120, "cmf": 120, "x4": 144}
NEXT = "next"
RUNBOOK = {
    "cards": "tools/bench2 {alias} --bench cube --driver {drv}",
    "triangles": "tools/bench2 {alias} --bench triangle --driver {drv}",
    "steam-menu": "MediaDeck scripts/quick/menubench {alias} --profile default",
    "megabonk": "bench/megabonk-compare.sh run {drv} 90 30",
    "glclear": "mali-gaming kopperglc run against the {drv} ICD",
}
ALIAS = {"x8": "poco-x8", "cmf": "cmf", "x4": "poco-x4"}
BUILD_APK = {"v25": "/tmp/opencode/x8-current-v25.apk"}
UNAVAILABLE = ("p8", "p19", "p22", "p24", "p25", "vc26", "vc27", "vc28")


def load():
    return [json.loads(l) for l in ROWS.read_text().splitlines() if l.strip()]


def valid_point(r):
    """A point that may be plotted and quoted: displayed fps, visible, >= 3 reps, default."""
    return (r.get("metric") == "displayed_fps" and r.get("fps") is not None
            and r.get("status") == "visible" and isinstance(r.get("panel_hz"), (int, float))
            and isinstance(r.get("reps"), int) and r["reps"] >= 3
            and r.get("profile") == "default" and r["fps"] <= r["panel_hz"] + 2)


def build(rows):
    versions = {s: [] for s in STACKS}
    for s in STACKS:
        seen = {}
        for r in sorted(rows, key=lambda r: r["ts"]):
            if r["stack"] == s and r["version"] not in ("unknown",) and r["version"] not in seen:
                seen[r["version"]] = r["ts"]
        versions[s] = list(seen) + [NEXT]

    cells, series = [], {}
    unattributed = 0
    for r in rows:
        if r.get("version") in (None, "unknown"):
            unattributed += 1
        if r["device"] not in DEVICES or r["stack"] not in STACKS:
            continue
        k = (r["device"], r["stack"], r["workload"])
        s = series.setdefault(k, {"points": 0, "valid": 0, "hollow": 0, "last": None})
        s["points"] += 1
        if valid_point(r):
            s["valid"] += 1
            s["last"] = r["version"]
        else:
            s["hollow"] += 1

    for dev in DEVICES:
        for st in STACKS:
            for ver in versions[st]:
                for wl in WORKLOADS:
                    exact = [r for r in rows if r["device"] == dev and r["stack"] == st
                             and r["version"] == ver and r["workload"] == wl]
                    val = [r for r in exact if valid_point(r)]
                    if val:
                        state = "measured"
                    elif exact:
                        state = "failed"
                    else:
                        state = "missing"
                    has_series = series.get((dev, st, wl), {}).get("points", 0) > 0
                    if ver == NEXT:
                        value = 100 if has_series else 50
                        why = ("moves a line's endpoint (series has data)" if has_series
                               else "starts a line (no data for this series yet)")
                    elif state == "missing" and not has_series:
                        value, why = 40, "starts a line (no data for this series yet)"
                    elif state == "missing":
                        value, why = 10, "another version of an existing series"
                    elif state == "failed":
                        value, why = 20, "a run exists but has no valid displayed-fps point"
                    else:
                        value, why = 0, "measured"
                    cell = {"device": dev, "stack": st, "version": ver, "workload": wl,
                            "state": state, "value": value, "why": why,
                            "series_valid": series.get((dev, st, wl), {}).get("valid", 0)}
                    if state != "missing":
                        cell["points"] = len(exact)
                        cell["valid_points"] = len(val)
                    else:
                        if st == "bridge":
                            apk_path = BUILD_APK.get(ver)
                            if apk_path and Path(apk_path).is_file():
                                apk, avail = f"{ver} APK at {apk_path}", True
                            elif ver == NEXT:
                                apk, avail = "next MediaDeck build (vc 28, unavailable on this host)", False
                            elif ver in UNAVAILABLE:
                                apk, avail = f"MediaDeck {ver} APK (unavailable on this host)", False
                            else:
                                apk, avail = f"MediaDeck {ver} (bridge 0.5.5.4)", False
                        else:
                            apk = "next mali-open commit" if ver == NEXT else f"mali-open ICD at {ver}"
                            avail = ver != NEXT
                        cell["runbook"] = {
                            "device": DEVICES[dev], "apk_or_driver": apk, "driver": STACKS[st],
                            "test": RUNBOOK[wl].format(alias=ALIAS[dev],
                                                       drv="bridge" if st == "bridge" else "ours"),
                            "build_available": avail,
                        }
                    cells.append(cell)

    missing = [c for c in cells if c["state"] == "missing"]
    missing.sort(key=lambda c: (-c["value"], c["series_valid"],
                                list(WORKLOADS).index(c["workload"]),
                                list(DEVICES).index(c["device"]),
                                list(STACKS).index(c["stack"]), c["version"]))
    counts = {s: sum(1 for c in cells if c["state"] == s)
              for s in ("measured", "failed", "missing")}
    per_workload = OrderedDict()
    for wl in WORKLOADS:
        wrows = [r for r in rows if r["workload"] == wl]
        per_workload[wl] = {
            "rows": len(wrows),
            "valid_points": sum(1 for r in wrows if valid_point(r)),
            "best_eligible": sorted({(r["device"], r["stack"], r["version"], r["profile"])
                                     for r in wrows if valid_point(r)}),
        }
    return {
        "source": "docs/bench.jsonl",
        "metric": "displayed_fps",
        "rules": "docs/MEASURE.md: displayed fps only, default profile, >=3 reps, fps <= panel_hz+2",
        "devices": list(DEVICES), "stacks": list(STACKS), "workloads": list(WORKLOADS),
        "panel_hz": PANEL_HZ,
        "versions": versions,
        "counts": dict(counts, cells=len(cells), unattributed_rows=unattributed),
        "per_workload": per_workload,
        "next": missing[:8],
        "cells": cells,
        "series": [dict(device=k[0], stack=k[1], workload=k[2], **v) for k, v in
                   sorted(series.items(), key=lambda kv: (list(WORKLOADS).index(kv[0][2]),
                                                          list(DEVICES).index(kv[0][0]),
                                                          list(STACKS).index(kv[0][1])))],
    }


def summary(g):
    c = g["counts"]
    out = [f"bench gaps: {c['measured']} measured, {c['failed']} failed, {c['missing']} missing "
           f"of {c['cells']} cells"]
    out.append("valid displayed-fps points per workload: " + ", ".join(
        f"{wl}={v['valid_points']}" for wl, v in g["per_workload"].items()))
    out.append("next cells to fill (value first):")
    for i, cell in enumerate(g["next"][:5], 1):
        out.append(f"  {i}. {cell['workload']}/{cell['device']}/{cell['stack']}/{cell['version']}"
                   f" — {cell['why']}")
        rb = cell.get("runbook") or {}
        if rb:
            out.append(f"     {rb['test']}  [{rb['apk_or_driver']}]")
    return "\n".join(out)


def main():
    check = "--check" in sys.argv[1:]
    g = build(load())
    blob = json.dumps(g, indent=1, sort_keys=False) + "\n"
    msg = summary(g)
    if "--text" in sys.argv[1:]:
        print(msg)
        return 0
    if check:
        if (OUT.read_text() if OUT.is_file() else "") != blob:
            print("bench gaps: docs/bench-gaps.json is stale; run make bench", file=sys.stderr)
            return 1
        print(msg)
        return 0
    OUT.write_text(blob)
    print(msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
