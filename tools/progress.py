#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Progress diagram of the mali-open driver: docs/progress.svg (+ docs/progress.md).

  tools/progress.py            write docs/progress.svg and docs/progress.md
  tools/progress.py --check    exit 1 if the committed files are stale (fast, offline)

Derived from the tree: DXVK feature and extension lists (vk/tests/t_dxvk_caps.c), what is advertised
(vk/src/mali_physical_device.c, cross-checked with DXVK_MISSING in vk/check.sh), dEQP sample results
(vk/deqp/ours-daily-sample-*.tsv), roadmap stages (ROADMAP.md). The rest is docs/progress.toml, where every
"done" item names evidence that this script verifies exists. Python 3.11+, stdlib only, deterministic.
"""
import re
import subprocess
import sys
import tomllib
from html import escape
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SVG = ROOT / "docs/progress.svg"
MD = ROOT / "docs/progress.md"
INPUTS = ["docs/progress.toml", "vk/tests/t_dxvk_caps.c", "vk/src/mali_physical_device.c", "vk/check.sh",
          "ROADMAP.md", "vk/deqp"]
STAMP_RE = re.compile(r"inputs last changed [^<]*")
W = 800


def die(msg):
    sys.exit("progress.py: " + msg)


def read(rel):
    p = ROOT / rel
    if not p.is_file():
        die(f"missing input {rel}")
    return p.read_text()


def strip_c_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)


def c_block(src, head):
    i = src.find(head)
    if i < 0:
        die(f"cannot find '{head}'")
    j = src.index("{", i)
    k = src.index("\n   };", j) if "\n   };" in src[j:] else src.index("};", j)
    return strip_c_comments(src[j:k])


# ---------------------------------------------------------------- evidence
def verify(ev, status, what):
    """Evidence entries: 'path', 'path#token' or 'branch:name'. Returns the list of verified file entries."""
    ok = []
    for e in ev:
        if e.startswith("branch:"):
            continue
        path, _, tok = e.partition("#")
        p = ROOT / path
        if not p.exists():
            die(f"{what}: evidence {e} does not exist")
        if tok and (not p.is_file() or tok not in p.read_text(errors="replace")):
            die(f"{what}: token '{tok}' not found in {path}")
        ok.append(e)
    if status == "done" and not ok:
        die(f"{what}: 'done' needs evidence that is a file in this tree")
    return ev


# ---------------------------------------------------------------- derived data
def derive_dxvk():
    caps = read("vk/tests/t_dxvk_caps.c")
    dev = read("vk/src/mali_physical_device.c")
    chk = read("vk/check.sh")
    feats_on = set(re.findall(r"\.(\w+)\s*=\s*true", c_block(dev, "const struct vk_features features")))
    exts_on = {"VK_" + e for e in re.findall(r"\.(\w+)\s*=\s*true",
                                             c_block(dev, "const struct vk_device_extension_table extensions"))}
    reqsrc = strip_c_comments(caps[caps.index("req[] = {"):caps.index("unsigned nreq")])
    reqsrc = re.sub(r"^\s*#.*$", "", reqsrc, flags=re.M)  # drop the #define / #undef lines
    req = [("core." + m, m) for m in re.findall(r"\bC\((\w+)\)", reqsrc)]
    req += [(f"{a}.{b}", b) for a, b in re.findall(r"\bV\((\d+),\s*(\w+)\)", reqsrc)]
    # extension-feature rows written by hand (they are not f11/f12/f13 members): {"robustness2.robustBufferAccess2", fr2.x}
    req += [(f"robustness2.{m}", m) for m in re.findall(r'\{"robustness2\.(\w+)",', reqsrc)]
    exts = re.findall(r'"(VK_\w+)"', caps[caps.index("dxvk_exts[]"):caps.index("int\nmain")])
    if not req or not exts:
        die("could not parse t_dxvk_caps.c")
    features = [(n, short, "done" if short in feats_on else "none") for n, short in req]
    extl = [("ext." + e, e, "done" if e in exts_on else "none") for e in exts]
    missing = sum(1 for f in features if f[2] == "none")
    want = int(re.search(r"^DXVK_MISSING=(\d+)", chk, re.M).group(1))
    if missing != want:
        die(f"t_dxvk_caps list vs advertised table gives {missing} missing, vk/check.sh DXVK_MISSING={want}")
    return features, extl


def derive_deqp(tsv):
    c = {"Pass": 0, "Fail": 0, "NotSupported": 0, "Other": 0}
    for line in read(f"vk/deqp/ours-daily-sample-{tsv}.tsv").splitlines():
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        k = parts[1]
        c[k if k in c else "Other"] += 1
    return c


def derive_stages():
    out = []
    for line in read("ROADMAP.md").splitlines():
        m = re.match(r"\|\s*(R\d)\s*\|([^|]*)\|\s*(.*?)\s*\|\s*$", line)
        if not m:
            continue
        rid, goal, state = m.group(1), m.group(2).strip(), m.group(3).replace("**", "")
        if state.startswith("Out of this tree"):
            continue
        st = "done" if state.startswith("Done") else "none" if state.startswith("Not started") else "partial"
        out.append((f"{rid}: {goal}", st, ["ROADMAP.md"]))
    if not out:
        die("no stages in ROADMAP.md")
    return out


def commit_stamp():
    try:
        r = subprocess.run(["git", "log", "-1", "--format=%h %cs", "--"] + INPUTS, cwd=ROOT, capture_output=True,
                           text=True, check=True)
        return r.stdout.strip() or "unknown"
    except Exception:
        return "unknown"


# ---------------------------------------------------------------- model
def build():
    cfg = tomllib.loads(read("docs/progress.toml"))
    ov = cfg.get("override", {})
    groups = []  # dict(title, kind, items=[(name, status, ev)], extra)

    # 1 GPUs
    ladder = cfg["ladder"]
    rows = []
    for g in cfg["gpu"]:
        items = []
        for r in ladder:
            st = g[r["id"]]
            ev = list(r["ev"])
            if g["tsv"] and r["id"] in ("draw", "compute", "deqp"):
                ev.append(f"vk/deqp/ours-daily-sample-{g['tsv']}.tsv")
            if g.get("branch"):
                if st == "done":
                    die(f"gpu {g['id']}: on branch {g['branch']}, rungs cannot be 'done'")
                ev = [f"branch:{g['branch']}"]
            verify(ev, st, f"gpu {g['id']}.{r['id']}")
            items.append((f"{g['name']}: {r['label']}", st, ev))
        rows.append((f"{g['name']} ({g['arch']})", items))
    groups.append(dict(title="GPU generations (bring-up ladder)", kind="rows", rows=rows,
                       items=[i for _, its in rows for i in its]))

    # 2, 3 DXVK features and extensions
    feats, exts = derive_dxvk()

    def apply(lst):
        res = []
        for key, short, st in lst:
            ev = [f"vk/src/mali_physical_device.c#{short.removeprefix('VK_').split('_', 1)[-1] if key.startswith('ext.') else short}"] \
                if st == "done" else ["vk/tests/t_dxvk_caps.c"]
            if key in ov:
                st, ev = ov[key]["status"], ov[key]["ev"]
            verify(ev, st, key)
            res.append((key.split(".", 1)[1] if key.startswith("ext.") else key, st, ev))
        return res
    fi, ei = apply(feats), apply(exts)
    groups.append(dict(title="Vulkan core features DXVK needs", kind="grid", items=fi))
    groups.append(dict(title="Extensions DXVK needs", kind="grid", items=ei))

    # 4 dEQP
    deqp = []
    for g in cfg["gpu"]:
        if g["tsv"]:
            c = derive_deqp(g["tsv"])
            run = c["Pass"] + c["Fail"] + c["Other"]
            deqp.append((f"{g['name']} ({g['arch']})", c, c["Pass"] / run if run else 0.0))
        else:
            deqp.append((f"{g['name']} ({g['arch']})", None, 0.0))
    groups.append(dict(title="dEQP-VK daily sample (cases the driver supports)", kind="bars", bars=deqp, items=[]))

    # 5 pipeline
    pi = []
    for p in cfg["piece"]:
        verify(p["ev"], p["status"], f"piece '{p['name']}'")
        pi.append((p["name"], p["status"], p["ev"]))
    groups.append(dict(title="Pipeline pieces", kind="grid", items=pi))

    # 6 milestones
    mi = derive_stages()
    for m in cfg["milestone"]:
        verify(m["ev"], m["status"], f"milestone '{m['name']}'")
        mi.append((m["name"], m["status"], m["ev"]))
    groups.append(dict(title="Roadmap milestones", kind="grid", items=mi))
    return groups


def score(groups):
    items = [i for g in groups for i in g["items"]]
    full = sum(1 for i in items if i[1] == "done")
    half = sum(1 for i in items if i[1] == "partial")
    return len(items), full, half, round(100 * (full + 0.5 * half) / len(items))


# ---------------------------------------------------------------- SVG
BG, BORDER, TXT, DIM = "#0d1117", "#30363d", "#e6edf3", "#9da7b3"
COL = {"done": "#2ea043", "partial": "#a5e8b4", "none": "#30363d"}
FONT = "-apple-system,BlinkMacSystemFont,'Segoe UI',Helvetica,Arial,sans-serif"
PAD, B, P = 24, 20, 25  # padding, block size, block pitch


def block(x, y, st, tip, w=B):
    t = f"<title>{escape(tip)}</title>"
    extra = ""
    if st == "partial":
        extra = f' stroke="#2ea043" stroke-width="2"'
    return f'<rect x="{x}" y="{y}" width="{w}" height="{B}" rx="4" fill="{COL[st]}"{extra}>{t}</rect>'


def text(x, y, s, size=14, fill=TXT, weight="normal", anchor="start"):
    return (f'<text x="{x}" y="{y}" font-size="{size}" fill="{fill}" font-weight="{weight}" '
            f'text-anchor="{anchor}">{escape(s)}</text>')


def counts(items):
    d = sum(1 for i in items if i[1] == "done")
    p = sum(1 for i in items if i[1] == "partial")
    return d, p, len(items)


def render(groups, stamp):
    total, full, half, pct = score(groups)
    o = []
    y = PAD + 20
    o.append(text(PAD, y, "mali-open driver progress", 24, TXT, "bold"))
    o.append(text(W - PAD, y, f"{pct} %", 30, "#3fb950", "bold", "end"))
    o.append(text(W - PAD, y + 18, "of tracked items, not of the whole driver", 11, DIM, "normal", "end"))
    y += 24
    o.append(text(PAD, y, f"{full} of {total} items done, {half} partial (counted as half)", 14, DIM))
    y += 20
    o.append(f'<text x="{PAD}" y="{y}" font-size="12" fill="{DIM}">{escape("inputs last changed " + stamp)}</text>')
    y += 22
    # legend
    lx = PAD
    for st, lab in (("done", "done and tested"), ("partial", "partial or experimental"), ("none", "not yet")):
        o.append(block(lx, y - 14, st, lab, 16).replace(f'height="{B}"', 'height="16"'))
        o.append(text(lx + 22, y, lab, 12, DIM))
        lx += 22 + 8 * len(lab) + 14
    y += 26
    for g in groups:
        o.append(f'<line x1="{PAD}" y1="{y - 12}" x2="{W - PAD}" y2="{y - 12}" stroke="{BORDER}"/>')
        y += 12
        if g["kind"] == "bars":
            o.append(text(PAD, y, g["title"], 16, TXT, "bold"))
            y += 14
            for label, c, frac in g["bars"]:
                n = round(frac * 20)
                bx = PAD + 230
                o.append(text(PAD, y + 15, label, 14))
                for k in range(20):
                    st = "done" if k < n else "none"
                    tip = f"{label}: {round(frac * 100)} % of supported cases pass" if c else f"{label}: no dEQP result on main"
                    o.append(block(bx + k * 18, y, st, tip, 15))
                if c:
                    o.append(text(bx + 20 * 18 + 8, y + 15, f"{round(frac * 100)} %", 14, TXT, "bold"))
                    o.append(text(PAD, y + 31, f"{c['Pass']} pass, {c['Fail']} fail, {c['NotSupported']} not supported", 11, DIM))
                else:
                    o.append(text(bx + 20 * 18 + 8, y + 15, "no data", 13, DIM))
                    o.append(text(PAD, y + 31, "not on main yet", 11, DIM))
                y += 44
            y += 4
            continue
        d, p, n = counts(g["items"])
        o.append(text(PAD, y, g["title"], 16, TXT, "bold"))
        o.append(text(W - PAD, y, f"{d}/{n}" + (f" (+{p} partial)" if p else ""), 14, DIM, "normal", "end"))
        y += 14
        if g["kind"] == "rows":
            for label, items in g["rows"]:
                o.append(text(PAD, y + 15, label, 14))
                for k, (name, st, ev) in enumerate(items):
                    o.append(block(PAD + 190 + k * P, y, st, name, B))
                dd, pp, nn = counts(items)
                o.append(text(W - PAD, y + 15, f"{dd}/{nn}", 13, DIM, "normal", "end"))
                y += P + 4
            y += 6
        else:
            per = (W - 2 * PAD + (P - B)) // P
            for k, (name, st, ev) in enumerate(g["items"]):
                o.append(block(PAD + (k % per) * P, y + (k // per) * P, st, name, B))
            y += ((len(g["items"]) - 1) // per + 1) * P + 10
    o.append(f'<line x1="{PAD}" y1="{y - 12}" x2="{W - PAD}" y2="{y - 12}" stroke="{BORDER}"/>')
    o.append(text(PAD, y + 6, "Software checks run offline on a fake GPU; phone results come from the maintainer's own devices.", 11, DIM))
    h = y + 24
    head = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{h}" viewBox="0 0 {W} {h}" '
            f'font-family="{FONT}" role="img" aria-labelledby="t d">\n'
            f'<title id="t">mali-open driver progress: {pct} percent</title>'
            f'<desc id="d">{full} of {total} checklist items done, {half} partial. Details in docs/progress.md.</desc>\n'
            f'<rect x="0.5" y="0.5" width="{W - 1}" height="{h - 1}" rx="10" fill="{BG}" stroke="{BORDER}"/>\n')
    return head + "\n".join(o) + "\n</svg>\n"


def markdown(groups):
    s = ["# Progress details", "", "Generated by `tools/progress.py` from `docs/progress.toml` and the tree. Do not edit.", ""]
    sym = {"done": "done", "partial": "partial", "none": "not yet"}
    for g in groups:
        s += [f"## {g['title']}", ""]
        if g["kind"] == "bars":
            s += ["| GPU | Pass | Fail | Not supported |", "|---|---|---|---|"]
            for label, c, frac in g["bars"]:
                s.append(f"| {label} | {c['Pass']} | {c['Fail']} | {c['NotSupported']} |" if c else f"| {label} | - | - | - |")
            s.append("")
            continue
        s += ["| Item | State | Evidence |", "|---|---|---|"]
        for name, st, ev in g["items"]:
            s.append(f"| {name} | {sym[st]} | {', '.join(f'`{e}`' for e in ev)} |")
        s.append("")
    return "\n".join(s)


def main():
    groups = build()
    svg = render(groups, commit_stamp())
    md = markdown(groups)
    if "--check" in sys.argv[1:]:
        bad = []
        for path, new in ((SVG, svg), (MD, md)):
            old = path.read_text() if path.exists() else ""
            if STAMP_RE.sub("", old) != STAMP_RE.sub("", new):
                bad.append(str(path.relative_to(ROOT)))
        if bad:
            print("progress.py: stale: " + ", ".join(bad) + " (run tools/progress.py and commit)")
            sys.exit(1)
        print("progress.py: up to date")
        return
    SVG.write_text(svg)
    MD.write_text(md)
    _, full, half, pct = score(groups)
    print(f"wrote {SVG.relative_to(ROOT)}: {pct} % ({full} done, {half} partial)")


if __name__ == "__main__":
    main()
