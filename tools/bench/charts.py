#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Benchmark charts: one inline-SVG chart per workload in docs/BENCH.md.

  tools/bench/charts.py            write docs/BENCH.md and docs/bench-gaps.json
  tools/bench/charts.py --check    exit 1 if a committed file would change
  tools/bench/charts.py --gaps     only print the coverage summary

The store is docs/bench.jsonl and the contract is docs/MEASURE.md: `fps` is displayed frames
per second (menubench `screen.fps` / SurfaceFlinger timestamps / a HUD number a human can see),
for the default profile, with `reps` and `panel_hz` on every point. `render_fps` and
`presented_fps` may be carried along but are never drawn, never compared and never used for
"best". Each chart draws its devices' panel ceiling (X8/CMF 120 Hz, X4 144 Hz) as a dashed
line, so a point above it is visibly impossible rather than quietly cropped.

A run with no visible frame (black / crash / not attempted / unverified) plots at fps 0 as a
hollow marker with its reason: a missing measurement is never an absent point and never a
filled one. Python 3.11+, stdlib only, deterministic.
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
W, H = 900, 330
L, R, T, B = 56, 884, 30, 262  # plot box


def ts_of(r):
    try:
        return datetime.fromisoformat(r["ts"]).timestamp()
    except (ValueError, TypeError):
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
        return 160
    return max(160, int((v * 1.15 + 9) // 10 * 10))


def visible(r):
    return r.get("status") == "visible" and isinstance(r.get("fps"), (int, float)) and r["fps"] > 0


def plotted(r):
    """Only default-profile visible points form lines: other profiles are not comparable."""
    return visible(r) and r.get("profile") == "default"


def chart(rows, wl):
    pts = [r for r in rows]
    xs = [ts_of(r) for r in pts]
    x0, x1 = min(xs), max(xs)
    if x1 - x0 < 3600:
        x0, x1 = x0 - 43200, x1 + 43200
    top = nice_max(max((r["fps"] for r in pts if visible(r)), default=0.0))
    panels = sorted({r["panel_hz"] for r in pts
                     if isinstance(r.get("panel_hz"), (int, float))})

    def X(t):
        return L + (t - x0) / (x1 - x0) * (R - L)

    def Y(f):
        return B - min(f, top) / top * (B - T)

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
           f'viewBox="0 0 {W} {H}" role="img" aria-label="{escape(wl)} displayed-fps chart" '
           f'font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif">',
           f'<rect width="{W}" height="{H}" fill="#ffffff"/>']
    step = top // 5 or top
    y = 0
    while y <= top:
        yy = Y(y)
        out.append(f'<line x1="{L}" y1="{yy:.1f}" x2="{R}" y2="{yy:.1f}" stroke="#e5e7eb"/>')
        out.append(f'<text x="{L - 8}" y="{yy + 3:.1f}" font-size="10" fill="#6b7280" '
                   f'text-anchor="end">{y}</text>')
        y += step
    out.append(f'<text x="8" y="{T - 14}" font-size="10" fill="#6b7280">displayed fps</text>')

    # Panel ceiling per device: anything above the line is physically impossible.
    owners = {}
    for r in pts:
        if isinstance(r.get("panel_hz"), (int, float)):
            owners.setdefault(int(r["panel_hz"]), set()).add(r["device"])
    for hz in sorted(owners):
        yy = Y(hz)
        out.append(f'<line x1="{L}" y1="{yy:.1f}" x2="{R}" y2="{yy:.1f}" stroke="#dc2626" '
                   f'stroke-width="1.2" stroke-dasharray="6 4" opacity="0.8"/>')
        names = "/".join(DEVICE_NAME.get(d, d) for d in sorted(owners[hz]))
        out.append(f'<text x="{R - 4}" y="{yy - 5:.1f}" font-size="9.5" fill="#b91c1c" '
                   f'text-anchor="end">{escape(names)} panel {hz} Hz ceiling</text>')

    for i in range(5):
        t = x0 + (x1 - x0) * i / 4
        xx = X(t)
        out.append(f'<line x1="{xx:.1f}" y1="{B}" x2="{xx:.1f}" y2="{B + 5}" stroke="#9ca3af"/>')
        text = datetime.fromtimestamp(t).strftime("%m-%d %H:%M" if x1 - x0 < 3 * 86400 else "%m-%d")
        out.append(f'<text x="{xx:.1f}" y="{B + 18}" font-size="10" fill="#6b7280" '
                   f'text-anchor="middle">{text}</text>')

    series = {}
    for r in pts:
        series.setdefault((r["stack"], r["device"]), []).append(r)
    for (stack, dev), srows in sorted(series.items(),
                                      key=lambda kv: (list(COLOUR).index(kv[0][0]),
                                                      list(DASH).index(kv[0][1]))):
        srows.sort(key=ts_of)
        col = COLOUR.get(stack, "#374151")
        dash = DASH.get(dev, "")
        vis = [r for r in srows if plotted(r)]
        if len(vis) > 1:
            path = " ".join(f"{X(ts_of(r)):.1f},{Y(r['fps']):.1f}" for r in vis)
            style = f' stroke-dasharray="{dash}"' if dash else ""
            out.append(f'<polyline points="{path}" fill="none" stroke="{col}" '
                       f'stroke-width="1.8"{style}/>')
        for r in srows:
            cx, cy = X(ts_of(r)), Y(r["fps"]) if visible(r) else Y(0)
            hollow = not plotted(r)
            out.append(marker(cx, cy, MARK.get(dev, "circle"), col, hollow))
            if plotted(r):
                label = f"{r['fps']:.1f}×{r['reps']}"
            else:
                label = SHORT.get(r.get("status"), r.get("status") or "no data")
                if visible(r):
                    label = f"{r['fps']:.1f} ({r.get('profile')})"
            out.append(f'<text x="{cx:.1f}" y="{cy - 7:.1f}" font-size="8.5" fill="{col}" '
                       f'text-anchor="middle" style="paint-order:stroke" stroke="#ffffff" '
                       f'stroke-width="2.5">{escape(label)}</text>')

    out.append("</svg>")
    return "\n".join(out)


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
    out = ["| when | device | stack | version | profile | displayed fps | panel Hz | reps | "
           "brightness | status | render_fps (not plotted) | note |",
           "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        fps = f"{r['fps']:.1f}" if visible(r) else "0"
        bright = "-" if r.get("brightness_mean") is None else f"{r['brightness_mean']:.2f}"
        rf = "-" if r.get("render_fps") is None else f"{r['render_fps']:.1f}"
        out.append(f"| {escape(str(r['ts'])[:16].replace('T', ' '))} | {r['device']} | "
                   f"{r['stack']} | {escape(str(r['version']))} | {r.get('profile')} | {fps} | "
                   f"{r.get('panel_hz')} | {r.get('reps')} | {bright} | {r.get('status')} | "
                   f"{rf} | {escape(str(r.get('note') or ''))} |")
    return "\n".join(out)


def median(values):
    v = sorted(values)
    n = len(v)
    if not n:
        return None
    return v[n // 2] if n % 2 else (v[n // 2 - 1] + v[n // 2]) / 2


def bests(rows):
    """'best' = median of >= 3 reps at the same device/stack/version/profile, panel and day."""
    groups = {}
    for r in rows:
        if not (visible(r) and r.get("profile") == "default" and (r.get("reps") or 0) >= 3):
            continue
        key = (r["device"], r["stack"], r["version"], r["profile"], r["panel_hz"], r["ts"][:10])
        groups.setdefault(key, []).append(r)
    out = []
    for (dev, st, ver, prof, panel, day), group in sorted(groups.items()):
        out.append({"device": dev, "stack": st, "version": ver, "profile": prof, "day": day,
                    "panel_hz": panel, "reps": len(group),
                    "median_fps": round(median([r["fps"] for r in group]), 1),
                    "values": [r["fps"] for r in group]})
    return out


def sources_block(rows):
    lines = [f"- `{str(r['ts'])[:10]}` {r['device']}/{r['stack']}/{r['version']}/"
             f"{r['workload']} fps={r['fps']} reps={r.get('reps')} — {r['source']}" for r in rows]
    return ("<details><summary>sources for these rows</summary>\n\n" + "\n".join(lines) +
            "\n\n</details>")


NOTE = {
    "cards": "cube/cards. No valid displayed-fps point exists yet: bench2 rows only carry "
             "`presented.fps`, a producer-side rate, and are rejected by `tools/bench/import.py`.",
    "triangles": "triangles. No valid displayed-fps point exists yet. The 850.6 that used to be "
                 "plotted was `presented.fps` for triangle-3k, an offscreen producer rate.",
    "steam-menu": "Steam Big Picture menu. Only menubench rows with `screen.fps` (SurfaceFlinger "
                  "present timestamps) are plotted; `render.avg_fps` stays in its own column.",
    "megabonk": "Megabonk. Every historical figure is a hand-typed claim in FINDINGS.md with no "
                "panel_hz or reps, so none is plotted; it needs a MEASURE.md run.",
    "glclear": "glClear readback. A swap that succeeds while the screenshot stays black is a "
               "black screen, not a measurement.",
    "vkcube-wayland": "vkcube over Wayland: the first driver-mode lane that reaches a real "
                      "present (`present #20 -> VK_SUCCESS`). A WSI smoke workload — it says the "
                      "wayland swapchain/present path works and how fast the cube is paced, never "
                      "a game's fps. Only compare within this lane.",
    "vkcube-xcb": "vkcube over X11 (dri3). Same WSI smoke contract as vkcube-wayland: it proves "
                  "`vkCreateXcbSurfaceKHR` + the xcb present path, not game performance.",
}


def build(rows):
    g = gapmod.build(rows)
    by_wl = {}
    for r in rows:
        by_wl.setdefault(r["workload"], []).append(r)
    lines = [
        "# Benchmark charts: displayed fps only (mali-open vs MaliBridge)",
        "",
        "Generated by `tools/bench/charts.py` from the strict `docs/bench.jsonl` — **do not edit**: "
        "run `make bench`. Contract: [`docs/MEASURE.md`](MEASURE.md).",
        "",
        "**The store was reset on 2026-11-20.** The previous charts compared render-side and "
        "producer-side rates with what a user sees (`421.1` on a 120 Hz panel, `850.6` for an "
        "offscreen triangle test). Those rows are archived, not deleted, in "
        "`docs/bench-archive/2026-11-20-invalid/`. Every number below is displayed fps from "
        "`screen.fps`/SurfaceFlinger, with its panel ceiling drawn as a dashed red line: nothing "
        "above that line is physically possible, and a point above it can only be instrument error.",
        "",
        "How to read a chart: x is real time, colour is the stack, line style is the device. "
        "Point labels are `fps×reps`. Only `profile=default` visible points form lines; other "
        "profiles (uncapped, CEF variants) are listed in the table but never compared. "
        "`best` is the median of at least 3 reps at the same device/stack/version/profile/panel.",
        "",
        legend(),
        "",
        "## Coverage",
        "",
        f"{g['counts']['measured']} measured cells (a valid point exists), "
        f"{g['counts']['failed']} failed (a run exists, no valid point), "
        f"{g['counts']['missing']} missing of {g['counts']['cells']} "
        f"(device × stack × version × workload). `docs/bench-gaps.json` lists every missing cell "
        f"with the runbook that would fill it.",
        "",
        "| workload | device | stack | rows | valid points | no-valid-point rows | last valid version |",
        "|---|---|---|---|---|---|---|",
    ]
    for s in g["series"]:
        lines.append(f"| {s['workload']} | {s['device']} | {s['stack']} | {s['points']} | "
                     f"{s['valid']} | {s['hollow']} | {escape(s['last'] or '—')} |")
    lines.append("")

    for wl, name in gapmod.WORKLOADS.items():
        wrows = sorted(by_wl.get(wl, []), key=lambda r: (r["ts"], r["device"], r["stack"]))
        val = [r for r in wrows if plotted(r) and (r.get("reps") or 0) >= 3]
        lines += [f"## {name}", ""]
        if NOTE.get(wl):
            lines += [NOTE[wl], ""]
        if not wrows:
            lines += ["**No rows at all.** Nothing on this chart is a measurement yet. Runbook: "
                      f"`{gapmod.RUNBOOK[wl].format(alias='poco-x8', drv='ours')}`.", ""]
            continue
        lines += [f"{len(val)} plotted point(s) from ≥3-rep default-profile runs; "
                  f"{len(wrows)} stored row(s) total.", "", chart(wrows, wl), ""]
        best = bests(wrows)
        if best:
            lines += ["| day | device | stack | version | profile | panel Hz | median fps | reps |",
                      "|---|---|---|---|---|---|---|---|"]
            for b in best:
                lines.append(f"| {b['day']} | {b['device']} | {b['stack']} | {escape(b['version'])} | "
                             f"{b['profile']} | {b['panel_hz']} | **{b['median_fps']}** | "
                             f"{b['reps']} ({', '.join(f'{v:.1f}' for v in b['values'])}) |")
            lines.append("")
        else:
            lines += ["**No valid point yet:** no default-profile group has 3 reps, so this "
                      "workload has no number to quote. That is the honest state, not a gap in "
                      "the drawing.", ""]
        lines += [table(wrows), "", sources_block(wrows), ""]

    lines += ["## Next cells to fill", ""]
    for i, cell in enumerate(g["next"][:8], 1):
        rb = cell.get("runbook") or {}
        lines.append(f"{i}. **{cell['workload']} / {cell['device']} / {cell['stack']} / "
                     f"{cell['version']}** — {cell['why']}."
                     + (f" `{rb['test']}` with {rb['apk_or_driver']}." if rb else ""))
    lines += ["", "Full matrix and per-cell runbooks: `docs/bench-gaps.json`.", "",
              f"_Generated from {len(rows)} rows; every plotted point carries `reps` and "
              f"`panel_hz`. Rejected source rows are listed with reasons in "
              f"`docs/rejected.jsonl`._", ""]
    return "\n".join(lines), g


def main():
    args = sys.argv[1:]
    if "--gaps" in args:
        print(gapmod.summary(gapmod.build(gapmod.load())))
        return 0
    rows = [json.loads(l) for l in ROWS.read_text().splitlines() if l.strip()]
    text, g = build(rows)
    blob = json.dumps(g, indent=1, sort_keys=False) + "\n"
    if "--check" in args:
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
