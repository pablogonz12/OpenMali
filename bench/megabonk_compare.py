"""Parse Megabonk bench run dirs (mali-gaming scripts/bench.sh layout) and compare drivers.

Run dir files: label.txt (expected driver: ours|bridge|panvk), pacing.json (droiddeckctl pacing),
thermal_pre.json / thermal_post.json (droiddeckctl thermal), driver-setup-tail.txt and optional
driver-identity.txt (vulkaninfo / ICD json from the guest). Parsing only; never talks to a phone.
"""
import json, re, sys
from pathlib import Path

PATTERNS = [  # order matters: first hit wins; checked against loaded-ICD evidence only
    ("ours", r"mali[-_]open|libvulkan_mali_open"),
    ("bridge", r"vkbridge|malibridge|libvkbridge"),
    ("panvk", r"panvk|libvulkan_panfrost|panfrost"),
]

def classify_driver(text):
    """Return (driver, evidence-line) from loaded-ICD evidence, or ('unknown', None)."""
    for line in text.splitlines():
        if re.search(r"driverName|driverInfo|library_path|\.so\b|VK_(DRIVER_FILES|ICD_FILENAMES)=.+|vkBridge", line, re.I):
            for name, pat in PATTERNS:
                if re.search(pat, line, re.I):
                    return name, line.strip()
    return "unknown", None

def pacing_stats(p, gap_ms=25):
    """fps, 1% low and gap counts from the present-to-present histogram (bins are integer ms)."""
    h = {int(k): int(v) for k, v in p.get("presentToPresentMs", {}).items()}
    n = sum(h.values())
    if not n:
        return {"fps": p.get("fpsOnScreen"), "intervals": 0, "low1pct_fps": None, "gaps_gt25": 0, "max_gap_ms": None}
    # 1% low = 1000 / p99 interval
    acc, p99 = 0, None
    for ms in sorted(h):
        acc += h[ms]
        if acc >= 0.99 * n:
            p99 = ms
            break
    return {"fps": p.get("fpsOnScreen"), "intervals": n, "p99_ms": p99,
            "low1pct_fps": round(1000.0 / p99, 1) if p99 else None,
            "gaps_gt25": sum(v for k, v in h.items() if k > gap_ms),
            "max_gap_ms": max(h), "offCadencePercent": p.get("offCadencePercent")}

def temps(d):
    out = {}
    for k in ("thermal_pre", "thermal_post"):
        f = d / f"{k}.json"
        out[k] = json.loads(f.read_text()).get("batteryC") if f.exists() else None
    t = [v for v in out.values() if v is not None]
    out["max_c"] = max(t) if t else None
    return out

def load_run(d):
    d = Path(d)
    rd = {"dir": str(d), "expected": None, "problems": []}
    lf = d / "label.txt"
    rd["expected"] = lf.read_text().strip() if lf.exists() else None
    ev = "\n".join((d / n).read_text() for n in ("driver-identity.txt", "driver-setup-tail.txt") if (d / n).exists())
    rd["driver"], rd["driver_evidence"] = classify_driver(ev)
    pf = d / "pacing.json"
    if pf.exists():
        p = json.loads(pf.read_text())
        rd.update(pacing_stats(p))
        if not p.get("ok", True):
            rd["problems"].append("pacing not ok")
    else:
        rd["problems"].append("no pacing.json")
    rd.update(temps(d))
    if rd["driver"] == "unknown":
        rd["problems"].append("NO DRIVER EVIDENCE: do not attribute this run to any driver")
    elif rd["expected"] and rd["expected"] != rd["driver"]:
        rd["problems"].append(f"driver mismatch: expected {rd['expected']}, evidence says {rd['driver']}")
    if rd.get("fps") is not None and rd["fps"] < 5:
        rd["problems"].append("fps<5: menu/confirm screen? run likely invalid")
    return rd

def table(runs):
    cols = ["driver", "fps", "low1pct_fps", "gaps_gt25", "max_gap_ms", "max_c"]
    lines = ["| run | " + " | ".join(cols) + " | problems |", "|---|" + "---|" * (len(cols) + 1)]
    for r in runs:
        lines.append(f"| {Path(r['dir']).name} | " + " | ".join(str(r.get(c)) for c in cols)
                     + " | " + ("; ".join(r["problems"]) or "-") + " |")
    return "\n".join(lines)

if __name__ == "__main__":
    runs = [load_run(a) for a in sys.argv[1:]]
    print(table(runs))
    sys.exit(1 if any(r["problems"] for r in runs) else 0)
