#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Benchmark charts: one inline-SVG chart per workload in docs/BENCH.md.

  tools/bench/charts.py            write docs/BENCH.md (and docs/bench-gaps.json via gaps.py)
  tools/bench/charts.py --check    exit 1 if the committed docs/BENCH.md would change
  tools/bench/charts.py --gaps     only print the coverage summary

Four series per chart: colour is the stack (bridge = blue, mali-open = red), line style is the
device (X8 solid, CMF dashed). X is real time; every point carries its version label. A run with
no visible frame plots at fps 0 with a hollow marker and a short reason, so a missing measurement
is never an absent point and never a filled one. Python 3.11+, stdlib only, deterministic.
"""
import json
import sys
from datetime import datetime
from html import escape
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ROWS = ROOT / "docs/bench.jsonl"
MD = ROOT / "docs/BENCH.md"
GAPS = ROOT / "docs/bench-gaps.json"

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gaps as gapmod  # noqa: E402

COLOUR = {"bridge": "#1d4ed8", "mali-open": "#b91c1c"}
DASH = {"x8": "", "cmf": "8 4", "x4": "1 4"}
MARK = {"x8": "circle", "cmf": "square", "x4": "triangle"}
DEVICE_NAME = {"x8": "X8", "cmf": "CMF", "x4": "X4"}
SHORT = {"black": "black screen", "crash": "crash", "not_run": "not attempted",
         "unverified": "unverified"}
W, H = 900, 300
L, R, T, B = 56, 884, 26, 244  # plot box


def short_source(s):
    return s.split(" (")[0]


def ts_of(r):
    try:
        return datetime.fromisoformat(r["ts"]).timestamp()
    except ValueError:
        return 0.0


def marker(cx, cy, kind, col, hollow=False):
    fill = "#ffffff" if hollow else col
    if kind == "square":
        return (f'<rect x="{cx - 3.4:.1f}" y="{cy - 3.4:.1f}" width="6.8" height="6.8" fill="{fill}" '
                f'stroke="{col}" stroke-width="1.4"/>')
    if kind == "triangle":
        return (f'<polygon points="{cx:.1f},{cy - 4.6:.1f} {cx + 4.4:.1f},{cy + 3.4:.1f} '
                f'{cx - 4.4:.1f},{cy + 3.4:.1f}" fill="{fill}" stroke="{col}" stroke-width="1.4"/>')
    return (f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{4.2 if hollow else 3.6}" fill="{fill}" '
            f'stroke="{col}" stroke-width="{1.6 if hollow else 0}"/>')


def nice_max(v):
    if v <= 0:
        return 10
    step = 10 if v <= 100 else 50 if v <= 400 else 100
    return int((v * 1.12 + step - 1) // step * step)


def chart(rows, wl):
    xs = [ts_of(r) for r in rows]
    x0, x1 = min(xs), max(xs)
    if x1 - x0 < 3600:
        x0, x1 = x0 - 43200, x1 + 43200
    ymax = nice_max(max((r["fps"] for r in rows if r["status"] == "visible"), default=0.0))

    def X(t):
        return L + (t - x0) / (x1 - x0) * (R - L)

    def Y(f):
        return B - min(f, ymax) / ymax * (B - T)

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
           f'viewBox="0 0 {W} {H}" role="img" aria-label="{escape(wl)} benchmark chart" '
           f'font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif">',
           f'<rect width="{W}" height="{H}" fill="#ffffff"/>']
    # y grid
    step = ymax // 5 or ymax
    y = 0
    while y <= ymax:
        yy = Y(y)
        out.append(f'<line x1="{L}" y1="{yy:.1f}" x2="{R}" y2="{yy:.1f}" stroke="#e5e7eb"/>')
        out.append(f'<text x="{L - 8}" y="{yy + 3:.1f}" font-size="10" fill="#6b7280" '
                   f'text-anchor="end">{y}</text>')
        y += step
    out.append(f'<text x="8" y="{T - 12}" font-size="10" fill="#6b7280" text-anchor="start">fps</text>')
    # x ticks
    ticks = 4
    prev = None
    for i in range(ticks + 1):
        t = x0 + (x1 - x0) * i / ticks
        xx = X(t)
        out.append(f'<line x1="{xx:.1f}" y1="{B}" x2="{xx:.1f}" y2="{B + 5}" stroke="#9ca3af"/>')
        text = datetime.fromtimestamp(t).strftime("%m-%d %H:%M" if x1 - x0 < 3 * 86400 else "%m-%d")
        if text != prev:
            out.append(f'<text x="{xx:.1f}" y="{B + 18}" font-size="10" fill="#6b7280" '
                       f'text-anchor="middle">{text}</text>')
        prev = text

    labels = []  # (x, y, text, colour)
    for (stack, dev), srows in sorted(series_of(rows).items(),
                                      key=lambda kv: (list(COLOUR).index(kv[0][0]),
                                                      list(DASH).index(kv[0][1]))):
        srows.sort(key=ts_of)
        vis = [r for r in srows if r["status"] == "visible" and r["fps"] > 0]
        col = COLOUR.get(stack, "#374151")
        dash = DASH.get(dev, "")
        if len(vis) > 1:
            pts = " ".join(f"{X(ts_of(r)):.1f},{Y(r['fps']):.1f}" for r in vis)
            out.append(f'<polyline points="{pts}" fill="none" stroke="{col}" stroke-width="1.8" '
                       f'{"stroke-dasharray=\"" + dash + "\"" if dash else ""}/>')
        for r in srows:
            xx, ok = X(ts_of(r)), r["status"] == "visible" and r["fps"] > 0
            yy = Y(r["fps"]) if ok else B
            kind = MARK.get(dev, "circle")
            if ok:
                out.append(marker(xx, yy, kind, col))
            else:
                out.append(marker(xx, yy, kind, col, hollow=True))
                out.append(f'<line x1="{xx:.1f}" y1="{yy:.1f}" x2="{xx:.1f}" y2="{yy - 12:.1f}" '
                           f'stroke="{col}" stroke-width="0.8" stroke-dasharray="2 2"/>')
        # Label the first point of each version run, and group repeated failures as "v reason xN".
        cand = []
        i = 0
        while i < len(srows):
            r = srows[i]
            ok = r["status"] == "visible" and r["fps"] > 0
            j, last = i, i
            if not ok:
                while (j + 1 < len(srows) and srows[j + 1]["status"] == r["status"]
                       and srows[j + 1]["version"] == r["version"]
                       and not (srows[j + 1]["status"] == "visible" and srows[j + 1]["fps"] > 0)):
                    j += 1
            text = r["version"] if ok else f'{r["version"]} {SHORT.get(r["status"], r["status"])}'
            if j > i:
                text += f" x{j - i + 1}"
            x = X(ts_of(srows[(i + j) // 2]))
            y = (Y(r["fps"]) if ok else B) - (9 if ok else 16)
            cand.append((x, y, text, col, ok))
            i = j + 1
        # A long commit-by-commit series would stack labels at the same hour: keep at most 7
        # visible labels (first, last and evenly spaced), every row stays in the table below.
        visc = [c for c in cand if c[4]]
        keep = set()
        if len(visc) > 4:
            step = (len(visc) - 1) / 3
            keep = {int(round(k * step)) for k in range(4)}
        vi = -1
        for c in cand:
            if c[4]:
                vi += 1
                if vi not in keep and len(visc) > 4:
                    continue
            labels.append(c[:4])
    # Greedy label placement: try a few offsets around the point, draw a leader when displaced.
    placed = []
    OFFS = [(0, -10), (0, 14), (0, -24), (0, 28), (38, -10), (-38, -10),
            (38, 14), (-38, 14), (0, -38), (0, 42), (0, -52)]
    for xx, yy, text, col in sorted(labels, key=lambda lb: (lb[0], -lb[1])):
        w = 5.2 * len(text)
        spot = None
        for dx, dy in OFFS:
            cx = max(L + w / 2, min(R - w / 2, xx + dx))
            cy = yy + dy
            if not (12 <= cy <= B - 4):
                continue
            if not any(abs(cx - px) < (w + pw) / 2 + 3 and abs(cy - py) < 10
                       for px, py, pw in placed):
                spot = (cx, cy)
                break
        if spot is None:
            continue  # the table below keeps every row
        cx, cy = spot
        if abs(cx - xx) > 12 or abs(cy - yy) > 14:
            out.append(f'<line x1="{xx:.1f}" y1="{yy:.1f}" x2="{cx:.1f}" y2="{cy - 3:.1f}" '
                       f'stroke="{col}" stroke-width="0.6" opacity="0.55"/>')
        placed.append((cx, cy, w))
        out.append(f'<text x="{cx:.1f}" y="{cy:.1f}" font-size="8.5" fill="{col}" '
                   f'text-anchor="middle" style="paint-order:stroke" stroke="#ffffff" '
                   f'stroke-width="2.5">{escape(text)}</text>')
    out.append("</svg>")
    return "\n".join(out)


def series_of(rows):
    d = {}
    for r in rows:
        d.setdefault((r["stack"], r["device"]), []).append(r)
    return d


NOTE = {
    "cards": "cube-720p and cube-native variants share this chart (the note column says which); "
             "both are GPU-heavy cube draws, so a mixed line is not a like-for-like comparison.",
    "triangles": "triangle-3k/5k/10k/20k share this chart; the draw count is in the note column.",
    "steam-menu": "Steam Big Picture menu. menubench rows are render-side avg fps; the 2026-11-20 "
                  "owner-reported runs are screen-side and their metric is stated in the source.",
    "megabonk": "Megabonk (app 3405340), pad loop, on-screen fps from the mali-gaming runs. "
                "No mali-open row exists yet: its series is a to-do, not a gap in the drawing.",
    "glclear": "glClear readback test. A swap that succeeds while the screenshot stays black is a "
               "black screen, not a measurement.",
}


def legend():
    parts, x, y = [], 56, 14
    for stack, col in COLOUR.items():
        for dev, dash in DASH.items():
            label = f"{stack}/{DEVICE_NAME[dev]}"
            if x + 34 + 6.2 * len(label) > W - 10:
                x, y = 56, y + 20
            d = f' stroke-dasharray="{dash}"' if dash else ""
            parts.append(f'<line x1="{x}" y1="{y}" x2="{x + 24}" y2="{y}" stroke="{col}" '
                         f'stroke-width="2"{d} stroke-linecap="round"/>')
            parts.append(marker(x + 12, y, MARK.get(dev, "circle"), col))
            parts.append(f'<text x="{x + 29}" y="{y + 3.5}" font-size="10" fill="#111827">'
                         f'{escape(label)}</text>')
            x += 29 + 6.2 * len(label) + 22
    parts.append(marker(x + 4, y, "circle", "#111827", hollow=True))
    parts.append(f'<text x="{x + 14}" y="{y + 3.5}" font-size="10" fill="#111827">'
                 f'no visible frame (fps 0, reason labelled)</text>')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{y + 12}" '
            f'viewBox="0 0 {W} {y + 12}"><rect width="{W}" height="{y + 12}" fill="#ffffff"/>'
            + "".join(parts) + "</svg>")


def table(rows):
    out = ["| when | device | stack | version | fps | brightness | status | note |",
           "|---|---|---|---|---|---|---|---|"]
    for r in rows:
        fps = f"{r['fps']:.1f}" if r["status"] == "visible" and r["fps"] > 0 else "0"
        bright = "-" if r.get("brightness_mean") is None else f"{r['brightness_mean']:.2f}"
        note = r.get("note") or ""
        out.append(f"| {escape(r['ts'][:16].replace('T', ' '))} | {r['device']} | {r['stack']} | "
                   f"{escape(r['version'])} | {fps} | {bright} | {r['status']} | {escape(note)} |")
    return "\n".join(out)


def sources_block(rows):
    lines = [f"- `{r['ts'][:10]}` {r['device']}/{r['stack']}/{r['version']}/{r['workload']} "
             f"fps={r['fps']:.1f} {r['status']} — {r['source']}" for r in rows]
    return ("<details><summary>sources for these rows</summary>\n\n" + "\n".join(lines) +
            "\n\n</details>")


def build(rows):
    g = gapmod.build(rows)
    by_wl = {}
    for r in rows:
        by_wl.setdefault(r["workload"], []).append(r)
    lines = [
        "# Benchmark charts: mali-open vs MaliBridge, X8 and CMF",
        "",
        "Generated by `tools/bench/charts.py` from `docs/bench.jsonl` — **do not edit**: run `make bench`.",
        "MaliBridge p22 and p25 are one line per device: versions are point labels, not separate series.",
        "",
        "How to read a chart: x is real time (each point labelled with its version), colour is the stack,",
        "line style is the device. A run with no visible frame is plotted at fps 0 with a hollow marker and",
        "a short reason (black screen / crash / not attempted / unverified); it is never omitted and never",
        "drawn as a filled measurement. Lines join visible points only.",
        "",
        legend(),
        "",
        "## Coverage",
        "",
        f"{g['counts']['measured']} measured, {g['counts']['failed']} failed, "
        f"{g['counts']['missing']} missing of {g['counts']['cells']} "
        f"(device × stack × version × workload) cells. `docs/bench-gaps.json` lists every missing cell "
        f"with the runbook that would fill it.",
        "",
        "| workload | device | stack | rows | visible | hollow | last version |",
        "|---|---|---|---|---|---|---|",
    ]
    for s in g["series"]:
        lines.append(f"| {s['workload']} | {s['device']} | {s['stack']} | {s['points']} | "
                     f"{s['visible']} | {s['hollow']} | {escape(s['last'] or '—')} |")
    lines.append("")

    for wl, name in gapmod.WORKLOADS.items():
        wrows = sorted(by_wl.get(wl, []), key=lambda r: (r["ts"], r["device"], r["stack"]))
        vis = [r for r in wrows if r["status"] == "visible" and r["fps"] > 0]
        hollow = [r for r in wrows if not (r["status"] == "visible" and r["fps"] > 0)]
        lines += [f"## {name}", "", NOTE.get(wl, ""), ""] if NOTE.get(wl) else [f"## {name}", ""]
        if not wrows:
            lines += ["No runs recorded for this workload yet — the whole chart is a to-do. "
                      f"Runbook: `{gapmod.RUNBOOK[wl].format(alias='poco-x8', drv='ours')}`.", ""]
            continue
        lines += [f"{len(vis)} real points, {len(hollow)} at fps 0 (hollow).", "", chart(wrows, wl), ""]
        lines += [table(wrows), "", sources_block(wrows), ""]

    lines += ["## Next cells to fill", ""]
    for i, cell in enumerate(g["next"][:8], 1):
        rb = cell["runbook"]
        lines.append(f"{i}. **{cell['workload']} / {cell['device']} / {cell['stack']} / "
                     f"{cell['version']}** — {cell['why']}. `{rb['test']}` on {rb['device']} "
                     f"with {rb['apk_or_driver']}.")
    lines += ["", "Full matrix and per-cell runbooks: `docs/bench-gaps.json`.", "",
              f"_Generated from {len(rows)} rows; {g['counts']['unattributed_rows']} source rows carry "
              f"no device/version and are excluded from the matrix._", ""]
    return "\n".join(lines), g


def main():
    check = "--check" in sys.argv[1:]
    if "--gaps" in sys.argv[1:]:
        print(gapmod.summary(gapmod.build(gapmod.load())))
        return 0
    rows = [json.loads(l) for l in ROWS.read_text().splitlines() if l.strip()]
    text, g = build(rows)
    blob = json.dumps(g, indent=1, sort_keys=False) + "\n"
    if check:
        stale = []
        if (MD.read_text() if MD.is_file() else "") != text:
            stale.append(str(MD.relative_to(ROOT)))
        if (GAPS.read_text() if GAPS.is_file() else "") != blob:
            stale.append(str(GAPS.relative_to(ROOT)))
        if stale:
            print("bench: stale: " + ", ".join(stale) + "; run make bench", file=sys.stderr)
            return 1
        print(f"bench: {MD.relative_to(ROOT)} and {GAPS.relative_to(ROOT)} up to date "
              f"({len(rows)} rows)")
        return 0
    MD.write_text(text)
    GAPS.write_text(blob)
    print(f"bench: wrote {MD.relative_to(ROOT)} and {GAPS.relative_to(ROOT)} ({len(rows)} rows)")
    print(gapmod.summary(g), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
