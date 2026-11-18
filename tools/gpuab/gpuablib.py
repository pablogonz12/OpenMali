"""Pure helpers of gpuab.py (host-testable, no adb)."""
import re, statistics as st

def rotate(variants, r):
    """Variant order for round r: rotated so no variant is always first or last."""
    k = r % len(variants)
    return variants[k:] + variants[:k]

def quantile(x, f):
    x = sorted(x); k = (len(x) - 1) * f; lo = int(k); hi = min(lo + 1, len(x) - 1)
    return x[lo] + (x[hi] - x[lo]) * (k - lo)

def iqr(x): return quantile(x, .75) - quantile(x, .25)

def parse(txt, stop_temp):
    """RUN/fps/SUM log -> {(scene, variant): [(round, gpu_ms, fps)]}; runs hotter than stop_temp are dropped."""
    res = {}; cur = None; fps = None
    for line in txt.split("\n"):
        m = re.search(r"RUN (\S+) (\S+) (\d+) temp=(\d+)", line)
        if m:
            t = int(m[4]); cur = (m[1], m[2], int(m[3]), t / 10 if t > 100 else t); fps = None; continue
        m = re.search(r'"fps":([0-9.]+)', line)
        if m and cur: fps = float(m[1])
        m = re.search(r"SUM gpu=([0-9.]+)", line)
        if m and cur and fps is not None and cur[3] <= stop_temp:
            res.setdefault((cur[1], cur[0]), []).append((cur[2], float(m[1]), fps))
    return res

def summarize(res, scenes, names):
    """-> {(scene, variant): dict(n, med, iqr, fps, ratio)}; ratio = median over rounds of gpu / gpu of names[0] in the same round."""
    out = {}
    for sc in scenes:
        b = {r: g for r, g, f in res.get((sc, names[0]), [])}
        for n in names:
            rows = res.get((sc, n), [])
            if not rows: continue
            g = [x[1] for x in rows]
            pr = [x[1] / b[x[0]] for x in rows if b.get(x[0])]
            out[(sc, n)] = dict(n=len(g), med=st.median(g), iqr=iqr(g), fps=st.median(x[2] for x in rows),
                                ratio=st.median(pr) if pr else float("nan"))
    return out
