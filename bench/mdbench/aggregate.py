#!/usr/bin/env python3
"""aggregate.py run1.json [run2.json ...]  -> summary JSON line (median of per-run fps / p99 / gpu / cpu; auto-scale: median score) + text table.
Reads the mdbench JSON (one object, key "mdbench"); skips empty / failed files. Use -o file to write the JSON too."""
import json, statistics as st, sys

def load(p):
    try:
        t = open(p).read().strip()
        d = json.loads(t.splitlines()[-1]) if t else None
        return d if d and d.get("pass") and "frame_time_ms" in d["mdbench"] else None
    except Exception:
        return None

def med(v):
    v = [x for x in v if x is not None]
    return st.median(v) if v else None

def main(a):
    out = None
    if "-o" in a:
        i = a.index("-o"); out = a[i + 1]; del a[i:i + 2]
    runs = [r for r in (load(p) for p in a) if r]
    if not runs:
        print(json.dumps({"mdbench_summary": {"runs": 0, "error": "no valid runs"}})); return 1
    m = [r["mdbench"] for r in runs]
    def g(f): return [f(x) for x in m]
    s = {"runs": len(m), "requested": len(a), "scene": m[0]["config"]["scene"], "device": m[0]["device"]["name"], "driver": m[0]["device"].get("driver_info") or m[0]["device"]["driver_version_hex"],
         "icd": m[0]["device"]["icd"] or m[0]["device"]["loader"], "present_mode": m[0]["present_mode"], "wsi": m[0]["wsi"], "load": m[0]["load"],
         "fps": {"median": med(g(lambda x: x["fps"])), "min": min(g(lambda x: x["fps"])), "max": max(g(lambda x: x["fps"]))},
         "fps_1pct_low_median": med(g(lambda x: x["fps_1pct_low"])), "frame_p50_ms": med(g(lambda x: x["frame_time_ms"]["all"]["p50"])),
         "frame_p99_ms": med(g(lambda x: x["frame_time_ms"]["all"]["p99"])), "over_25ms_total": sum(g(lambda x: x["over_25ms"])),
         "gpu_ms_mean": med(g(lambda x: (x["gpu_ms"] or {}).get("mean"))), "cpu_submit_ms_mean": med(g(lambda x: x["cpu_submit_ms_mean"])),
         "cpu_busy_ms_mean": med(g(lambda x: x["cpu_busy_ms_mean"]))}
    if all("auto_scale" in x for x in m):
        sc = g(lambda x: x["score"]); s["score_level"] = {"median": med(sc), "min": min(sc), "max": max(sc)}
        s["score_scale_median"] = med(g(lambda x: x["auto_scale"]["score_scale"]))
    line = json.dumps({"mdbench_summary": s})
    print(line)
    if out: open(out, "w").write(line + "\n")
    f = lambda v, p=2: "n/a" if v is None else f"{v:.{p}f}"
    print(f"{s['scene']:<10} {s['device']} [{s['icd']}] {s['wsi']}/{s['present_mode']}  runs {s['runs']}/{s['requested']}  fps {f(s['fps']['median'], 1)} (min {f(s['fps']['min'], 1)})  1%low {f(s['fps_1pct_low_median'], 1)}  "
          f"p99 {f(s['frame_p99_ms'])} ms  >25ms {s['over_25ms_total']}  gpu {f(s['gpu_ms_mean'])} ms  cpu-submit {f(s['cpu_submit_ms_mean'], 3)} ms" +
          (f"  SCORE level {s['score_level']['median']} (x{f(s['score_scale_median'])})" if "score_level" in s else ""))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
