#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Normalise the benchmark history into docs/bench.jsonl.

  tools/bench/import.py            write docs/bench.jsonl (idempotent, deterministic)
  tools/bench/import.py --check    exit 1 if the committed docs/bench.jsonl is stale

Sources (read-only; nothing outside this repo is modified):
  docs/bench-seed.jsonl                      hand-curated rows, every one with a dated source
  bench/results.jsonl                        bench2 triangle/cube rows (tools/bench2 --report)
  bench/menu.jsonl                           menubench Steam-menu rows (mali-open copy)
  $MEDIADECK/results/menubench.jsonl         menubench's own append log (default
                                             ~/Documents/scripts/MediaDeck/results)

One output object per run:
  {ts, device, stack, version, workload, fps, brightness_mean, status, source[, note]}

`status` is visible|black|crash|not_run|unverified. A row with no fps is written with fps 0 and a
status that says why; nothing is interpolated, and a field the source does not carry is null or
"unknown" rather than a guess.
"""
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "docs/bench.jsonl"
SEED = ROOT / "docs/bench-seed.jsonl"
MEDIADECK = Path(os.environ.get("MEDIADECK", Path.home() / "Documents/scripts/MediaDeck"))
SOURCES = [SEED, ROOT / "bench/results.jsonl", ROOT / "bench/menu.jsonl",
           MEDIADECK / "results/menubench.jsonl"]

DEVICE = {"poco-x8": "x8", "x8": "x8", "cmf": "cmf", "poco-x4": "x4", "x4": "x4"}
STACK = {"bridge": "bridge", "ours": "mali-open", "mali-open": "mali-open"}
WORKLOAD = {"triangle": "triangles", "cube": "cards", "menu": "steam-menu", "menubench": "steam-menu"}


def p_version(text):
    """'malideck p25 (...)' -> v25; '0.7.1 (vc 27)' -> vc27; '1.0.0-p22-...' -> p22.

    The p25 APK is the MediaDeck 1.0.0 build that installs as versionCode 25. Its APK was pulled
    from the X8 before the update (/tmp/opencode/x8-current-v25.apk), so that build is named v25 and
    its historical rows stay attributable now that the original p25 file is gone.
    """
    t = str(text)
    if re.search(r"\bp25\b|\bvc\s*25\b", t):
        return "v25"
    m = re.search(r"p(\d+)\b", t)
    if m:
        return "p" + m.group(1)
    m = re.search(r"\bvc\s*(\d+)\b", str(text))
    if m:
        return "vc" + m.group(1)
    m = re.search(r"\b(v\d+)\b", str(text))
    return m.group(1) if m else "unknown"


def mali_version(text):
    """A driver commit stays as its short hash; anything else is not a version we can name."""
    t = str(text or "").split("-dirty")[0]
    return t[:9] if re.fullmatch(r"[0-9a-f]{7,40}", t) else "unknown"


def day(ts, date):
    if ts:
        return ts
    return (date or "1970-01-01") + "T12:00:00+01:00"


def from_seed():
    if not SEED.is_file():
        return []
    return [json.loads(l) for l in SEED.read_text().splitlines() if l.strip()]


def from_bench2():
    path = ROOT / "bench/results.jsonl"
    rows = []
    if not path.is_file():
        return rows
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        r = json.loads(line)
        dev = DEVICE.get(r.get("phone"))
        pres = r.get("presented") or {}
        fps = pres.get("fps")
        if dev is None:
            continue  # unknown phone: keep it out of the store rather than guessing a device
        rows.append({
            "ts": day(None, r.get("date")),
            "device": dev,
            "stack": STACK.get(r.get("driver"), "unknown"),
            "version": p_version(r.get("version")) if r.get("driver") == "bridge"
                       else mali_version(r.get("version")),
            "workload": WORKLOAD.get(r.get("bench"), r.get("bench") or "unknown"),
            "fps": float(fps) if fps else 0.0,
            "brightness_mean": None,
            "status": "visible" if fps else "unverified",
            "source": f"bench/results.jsonl ({r.get('bench')}/{r.get('variant')}, {r.get('date')}, "
                      f"presented.fps; date-only, no time)",
            "note": r.get("variant"),
        })
    return rows


def from_menu():
    path = ROOT / "bench/menu.jsonl"
    rows = []
    if not path.is_file():
        return rows
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        r = json.loads(line)
        dev = DEVICE.get(r.get("phone"))
        fps = r.get("avg_fps")
        ok = bool(r.get("valid")) and bool(fps)
        rows.append({
            "ts": day(r.get("ts"), r.get("date")),
            "device": dev or "unknown",
            "stack": "bridge",
            "version": p_version(f"{r.get('app_version') or ''} (vc {r.get('version_code')})"),
            "workload": "steam-menu",
            "fps": float(fps) if fps else 0.0,
            "brightness_mean": None,
            "status": "visible" if ok else "unverified",
            "source": f"bench/menu.jsonl ({r.get('date')}, profile={r.get('profile')}, "
                      f"avg_fps=render avg; invalid_reason={r.get('invalid_reason')})",
            "note": "profile=" + str(r.get("profile")),
        })
    return rows


def from_menubench():
    path = MEDIADECK / "results/menubench.jsonl"
    rows = []
    if not path.is_file():
        return rows
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        r = json.loads(line)
        dev = DEVICE.get(r.get("alias"))
        if dev is None:
            continue
        ren = r.get("render") or {}
        fps = ren.get("avg_fps")
        ok = bool(r.get("ok")) and bool(fps)
        if ok:
            status = "visible"
        elif "FAILED" in str(r.get("status")) or "crash" in str(r.get("status")).lower():
            status = "crash"
        else:
            status = "not_run"
        rows.append({
            "ts": day(r.get("ts"), None),
            "device": dev,
            "stack": "bridge",
            "version": p_version(f"{r.get('app_version') or ''} (vc {r.get('version_code')})"),
            "workload": "steam-menu",
            "fps": float(fps) if fps else 0.0,
            "brightness_mean": None,
            "status": status,
            "source": f"{path} ({r.get('ts')}, profile={r.get('profile')}, "
                      f"status={r.get('status')})",
            "note": "profile=" + str(r.get("profile")) + ("" if ok else "; " + str(r.get("status"))),
        })
    return rows


def main():
    check = "--check" in sys.argv[1:]
    rows = []
    for loader in (from_seed, from_bench2, from_menu, from_menubench):
        rows.extend(loader())
    # Stable order, then de-duplicate identical measurements (both logs can record the same run).
    rows.sort(key=lambda r: (r["ts"], r["device"], r["stack"], r["workload"], r["version"],
                             r["fps"], r["source"]))
    seen, out = set(), []
    for r in rows:
        key = (r["ts"], r["device"], r["stack"], r["version"], r["workload"], round(r["fps"], 3))
        if key in seen:
            continue
        seen.add(key)
        out.append({k: r.get(k) for k in ("ts", "device", "stack", "version", "workload", "fps",
                                          "brightness_mean", "status", "source", "note")
                    if r.get(k) is not None or k in ("brightness_mean",)})
    text = "".join(json.dumps(r, sort_keys=False, ensure_ascii=False) + "\n" for r in out)
    if check:
        cur = OUT.read_text() if OUT.is_file() else ""
        if cur != text:
            print(f"bench: {OUT.relative_to(ROOT)} is stale; run make bench", file=sys.stderr)
            return 1
        print(f"bench: {OUT.relative_to(ROOT)} up to date ({len(out)} rows)")
        return 0
    OUT.write_text(text)
    print(f"bench: wrote {OUT.relative_to(ROOT)} ({len(out)} rows from {len(SOURCES)} sources)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
