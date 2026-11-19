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

def _name(line):
    for name, pat in PATTERNS:
        if re.search(pat, line, re.I):
            return name
    return None

def classify_driver(text):
    """Return (driver, evidence-line). Loaded-driver records (driverName/driverInfo/library_path/.so/vkBridge) outrank configuration hints
    (VK_DRIVER_FILES=, VK_ICD_FILENAMES=); records that disagree with each other, or with a hint, give ('conflict', line) instead of the first hit."""
    loaded, hints, unrec = [], [], []
    for line in text.splitlines():
        n = _name(line)
        is_hint = re.search(r"VK_(DRIVER_FILES|ICD_FILENAMES)=.+", line, re.I)
        is_loaded = not is_hint and re.search(r"driverName|driverInfo|library_path|\.so\b|vkBridge", line, re.I)
        if not n:
            if is_loaded:
                unrec.append(line.strip())   # a loaded record we cannot name still outranks every hint
            continue
        if is_loaded:
            loaded.append((n, line.strip()))
        elif is_hint:
            hints.append((n, line.strip()))
    if unrec and not loaded:
        return "unknown", unrec[0][:300]   # only unrecognized loaded records: never fall back to a configuration hint
    recs = loaded or hints
    if not recs:
        return "unknown", None
    names = {n for n, _ in recs} | ({n for n, _ in hints} if loaded else set())
    if len(names) > 1:
        return "conflict", "; ".join(sorted({l for _, l in recs + (hints if loaded else [])}))[:300]
    return recs[0]

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

def temps(d, problems):
    out = {}
    for k in ("thermal_pre", "thermal_post"):
        f = d / f"{k}.json"
        try:
            out[k] = json.loads(f.read_text()).get("batteryC") if f.exists() else None
        except (ValueError, AttributeError, OSError) as e:
            out[k] = None
            problems.append(f"damaged {f.name}: {type(e).__name__}")
    for k, v in list(out.items()):
        if v is not None and (isinstance(v, bool) or not isinstance(v, (int, float))):
            out[k] = None
            problems.append(f"damaged {k}.json: batteryC is not a number")
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
        try:
            p = json.loads(pf.read_text())
            fo = p.get("fpsOnScreen")
            if fo is not None and (isinstance(fo, bool) or not isinstance(fo, (int, float))):
                raise TypeError("fpsOnScreen is not a number")
            rd.update(pacing_stats(p))
            if not p.get("ok", True):
                rd["problems"].append("pacing not ok")
            if not rd.get("intervals"):
                rd["problems"].append("no measured intervals in pacing.json")
        except (ValueError, AttributeError, TypeError, OSError) as e:
            rd["problems"].append(f"damaged pacing.json: {type(e).__name__}")
    else:
        rd["problems"].append("no pacing.json")
    rd.update(temps(d, rd["problems"]))
    if rd["driver"] == "unknown":
        rd["problems"].append("NO DRIVER EVIDENCE: do not attribute this run to any driver")
    elif rd["driver"] == "conflict":
        rd["problems"].append("CONFLICTING DRIVER EVIDENCE: " + (rd["driver_evidence"] or ""))
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
