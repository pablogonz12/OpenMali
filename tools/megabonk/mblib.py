#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Helpers for tools/megabonk/run.sh (pure functions, unit-tested in test_megabonk.py).

  mblib.py evidence <file> [--allow-unknown]   classify driver evidence; print the DRIVER-EVIDENCE line; exit 0 only for kind=ours
  mblib.py stats <pacing.json> <secs>   avg fps, 1% low, % frames <= 8.3 ms from `droiddeckctl pacing` output
  mblib.py errors <log> [n]         first n DXVK/Vulkan device-creation error lines

Evidence file = lines of `key=value` (driver_pref, debug_pref, vkBridge, driverName, icd, env=K=V) plus any raw guest/DXVK log
text; "Found device: NAME (DRIVER VER)" lines from the DXVK log count as driverName evidence. Same classification as MediaDeck's
scripts/quick/driverinfo.py: ours | bridge | panvk | unknown | conflict. Only `ours` may report fps.
"""
import json, re, sys

ERR_RE = re.compile(r"(err:|error:|vkCreateDevice|VK_ERROR_|DXVK_MISSING|Failed to create|not supported|missing (required )?feature|"
                    r"Required feature|geometryShader|shaderCullDistance|robustBufferAccess|DEVICE_LOST|Segmentation fault|SIGSEGV)", re.I)


def kind_text(s):
    s = (s or "").lower()
    if not s:
        return None
    if re.search(r"bridge|droidbridge", s):
        return "bridge"
    if re.search(r"panvk|panfrost", s):
        return "panvk"
    if re.search(r"mali[-_]?open|maliopen|mali_icd", s):
        return "ours"
    return None


def parse_evidence(text):
    d = {"driver_pref": None, "debug_pref": None, "vkBridge": None, "icd": None, "driverNames": [], "env": []}
    for raw in text.splitlines():
        line = raw.strip()
        m = re.match(r"(driver_pref|debug_pref|vkBridge|icd)=(.*)$", line)
        if m:
            k, v = m.group(1), m.group(2).strip()
            d[k] = (v.lower() == "true") if k == "vkBridge" and v.lower() in ("true", "false") else (v or None)
            continue
        m = re.match(r"driverName\s*=\s*(.+)$", line)
        if m:
            d["driverNames"].append(m.group(1).strip())
            continue
        m = re.search(r"Found device:\s*(.+)$", line)      # DXVK log: "info:  Vulkan: Found device: <name> (<driver> <version>)"
        if m:
            d["driverNames"].append(m.group(1).strip())
            continue
        m = re.match(r"env=(.+)$", line)
        if m:
            d["env"].append(m.group(1).strip())
        for k in ("VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES"):
            mm = re.search(k + r"=(\S+)", line)
            if mm and not d["icd"]:
                d["icd"] = mm.group(1).split(":")[0]
    return d


def pref_kind(s):
    s = (s or "").strip().lower()
    return {"ours": "ours", "bridge": "bridge", "panvk": "panvk"}.get(s)


def classify(d):
    """The driver pref is only the SELECTION: it can contradict the proof (conflict) but never prove `ours` alone.
    Proof = a loaded ICD path or a driverName (vulkaninfo / DXVK "Found device") naming mali-open."""
    proof = {k for k in [kind_text(d["icd"])] + [kind_text(n) for n in d["driverNames"]] if k}
    pref = pref_kind(d["driver_pref"])
    votes = proof | ({pref} if pref else set())
    if len(votes) > 1:
        kind = "conflict"
    elif proof:
        kind = proof.pop()
    elif pref in ("bridge", "panvk"):
        kind = pref
    elif d["vkBridge"] is True and not d["icd"]:
        kind = "bridge"
    else:
        kind = "unknown"
    if kind in ("ours", "panvk") and d["vkBridge"] is True:
        kind = "conflict"            # vkBridge=true routes Vulkan through MaliBridge whatever the ICD says
    if kind == "ours" and d["debug_pref"] not in (None, "on", "true"):
        kind = "conflict"            # MediaDeck ignores the driver selection while Debug is off
    return kind


def evidence_line(d, kind):
    return (f"DRIVER-EVIDENCE kind={kind} driver_pref={d['driver_pref']} debug={d['debug_pref']} vkBridge={d['vkBridge']} "
            f"icd={d['icd']} driverName={'; '.join(d['driverNames']) or None}")


def cmd_evidence(path, allow_unknown=False):
    text = open(path, errors="replace").read()
    d = parse_evidence(text)
    kind = classify(d)
    print(evidence_line(d, kind))
    if kind == "unknown" and allow_unknown and d["driver_pref"] == "ours":
        print("pre-launch: selection is ours, proof (driverName / DXVK Found device) comes after the launch")
        return 0
    if kind != "ours":
        print(f"REFUSED: driver evidence says {kind}, not ours; no fps will be reported (a number from MaliBridge/PanVK is not our driver's)")
        return 3
    return 0


def stats(pacing, secs):
    h = {int(k): int(v) for k, v in (pacing.get("presentToPresentMs") or {}).items()}
    n = sum(h.values())
    if not n:
        return None
    frames = pacing.get("frames") or n
    acc, p99 = 0, max(h)
    for ms in sorted(h):                      # 99th percentile interval (1 ms bins from SurfaceFlinger's histogram)
        acc += h[ms]
        if acc >= 0.99 * n:
            p99 = ms
            break
    fast = sum(c for ms, c in h.items() if ms <= 8)     # bin resolution is 1 ms: "<= 8.3 ms" = bins 0..8
    return {"frames": frames, "secs": secs, "avg_fps": frames / secs, "low1_fps": 1000.0 / max(p99, 1), "p99_ms": p99,
            "pct_le_8_3ms": 100.0 * fast / n}


def cmd_stats(path, secs):
    s = stats(json.load(open(path)), float(secs))
    if not s:
        print("NO-FRAMES: pacing recorded no frames of the game layer")
        return 4
    print(f"avg fps {s['avg_fps']:.1f} ({s['frames']} frames / {s['secs']:.0f} s) | 1% low {s['low1_fps']:.1f} fps (p99 interval {s['p99_ms']} ms) | "
          f"frames <= 8.3 ms {s['pct_le_8_3ms']:.1f} % (1 ms bins: <= 8 ms)")
    return 0


def first_errors(text, n=12):
    out = []
    for l in text.splitlines():
        if ERR_RE.search(l) and l.strip() not in out:
            out.append(l.strip())
        if len(out) >= n:
            break
    return out


def main(a):
    if len(a) >= 2 and a[0] == "evidence":
        return cmd_evidence(a[1], "--allow-unknown" in a[2:])
    if len(a) >= 3 and a[0] == "stats":
        return cmd_stats(a[1], a[2])
    if len(a) >= 2 and a[0] == "errors":
        for l in first_errors(open(a[1], errors="replace").read(), int(a[2]) if len(a) > 2 else 12):
            print("  " + l[:300])
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
