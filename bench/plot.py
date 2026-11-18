#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Performance charts + README table from bench/results.jsonl (the source of bench/RESULTS.md, same rows, structured).

Writes bench/charts/<phone>.svg (one panel per benchmark: ours over builds, bridge per MediaDeck version, PanVK if measured),
bench/charts/summary.svg (ours as % of the bridge) and the table between <!-- perf:begin --> / <!-- perf:end --> in README.md.
fps = presented fps (what a game shows); GPU ms = offscreen GPU time. Run: python3 bench/plot.py [--root DIR]"""
import json, os, re, sys

PHONES = {"cmf": "CMF Mali-G615 (v11)", "poco-x8": "Poco X8 Mali-G925 (v13)", "poco-x4": "Poco X4 Mali-G610 (v10)"}
BEGIN, END = "<!-- perf:begin -->", "<!-- perf:end -->"
BAD = re.compile(r"invalid|not publishable|unpublishable|failed", re.I)


def drv_kind(d):
    d = (d or "").lower()
    return "ours" if d == "ours" else "bridge" if d == "bridge" else "panvk" if d in ("panvk", "openvk") else None


def load(path):
    """-> list of clean rows {phone,variant,kind,build,ver,date,fps,gpu,idx}; INVALID/failed/unpublishable rows are dropped"""
    out, dropped = [], []
    for i, line in enumerate(open(path)):
        if not line.strip():
            continue
        r = json.loads(line)
        kind = drv_kind(r.get("driver"))
        pres = r.get("presented")
        if not kind or not isinstance(pres, dict) or not pres.get("fps"):
            dropped.append((r.get("phone"), r.get("variant"), r.get("driver"), r.get("commit"), "no fps / unknown driver"))
            continue
        if r.get("version") == "fake" or any(BAD.search(str(r.get(k) or "")) for k in ("note", "notes", "status", "best_settings")):
            dropped.append((r.get("phone"), r.get("variant"), r.get("driver"), r.get("commit"), "invalid/fake/not publishable"))
            continue
        off = r.get("offscreen") if isinstance(r.get("offscreen"), dict) else {}
        build = str(r.get("commit_full") or r.get("commit") or "?")
        out.append({"phone": r["phone"], "variant": r["variant"], "kind": kind, "build": build[:9] + ("-dirty" if build.endswith("-dirty") and "-dirty" not in build[:9] else ""),
                    "ver": r.get("version") or "?", "date": r.get("date") or "", "fps": float(pres["fps"]), "gpu": off.get("gpu_ms"), "idx": i})
    for d in dropped:                                            # never drop silently: every skipped row is named
        print("plot: skipped row", *d, file=sys.stderr)
    return out


def series(rows, phone, variant, kind):
    """chronological (file order = newest last) list of rows; one per build for ours (last wins)"""
    s = [r for r in rows if (r["phone"], r["variant"], r["kind"]) == (phone, variant, kind)]
    if kind == "ours":
        d = {}
        for r in s:
            d.pop(r["build"], None)
            d[r["build"]] = r
        s = list(d.values())
    return s


def vers_short(v):
    m = re.match(r"malideck (p\d+)", v)
    return m.group(1) if m else v[:14]


def collect(rows):
    """-> {phone: {variant: {"ours": [...], "bridge": {ver: [...]}, "panvk": [...]}}}"""
    res = {}
    for p in [p for p in PHONES if any(r["phone"] == p for r in rows)] + sorted({r["phone"] for r in rows} - set(PHONES)):
        for v in sorted({r["variant"] for r in rows if r["phone"] == p}):
            br = {}
            for r in series(rows, p, v, "bridge"):
                br.setdefault(r["ver"], []).append(r)
            res.setdefault(p, {})[v] = {"ours": series(rows, p, v, "ours"), "bridge": br, "panvk": series(rows, p, v, "panvk")}
    return res


def latest_bridge(d):
    vs = [r for ver in d["bridge"].values() for r in ver]
    return max(vs, key=lambda r: r["idx"]) if vs else None


def pct(a, b):
    return 100.0 * a / b if a is not None and b else None


def fmt(x, nd=1):
    return "n/a" if x is None else f"{x:.{nd}f}"


def table(data):
    lines = ["| Phone | GPU / arch | Benchmark | Ours first (build, date) | Ours latest (build, date) | Change | Bridge latest fps | Ours % of bridge | PanVK | GPU ms ours / bridge |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    for p, vs in data.items():
        for v, d in vs.items():
            o, b = d["ours"], latest_bridge(d)
            if not o and not b and not d["panvk"]:
                continue
            first, last = (o[0], o[-1]) if o else (None, None)
            ch = pct(last["fps"], first["fps"]) - 100 if o and len(o) > 1 else None
            pv = d["panvk"][-1]["fps"] if d["panvk"] else None
            gp = f"{fmt(last['gpu'], 2)} / {fmt(b['gpu'], 2)}" if last and b else "n/a"
            lines.append("| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
                p, PHONES.get(p, p), v,
                f"{first['fps']:.1f} ({first['build']}, {first['date']})" if first else "n/a",
                f"{last['fps']:.1f} ({last['build']}, {last['date']})" if last else "n/a",
                "n/a" if ch is None else f"{ch:+.0f}%", f"{b['fps']:.1f} ({vers_short(b['ver'])})" if b else "n/a",
                "n/a" if not (last and b) else f"{pct(last['fps'], b['fps']):.0f}%", f"{pv:.1f}" if pv else "not measured", gp))
    return "\n".join(lines)


def charts(data, outdir):
    written = []
    import matplotlib
    matplotlib.use("svg")
    matplotlib.rcParams["svg.hashsalt"] = "mali-open"          # stable ids: no diff noise between runs
    matplotlib.rcParams["svg.fonttype"] = "none"
    import matplotlib.pyplot as plt
    os.makedirs(outdir, exist_ok=True)
    md = {"Date": None}
    for p, vs in data.items():
        vs = {v: d for v, d in vs.items() if d["ours"] or d["bridge"] or d["panvk"]}
        if not vs:
            continue
        fig, axs = plt.subplots(1, len(vs), figsize=(5.2 * len(vs), 4.2), squeeze=False)
        for ax, (v, d) in zip(axs[0], vs.items()):
            o = d["ours"]
            if o:
                ax.plot(range(len(o)), [r["fps"] for r in o], "o-", color="tab:green", label="ours (mali-open)")
                ax.set_xticks(range(len(o)))
                ax.set_xticklabels([f"{r['build'][:7]}\n{r['date'][5:]}" for r in o], rotation=60, fontsize=7, ha="right")
            else:
                ax.set_xticks([])
            n = max(len(o), 1)
            for k, (ver, rs) in enumerate(d["bridge"].items()):
                c = ["tab:blue", "tab:orange", "tab:purple", "tab:brown", "tab:gray"][k % 5]      # never green: that is ours
                pos = {r["build"]: i for i, r in enumerate(o)}
                hit = [r for r in rs if r["build"] in pos]
                if len(rs) > 1 and hit:
                    ax.plot([pos[r["build"]] for r in hit], [r["fps"] for r in hit], "s--", ms=3, color=c, label=f"bridge {vers_short(ver)}")
                else:
                    ax.axhline(rs[-1]["fps"], ls="--", color=c, label=f"bridge {vers_short(ver)}")
            if d["panvk"]:
                ax.axhline(d["panvk"][-1]["fps"], ls=":", color="tab:red", label="PanVK")
            ax.set_title(v, fontsize=10)
            ax.set_ylabel("fps")
            ax.set_ylim(bottom=0)
            ax.grid(alpha=.3)
            ax.legend(fontsize=7)
            ax.set_xlim(-0.5, n - 0.5)
        fig.suptitle(PHONES.get(p, p), fontsize=12)
        fig.tight_layout()
        fig.savefig(os.path.join(outdir, p + ".svg"), metadata=md)
        written.append(p + ".svg")
        plt.close(fig)
    # summary: latest ours as % of the bridge
    phones = [p for p, vs in data.items() if any(d["ours"] and latest_bridge(d) for d in vs.values())]
    if phones:
        vars_ = sorted({v for p in phones for v, d in data[p].items() if d["ours"] and latest_bridge(d)})
        fig, ax = plt.subplots(figsize=(1.6 * len(vars_) * len(phones) / 2 + 4, 4.2))
        w = 0.8 / len(phones)
        for i, p in enumerate(phones):
            xs, hs = [], []
            for j, v in enumerate(vars_):
                d = data[p].get(v)
                if d and d["ours"] and latest_bridge(d):
                    xs.append(j + i * w - 0.4 + w / 2)
                    hs.append(pct(d["ours"][-1]["fps"], latest_bridge(d)["fps"]))
            bars = ax.bar(xs, hs, w * 0.95, label=PHONES.get(p, p))
            for x, h in zip(xs, hs):
                ax.text(x, h + 1, f"{h:.0f}", ha="center", fontsize=7)
        ax.axhline(100, color="k", lw=1, ls="--")
        ax.set_xticks(range(len(vars_)))
        ax.set_xticklabels(vars_)
        ax.set_ylabel("ours as % of Mali bridge (fps)")
        ax.set_title("mali-open vs Mali bridge, latest build per phone")
        ax.legend(fontsize=8)
        ax.grid(axis="y", alpha=.3)
        fig.tight_layout()
        fig.savefig(os.path.join(outdir, "summary.svg"), metadata=md)
        written.append("summary.svg")
        plt.close(fig)
    for f in os.listdir(outdir):                                  # obsolete generated charts (only ones this script names: phone svgs + summary)
        if f.endswith(".svg") and f not in written:
            os.remove(os.path.join(outdir, f))
    return sorted(written)


def section(data, files):
    links = ["", "Charts (ours over builds; dashed = Mali bridge per MediaDeck version; PanVK is drawn once it is measured):", "",
             "![summary](bench/charts/summary.svg)" if "summary.svg" in files else ""]
    links += [f"![{f[:-4]}](bench/charts/{f})" for f in files if f != "summary.svg"]
    note = ("fps = presented fps (best settings per row); GPU ms = offscreen GPU time. Ours is the latest build per phone, the bridge its "
            "latest MediaDeck build. `not measured` = no PanVK (Mesa) result for that row. Source: `bench/results.jsonl` via `bench/plot.py` (best effort: refreshed when a bench publish adds benchmark rows).")
    return "\n".join([BEGIN, "", table(data), "", note, *links, "", END])


def main(argv):
    root = os.path.abspath(argv[argv.index("--root") + 1]) if "--root" in argv else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    rows = load(os.path.join(root, "bench", "results.jsonl"))
    data = collect(rows)
    try:
        files = charts(data, os.path.join(root, "bench", "charts"))
    except ImportError as e:                                     # no matplotlib on this machine: still refresh the table, keep existing charts
        print(f"plot: charts skipped ({e})", file=sys.stderr)
        cd = os.path.join(root, "bench", "charts")
        files = sorted(f for f in os.listdir(cd) if f.endswith(".svg")) if os.path.isdir(cd) else []
    rd = os.path.join(root, "README.md")
    txt = open(rd).read()
    sec = section(data, files)
    if BEGIN in txt and END in txt:
        a, b = txt.index(BEGIN), txt.index(END) + len(END)
        txt = txt[:a] + sec + txt[b:]
    else:
        txt = txt.rstrip("\n") + "\n\n" + sec + "\n"
    open(rd, "w").write(txt)
    print(f"plot: {len(rows)} rows, charts {', '.join(files)}")


if __name__ == "__main__":
    main(sys.argv[1:])
