# SPDX-License-Identifier: MIT
"""Idle sweep storage: the full dEQP-VK mustpass list walked in chunks on idle phones, results kept compactly per GPU class.
  <state>/sweep/<class>.json         progress + totals + per-group totals of the current pass (+ finished passes)
  <state>/sweep/<class>-chunks.jsonl one line per chunk: start index, commit, .so sha, counts, temps, zlib+base64 status string (P F N C T K O per case)
  <state>/sweep/<class>-fails.tsv    name<TAB>status<TAB>commit8 of the current pass's non-Pass, non-NotSupported cases (Fail/Crash/Timeout/Other)
Nothing here talks to a phone: farmd feeds it; `farm sweep status|failures` reads it."""
import base64, json, os, re, time, zlib

import farmlib as L

CODE = {"Pass": "P", "QualityWarning": "P", "CompatibilityWarning": "P", "Fail": "F", "NotSupported": "N", "Crash": "C", "ResourceHog": "T"}
NAME = {"P": "Pass", "F": "Fail", "N": "NotSupported", "C": "Crash", "T": "Timeout", "K": "Skipped", "O": "Other"}
RESTART_AFTER_COMMITS = 25                    # main moved by more than this many commits since the pass began -> start over
STRESS = re.compile(r"object_management\.(alloc_callback_fail|max_|multithreaded)|many")     # same filter as the daily sample: hangs the phone


def sdir():
    d = os.path.join(L.state_dir(), "sweep")
    os.makedirs(d, exist_ok=True)
    return d


def listfiles(main_checkout):
    """-> [path] of the mustpass case files (env MALI_FARM_SWEEP_LIST: one list file, for tests)"""
    if os.environ.get("MALI_FARM_SWEEP_LIST"):
        return [os.environ["MALI_FARM_SWEEP_LIST"]]
    base = os.path.join(main_checkout, "third_party", "cts-src", "external", "vulkancts", "mustpass", "main")
    idx = os.path.join(base, "vk-default.txt")
    if not os.path.exists(idx):
        return []
    return [os.path.join(base, l.strip()) for l in open(idx) if l.strip()]


class CaseList:
    """the full list without holding 1.4M names in memory: per-file line counts (cached by size+mtime), slices read on demand"""
    def __init__(self, files):
        self.files = files
        meta_p = os.path.join(sdir(), "listmeta.json")
        sig = [[f, os.path.getsize(f), int(os.path.getmtime(f))] for f in files]
        meta = L.read_json(meta_p) if os.path.exists(meta_p) else {}
        if meta.get("sig") != sig:
            meta = {"sig": sig, "counts": [sum(1 for l in open(f) if l.strip()) for f in files]}
            L.write_json(meta_p, meta)
        self.counts, self.sig = meta["counts"], sig
        self.n = sum(self.counts)

    def slice(self, i, n):
        out, base = [], 0
        for f, c in zip(self.files, self.counts):
            if base + c > i and len(out) < n:
                with open(f) as fh:
                    k = 0
                    for l in fh:
                        l = l.strip()
                        if not l:
                            continue
                        if base + k >= i:
                            out.append(l)
                            if len(out) >= n:
                                break
                        k += 1
            base += c
            if len(out) >= n:
                break
        return out


def st_path(cls):
    return os.path.join(sdir(), cls + ".json")


def load(cls):
    p = st_path(cls)
    if os.path.exists(p):
        return L.read_json(p)
    return {"pos": 0, "n": 0, "commit": None, "started": None, "counts": {}, "groups": {}, "chunks": 0, "stall": 0, "passes": []}


def save(cls, st):
    L.write_json(st_path(cls), st)


def group(name):
    p = name.split(".")
    return ".".join(p[1:3]) if len(p) > 3 else ".".join(p[1:])


def start_pass(cls, st, commit, n, why=""):
    """finish the old pass (kept in `passes`), begin a new one at index 0 on `commit`"""
    if st.get("commit") and st.get("pos"):
        st["passes"].append({"commit": st["commit"], "started": st["started"], "ended": L.now(), "pos": st["pos"], "n": st["n"], "counts": st["counts"], "why": why or "finished"})
        st["passes"] = st["passes"][-5:]
    st.update(pos=0, n=n, commit=commit, started=L.now(), counts={}, groups={}, chunks=0, stall=0)
    try:
        os.unlink(os.path.join(sdir(), cls + "-fails.tsv"))
    except OSError:
        pass
    return st


def record(cls, st, start, names, codes, meta):
    """append one finished chunk (codes: one char per name), update totals; advance pos"""
    for nm, c in zip(names, codes):
        st["counts"][c] = st["counts"].get(c, 0) + 1
        g = st["groups"].setdefault(group(nm), {})
        g[c] = g.get(c, 0) + 1
    fl = [f"{nm}\t{c}\t{(st['commit'] or '')[:8]}\n" for nm, c in zip(names, codes) if c in "FCTO"]
    if fl:
        with open(os.path.join(sdir(), cls + "-fails.tsv"), "a") as f:
            f.writelines(fl)
    cnt = {}
    for c in codes:
        cnt[c] = cnt.get(c, 0) + 1
    rec = dict(meta, i=start, n=len(names), commit=(st["commit"] or "")[:8], t=L.now(), counts=cnt,
               z=base64.b64encode(zlib.compress("".join(codes).encode(), 9)).decode())
    with open(os.path.join(sdir(), cls + "-chunks.jsonl"), "a") as f:
        f.write(json.dumps(rec) + "\n")
    st["pos"] = start + len(names)
    st["chunks"] += 1
    save(cls, st)


def code_of(status, note=""):
    if status == "ResourceHog" or (status == "Crash" and re.search(r"timeout|hang|watchdog", note or "", re.I)):
        return "T"
    return CODE.get(status, "O")


def fails(cls, group_sub="", cap=40):
    p = os.path.join(sdir(), cls + "-fails.tsv")
    rows = [l.rstrip("\n").split("\t") for l in open(p)] if os.path.exists(p) else []
    rows = [r for r in rows if group_sub in r[0]]
    return rows, cap


def status_lines(classes):
    out = []
    for cls in classes:
        st = load(cls)
        if not st.get("n"):
            out.append(f"{cls}: not started")
            continue
        c = st["counts"]
        done = st["pos"]
        out.append(f"{cls}: {100 * done / st['n']:.1f}% of {st['n']} ({done}) on {st['commit'][:7]} since {st['started'][5:16]}; "
                   + " ".join(f"{NAME[k]} {c.get(k, 0)}" for k in "PFNCTK" if c.get(k)) + (f"; {len(st['passes'])} earlier pass(es)" if st["passes"] else ""))
        bad = sorted(((sum(v.get(k, 0) for k in "FCTO"), g) for g, v in st["groups"].items()), reverse=True)
        out.append("   top failing: " + ", ".join(f"{g} {n}" for n, g in bad[:3] if n) if any(n for n, _ in bad) else "   no failing groups yet")
    return out


def top_groups(cls, cap=10):
    st = load(cls)
    bad = sorted(((sum(v.get(k, 0) for k in "FCTO"), g, v) for g, v in st["groups"].items()), reverse=True)
    return [(g, n, sum(v.values())) for n, g, v in bad[:cap] if n]
