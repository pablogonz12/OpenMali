#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Coverage matrix for the benchmark charts: which device x stack x version x workload cells are
measured, failed, or still missing, and what to run to fill each missing one.

  tools/bench/gaps.py                write docs/bench-gaps.json, print the next cells to fill
  tools/bench/gaps.py --check        exit 1 if the committed docs/bench-gaps.json is stale
  tools/bench/gaps.py --text         only print the summary (used by make bench)

`missing` is the honest state until a real run exists: nothing here invents a number, a runbook
only says which build, which test and which device would produce one.
"""
import json
import sys
from collections import OrderedDict
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ROWS = ROOT / "docs/bench.jsonl"
OUT = ROOT / "docs/bench-gaps.json"

DEVICES = OrderedDict([("x8", "Poco X8 Pro Max (v13, G925)"), ("cmf", "CMF Phone 1 (v11, G615)")])
STACKS = OrderedDict([("bridge", "MaliBridge (MediaDeck)"), ("mali-open", "mali-open ICD")])
WORKLOADS = OrderedDict([
    ("cards", "cube/cards (bench2 --bench cube)"),
    ("triangles", "triangles (bench2 --bench triangle)"),
    ("steam-menu", "Steam Big Picture menu (menubench)"),
    ("megabonk", "Megabonk (bench/megabonk-compare.sh)"),
    ("glclear", "glclear readback (mali-gaming kopperglc)"),
])
NEXT = "next"
RUNBOOK = {
    "cards": "tools/bench2 {alias} --bench cube --driver {drv}",
    "triangles": "tools/bench2 {alias} --bench triangle --driver {drv}",
    "steam-menu": "MediaDeck scripts/quick/menubench {alias} --profile default",
    "megabonk": "bench/megabonk-compare.sh run {drv} 90 30",
    "glclear": "mali-gaming kopperglc run against the {drv} ICD",
}
ALIAS = {"x8": "poco-x8", "cmf": "cmf"}


def load():
    return [json.loads(l) for l in ROWS.read_text().splitlines() if l.strip()]


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
        if r["version"] == "unknown":
            unattributed += 1
        if r["device"] not in DEVICES or r["stack"] not in STACKS:
            continue
        k = (r["device"], r["stack"], r["workload"])
        s = series.setdefault(k, {"points": 0, "visible": 0, "hollow": 0, "last": None})
        s["points"] += 1
        if r["status"] == "visible" and r["fps"] > 0:
            s["visible"] += 1
            s["last"] = r["version"]
        else:
            s["hollow"] += 1

    for dev in DEVICES:
        for st in STACKS:
            for ver in versions[st]:
                for wl in WORKLOADS:
                    exact = [r for r in rows if r["device"] == dev and r["stack"] == st
                             and r["version"] == ver and r["workload"] == wl]
                    vis = [r for r in exact if r["status"] == "visible" and r["fps"] > 0]
                    if vis:
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
                        value = 40
                        why = "starts a line (no data for this series yet)"
                    elif state == "missing":
                        value = 10
                        why = "another version of an existing series"
                    elif state == "failed":
                        value = 20
                        why = "a run exists but no visible frame was measured"
                    else:
                        value = 0
                        why = "measured"
                    if state == "missing":
                        apk = ("next MediaDeck build (vc 28, bridge 0.5.5.4)" if st == "bridge" and ver == NEXT
                               else f"MediaDeck {ver} (bridge 0.5.5.4)" if st == "bridge"
                               else "next mali-open commit" if ver == NEXT
                               else f"mali-open ICD at {ver}")
                        cells.append({
                            "device": dev, "stack": st, "version": ver, "workload": wl,
                            "state": state, "value": value, "why": why,
                            "series_visible": series.get((dev, st, wl), {}).get("visible", 0),
                            "runbook": {
                                "device": DEVICES[dev],
                                "apk_or_driver": apk,
                                "driver": STACKS[st],
                                "test": RUNBOOK[wl].format(alias=ALIAS[dev],
                                                           drv="bridge" if st == "bridge" else "ours"),
                            },
                        })
                    else:
                        cells.append({"device": dev, "stack": st, "version": ver, "workload": wl,
                                      "state": state, "value": value, "why": why,
                                      "series_visible": series.get((dev, st, wl), {}).get("visible", 0),
                                      "points": len(exact)})

    missing = [c for c in cells if c["state"] == "missing"]
    missing.sort(key=lambda c: (-c["value"], c["series_visible"], list(WORKLOADS).index(c["workload"]),
                                list(DEVICES).index(c["device"]), list(STACKS).index(c["stack"]),
                                c["version"]))
    counts = {s: sum(1 for c in cells if c["state"] == s)
              for s in ("measured", "failed", "missing")}
    return {
        "source": "docs/bench.jsonl",
        "devices": list(DEVICES), "stacks": list(STACKS), "workloads": list(WORKLOADS),
        "versions": versions,
        "counts": dict(counts, cells=len(cells), unattributed_rows=unattributed),
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
           f"of {c['cells']} cells ({c['unattributed_rows']} rows with no device/version)"]
    out.append("next cells to fill (value first):")
    for i, cell in enumerate(g["next"][:5], 1):
        out.append(f"  {i}. {cell['workload']}/{cell['device']}/{cell['stack']}/{cell['version']}"
                   f" — {cell['why']}")
        out.append(f"     {cell['runbook']['test']}  [{cell['runbook']['apk_or_driver']}]")
    return "\n".join(out)


def main():
    check = "--check" in sys.argv[1:]
    text = "--text" in sys.argv[1:]
    g = build(load())
    blob = json.dumps(g, indent=1, sort_keys=False) + "\n"
    msg = summary(g)
    if text:
        print(msg)
        return 0
    if check:
        cur = OUT.read_text() if OUT.is_file() else ""
        if cur != blob:
            print("bench gaps: docs/bench-gaps.json is stale; run make bench", file=sys.stderr)
            return 1
        print(msg)
        return 0
    OUT.write_text(blob)
    print(msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
