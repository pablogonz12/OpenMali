#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generated status dashboard for the mali-open Vulkan driver.

  tools/status/status.py            write docs/STATUS.md, docs/status.json and append docs/status-history.jsonl
  tools/status/status.py --check    exit 1 if regenerating would change a committed file (no writes to history)

Every number comes from this tree or from a run of the built ICD. Nothing here is hand-written into the
page: change the driver (or a test), rerun, and the panels move. Data sources are named in the header and
in the footer of docs/STATUS.md, with file:line for each one.

Panels
  entry points  mali_<Name> definitions in vk/src/*.c        vs core commands in vk/tests/dxvk_api_table.h
  extensions    the instance/device extension tables         vs advertised + DXVK's required list
  formats       vkGetPhysicalDeviceFormatProperties (fake GPUs) vs VK_FORMAT_ASTC_12x12_SRGB_BLOCK (184)
  features      the ONE feature table in mali_physical_device.c vs the 55+15 core 1.0/1.3 feature bits
  gate          vk/check.sh results                          case counts split v11/v13
  shader gaps   no data: tools/shaderdb records timings, not unsupported ops (panel is omitted, see footer)

Python 3.11+, stdlib only, deterministic: the same tree and the same built ICD give the same bytes.
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MD = ROOT / "docs" / "STATUS.md"
JSONF = ROOT / "docs" / "status.json"
HIST = ROOT / "docs" / "status-history.jsonl"
ICD = ROOT / "vk" / "out" / "native" / "libvulkan_mali.so"
FEATURES = ROOT / "tools" / "status" / "features-vk13.txt"

# Vulkan 1.3 core formats are 1..184 (VK_FORMAT_ASTC_12x12_SRGB_BLOCK): the denominator of the format panel.
# mali_format.c scans 1..156 only and documents ASTC (157..184) as a known gap.
FMT_CORE13_LAST = 184
FMT_DRIVER_LAST = 156

VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT = 0x1
VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT = 0x2
VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT = 0x80
VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT = 0x100
VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT = 0x200

# DXVK 3.x owner tags in dxvk_api_table.h that mean "core Vulkan 1.x", not an extension.
CORE_OWNERS = ("VK_BASE_VERSION", "VK_GRAPHICS_VERSION", "VK_COMPUTE_VERSION")


def die(msg):
    sys.exit("status.py: " + msg)


def read(rel):
    p = ROOT / rel
    if not p.is_file():
        die(f"missing input {rel}")
    return p.read_text()


def git(*args):
    try:
        return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def ranges(nums):
    """[1,2,3,7,9,10] -> '1-3, 7, 9-10'."""
    out, i = [], 0
    while i < len(nums):
        j = i
        while j + 1 < len(nums) and nums[j + 1] == nums[j] + 1:
            j += 1
        out.append(str(nums[i]) if i == j else f"{nums[i]}-{nums[j]}")
        i = j + 1
    return ", ".join(out)


def pct(have, total):
    return None if not total else round(100.0 * have / total, 1)


def fmt_pct(have, total):
    if total is None or have is None:
        return "unknown"
    if total == 0:
        return "n/a"
    return f"{100.0 * have / total:.1f}%"


# ------------------------------------------------------------------ entry points
def so_symbols():
    """Defined symbols of the built ICD (nm), or None when there is no build."""
    if not ICD.is_file():
        return None
    try:
        out = subprocess.run(["nm", str(ICD)], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    syms = set()
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in ("T", "t", "W", "w"):
            syms.add(p[2])
    return syms


def panel_entrypoints():
    """mali_<Name> definitions in vk/src/*.c (or the built ICD), against the core command table the tests use."""
    defs = {}
    for c in sorted((ROOT / "vk" / "src").glob("*.c")):
        for m in re.finditer(r"^mali_(?:v10_|v11_|v13_)?(\w+)\s*\(", c.read_text(errors="replace"), re.M):
            defs.setdefault(m.group(1), c.name)
    table = read("vk/tests/dxvk_api_table.h")
    rows = re.findall(r'\{"(vk\w+)", "([^"]+)", \d, "([^"]*)"\}', table)
    core = [n for n, owner, _ in rows if owner.startswith(CORE_OWNERS)]
    syms = so_symbols()

    def sym_match(name, prefix):
        """Symbol for a command, allowing a numbered/KHR alias (vkCmdBindVertexBuffers2 -> ...Buffers3KHR)."""
        base = name[2:]
        if f"{prefix}_{base}" in syms:
            return True
        if base and base[-1].isdigit():
            return any(s.startswith(f"{prefix}_{base[:-1]}") for s in syms)
        return False

    if syms is not None:
        have = [n for n in core if sym_match(n, "mali")]
        runtime = [n for n in core if n not in have and sym_match(n, "vk_common")]
    else:
        have = [n for n in core if n[2:] in defs]
        runtime = None
    missing = [n for n in core if n not in have and (runtime is None or n not in runtime)]
    all_cmds = [n for n, _, _ in rows]
    answered = len(have) + (len(runtime) if runtime is not None else 0)
    return {
        "have": len(have),
        "total": len(core),
        "pct": pct(len(have), len(core)),
        "runtime": None if runtime is None else len(runtime),
        "runtime_names": runtime or [],
        "answered": answered if runtime is not None else None,
        "answered_pct": pct(answered, len(core)) if runtime is not None else None,
        "missing": missing,
        "have_names": have,
        "definitions": len(defs),
        "table_total": len(all_cmds),
        "method": "nm on vk/out/native/libvulkan_mali.so (mali_<Name> strong, vk_common_<Name> for the Mesa runtime)"
                  if runtime is not None else "source scan of vk/src/*.c (no built ICD: runtime column unknown)",
        "source": "vk/src/*.c:^mali_<Name>( (or nm on the ICD) vs vk/tests/dxvk_api_table.h core rows",
    }


# ------------------------------------------------------------------ extensions
def ext_entries(rel, head):
    """Parse a vk_*_extension_table initialiser into {VK_name: (bool, comment, conditional)}."""
    src = read(rel)
    i = src.find(head)
    if i < 0:
        die(f"{rel}: cannot find '{head}'")
    m = re.search(r"\n\s*\};", src[i:])
    if not m:
        die(f"{rel}: unterminated table at '{head}'")
    block = src[i:i + m.start()]
    out = {}
    cond = False
    for line in block.splitlines():
        s = line.strip()
        if re.match(r"#if(def)?\b", s):
            cond = True
        elif re.match(r"#(else|endif|elif)\b", s):
            cond = False
        cm = re.search(r"/\*.*", line)
        comment = cm.group(0) if cm else ""
        for m in re.finditer(r"\.(\w+)\s*=\s*(true|false)\s*,?", line):
            name = m.group(1)
            name = name if name.startswith("VK_") else "VK_" + name
            out[name] = (m.group(2) == "true", comment, cond)
    return out


def panel_extensions():
    inst = ext_entries("vk/src/mali_instance.c", "mali_instance_extensions = {")
    dev = ext_entries("vk/src/mali_physical_device.c", "const struct vk_device_extension_table extensions = {")
    adv = {}
    for d in (inst, dev):
        for k, v in d.items():
            adv[k] = v if k not in adv else (v[0], v[1] or adv[k][1], v[2] or adv[k][2])

    dxvk = re.search(r"static const char \*const dxvk_exts\[\] = \{(.*?)\};",
                     read("vk/tests/t_dxvk_caps.c"), re.S)
    required = re.findall(r'"(VK_\w+)"', dxvk.group(1)) if dxvk else []
    reference = sorted(set(adv) | set(required))

    PARTIAL_RE = re.compile(r"\b((?:[A-Za-z0-9_*]*[A-Z][A-Za-z0-9_*]*)\s+only|partial|smoke|experimental)\b")
    cells, partial, missing, supported = {}, [], [], []
    for name in reference:
        a = adv.get(name)
        if not a or not a[0]:
            cells[name] = "missing"
            missing.append(name)
        elif PARTIAL_RE.search(a[1]):
            cells[name] = "partial"
            kw = PARTIAL_RE.search(a[1]).group(0)
            partial.append((name, kw, a[1].strip(" ,*/").strip()))
        else:
            cells[name] = "supported"
            supported.append(name)
    return {
        "have": len(supported),
        "partial": len(partial),
        "advertised": sum(1 for v in adv.values() if v[0]),
        "total": len(reference),
        "pct": pct(len(supported), len(reference)),
        "pct_adv": pct(len(supported) + len(partial), len(reference)),
        "cells": cells,
        "partial_list": partial,
        "missing": missing,
        "required_missing": [n for n in required if n not in adv or not adv[n][0]],
        "instance": len(inst),
        "device": len(dev),
        "source": "vk/src/mali_instance.c:22 instance table, vk/src/mali_physical_device.c:226 device table, vk/tests/t_dxvk_caps.c:33 reference set",
    }


# ------------------------------------------------------------------ formats
PROBE = r"""
import ctypes, json, sys
so, arch = sys.argv[2], sys.argv[3]
lib = ctypes.CDLL(so)
gp = lib.vk_icdGetInstanceProcAddr
gp.restype = ctypes.c_void_p
gp.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

class AI(ctypes.Structure):
    _fields_ = [("sType", ctypes.c_int), ("pNext", ctypes.c_void_p), ("pApplicationName", ctypes.c_char_p),
                ("applicationVersion", ctypes.c_uint32), ("pEngineName", ctypes.c_char_p),
                ("engineVersion", ctypes.c_uint32), ("apiVersion", ctypes.c_uint32)]
class ICI(ctypes.Structure):
    _fields_ = [("sType", ctypes.c_int), ("pNext", ctypes.c_void_p), ("flags", ctypes.c_uint32),
                ("pApplicationInfo", ctypes.c_void_p), ("enabledLayerCount", ctypes.c_uint32),
                ("ppEnabledLayerNames", ctypes.c_void_p), ("enabledExtensionCount", ctypes.c_uint32),
                ("ppEnabledExtensionNames", ctypes.c_void_p)]
class FP(ctypes.Structure):
    _fields_ = [("linear", ctypes.c_uint32), ("optimal", ctypes.c_uint32), ("buffer", ctypes.c_uint32)]

app = AI(0, None, b"status", 0, b"status", 0, (1 << 22) | (3 << 12))
ci = ICI(1, None, 0, ctypes.cast(ctypes.pointer(app), ctypes.c_void_p), 0, None, 0, None)
inst = ctypes.c_void_p()
r = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(ICI), ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))(
    gp(None, b"vkCreateInstance"))(ctypes.byref(ci), None, ctypes.byref(inst))
if r != 0:
    print(json.dumps({"error": "vkCreateInstance %d" % r})); sys.exit(0)
n = ctypes.c_uint32(1)
pd = ctypes.c_void_p()
r = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_void_p))(
    gp(inst, b"vkEnumeratePhysicalDevices"))(inst, ctypes.byref(n), ctypes.byref(pd))
if r != 0 or n.value != 1:
    print(json.dumps({"error": "enumerate %d/%d" % (r, n.value)})); sys.exit(0)
gfp = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p)(gp(inst, b"vkGetPhysicalDeviceFormatProperties"))
out = {}
for f in range(1, 185):
    fp = FP()
    gfp(pd, f, ctypes.byref(fp))
    out[str(f)] = fp.optimal | (fp.buffer << 32)
print(json.dumps({"formats": out}))
"""


def probe_formats(arch):
    """Run the ICD on a fake GPU through the ICD entry point and read every format's feature bits."""
    if not ICD.is_file():
        return {"known": False, "reason": f"{ICD.relative_to(ROOT)} not built"}
    env = {"MALI_BACKEND": "fake", "MALI_FAKE_GPU": arch, "PATH": "/usr/bin:/bin"}
    try:
        p = subprocess.run([sys.executable, "-c", PROBE, str(ROOT / "tools/status/status.py"), str(ICD), arch],
                           capture_output=True, text=True, timeout=120, env=env)
    except (OSError, subprocess.TimeoutExpired) as e:
        return {"known": False, "reason": f"probe failed: {e}"}
    lines = [l for l in p.stdout.strip().splitlines() if l.startswith("{")]
    if not lines:
        return {"known": False, "reason": "probe produced no data"}
    data = json.loads(lines[-1])
    if "error" in data:
        return {"known": False, "reason": data["error"]}
    return {"known": True, "bits": {int(k): v for k, v in data["formats"].items()}}


def panel_formats():
    per = {a: probe_formats(a) for a in ("v13", "v11")}
    out = {"per_arch": {}, "source": "vk/src/mali_format.c:39 range, runtime vkGetPhysicalDeviceFormatProperties"}
    for arch, r in per.items():
        if not r["known"]:
            out["per_arch"][arch] = {"known": False, "reason": r["reason"]}
            continue
        bits = r["bits"]
        sup = [f for f, b in bits.items() if b]
        out["per_arch"][arch] = {
            "known": True,
            "have": len(sup),
            "total": FMT_CORE13_LAST,
            "pct": pct(len(sup), FMT_CORE13_LAST),
            "have_in_range": len([f for f in sup if f <= FMT_DRIVER_LAST]),
            "range_total": FMT_DRIVER_LAST,
            "pct_range": pct(len([f for f in sup if f <= FMT_DRIVER_LAST]), FMT_DRIVER_LAST),
            "sampled": len([f for f in sup if bits[f] & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT]),
            "linear_filter": len([f for f in sup if bits[f] & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT]),
            "color_attachment": len([f for f in sup if bits[f] & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT]),
            "color_blend": len([f for f in sup if bits[f] & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT]),
            "depth_stencil": len([f for f in sup if bits[f] & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT]),
            "supported": sup,
            "missing": [f for f in range(1, FMT_CORE13_LAST + 1) if f not in sup],
        }
    v13 = out["per_arch"]["v13"]
    out["have"] = v13.get("have") if v13.get("known") else None
    out["total"] = FMT_CORE13_LAST if out["have"] is not None else None
    out["pct"] = v13.get("pct") if v13.get("known") else None
    return out


# ------------------------------------------------------------------ features
def panel_features():
    src = read("vk/src/mali_physical_device.c")
    i = src.find("const struct vk_features features = {")
    if i < 0:
        die("mali_physical_device.c: cannot find the feature table")
    j = src.index("\n   };", i)
    block = src[i:j]
    entries = {}
    for m in re.finditer(r"\.(\w+)\s*=\s*(true|false)\s*,?", block):
        entries[m.group(1)] = m.group(2) == "true"
    ref = []
    for line in read("tools/status/features-vk13.txt").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        ref.append(line.split()[1])
    true_in = [f for f in ref if entries.get(f)]
    false_in = [f for f in ref if not entries.get(f)]
    extra = sorted(f for f, v in entries.items() if v and f not in ref)
    return {
        "have": len(true_in),
        "total": len(ref),
        "pct": pct(len(true_in), len(ref)),
        "true": true_in,
        "missing": false_in,
        "explicit_false": sorted(f for f, v in entries.items() if not v),
        "extension_true": extra,
        "entries": len(entries),
        "source": "vk/src/mali_physical_device.c:207 feature table, tools/status/features-vk13.txt (55 core 1.0 + 15 core 1.3 bits)",
    }


# ------------------------------------------------------------------ gate / tests
def panel_tests(log_path):
    chk = read("vk/check.sh")
    singles = len(re.findall(r'^\s*run "', chk, re.M))
    gpu_loops = len(re.findall(r"for g in v11 v13; do run ", chk))
    out = {
        "static_cases": singles + gpu_loops,
        "static_detail": f"{singles - gpu_loops} single-arch cases + {gpu_loops} run on both v11 and v13",
        "source": "vk/check.sh:run lines" + (f", {log_path}:case results" if log_path else ""),
    }
    p = ROOT / log_path if log_path else None
    if not p or not p.is_file():
        out.update({"known": False, "reason": f"{log_path or 'no log'} not found (run vk/check.sh > log)"})
        return out
    ok = fail = v11 = v13 = 0
    fails = []
    for line in p.read_text(errors="replace").splitlines():
        m = re.match(r"^(ok|FAIL)\s+(.+)$", line)
        if not m:
            continue
        status, name = m.group(1), m.group(2)
        if status == "ok":
            ok += 1
        else:
            fail += 1
            fails.append(name)
        if re.search(r"\bv11\b", name):
            v11 += 1
        if re.search(r"\bv13\b", name):
            v13 += 1
    out.update({"known": True, "have": ok, "total": ok + fail, "pct": pct(ok, ok + fail),
                "v11": v11, "v13": v13, "other": ok + fail - v11 - v13, "fails": fails})
    return out


# ------------------------------------------------------------------ history
def load_history():
    if not HIST.is_file():
        return []
    out = []
    for line in HIST.read_text().splitlines():
        if line.strip():
            out.append(json.loads(line))
    return out


def history_entry(panels, sha, date, dirty):
    def num(name, key="have"):
        v = panels.get(name, {})
        return v.get(key) if v.get("known", True) is not False else None
    return {
        "date": date,
        "commit": sha[:7],
        "dirty": dirty,
        "entrypoints": {"have": num("entry_points"), "total": panels["entry_points"].get("total")},
        "extensions": {"have": num("extensions"), "partial": panels["extensions"].get("partial"),
                       "total": panels["extensions"].get("total")},
        "formats": {"have": num("formats"), "total": panels["formats"].get("total")},
        "features": {"have": num("features"), "total": panels["features"].get("total")},
        "tests": {"pass": num("tests"), "total": panels["tests"].get("total")},
    }


def delta(first, now, key, sub="have"):
    a = (first.get(key) or {}).get(sub)
    b = (now.get(key) or {}).get(sub)
    if a is None or b is None:
        return "unknown"
    d = b - a
    return f"{d:+d}" if d else "0"


# ------------------------------------------------------------------ rendering
def bar(have, total, width=20):
    if not total:
        return ""
    n = round(width * have / total)
    return "█" * n + "·" * (width - n)


def render(panels, sha, date, dirty, first, cur):
    ep, ex, fm, ft, ts = (panels["entry_points"], panels["extensions"], panels["formats"],
                          panels["features"], panels["tests"])
    L = []
    A = L.append
    A("# mali-open status")
    A("")
    A(f"Generated by `tools/status/status.py` from the driver tree at commit `{sha[:7]}`"
      f"{' (dirty tree)' if dirty else ''} ({date}) — **do not edit**: rerun `make status`.")
    A("")
    A("Progress is measured against fixed denominators, so a number moving up means the driver improved, not")
    A("that the question changed. `docs/status-history.jsonl` keeps one line per commit; the Δ column compares")
    A(f"the first recorded entry (`{(first or {}).get('commit', '—')}`) with this one.")
    A("")
    A("## Summary")
    A("")
    A("| Panel | Have | Total | % | Δ since first entry |")
    A("|---|---:|---:|---:|---:|")
    rows = [
        ("Entry points answered (driver + Mesa runtime)", ep["answered"], ep["total"] if ep["answered"] is not None else None, "entrypoints"),
        ("Entry points written in this driver", ep["have"], ep["total"], "entrypoints"),
        ("Extensions usable (advertised, none partial)", ex["have"], ex["total"], "extensions"),
        ("Formats (core 1.3)", fm.get("have"), fm.get("total"), "formats"),
        ("Core 1.0/1.3 feature bits", ft["have"], ft["total"], "features"),
        ("Gate cases passing", ts.get("have"), ts.get("total"), "tests"),
    ]
    for label, have, total, key in rows:
        h = "unknown" if have is None else f"{have}"
        t = "unknown" if total is None else f"{total}"
        A(f"| {label} | {h} | {t} | {fmt_pct(have, total)} | {delta(first, cur, key, 'pass' if key == 'tests' else 'have')} |")
    A("")

    A("## History")
    A("")
    A("`docs/status-history.jsonl` holds one JSON object per commit that changed a number:")
    A("`{\"date\", \"commit\", \"dirty\", \"entrypoints\", \"extensions\", \"formats\", \"features\", \"tests\"}`, each panel")
    A("`{have, total}` (extensions also `partial`, tests also `pass`). A line is appended only when the panels")
    A("differ from the previous line, so the file is a record of real changes, not of runs. `--check` never")
    A("writes it. The Δ column above is `first entry → this commit`.")
    A("")
    A("## Entry points")
    A("")
    if ep["runtime"] is None:
        A(f"`{ep['have']}/{ep['total']}` core Vulkan 1.3 commands have a `mali_<Name>` implementation in")
        A(f"`vk/src/*.c` ({ep['definitions']} `mali_*` definitions in total, {ep['table_total']} commands in the")
        A(f"test table). The runtime column is **unknown** (no built ICD to scan).")
        A(f"`{bar(ep['have'], ep['total'])}` {fmt_pct(ep['have'], ep['total'])}")
    else:
        A(f"`{ep['answered']}/{ep['total']}` core Vulkan 1.3 commands are answered by the ICD = "
          f"{fmt_pct(ep['answered'], ep['total'])}: `{ep['have']}` implemented in `vk/src` plus")
        A(f"`{ep['runtime']}` served by Mesa's `vk_common_*` runtime (`{fmt_pct(ep['have'], ep['total'])}` of the")
        A(f"surface is this driver's own code). `{ep['definitions']}` `mali_*` definitions in total,")
        A(f"{ep['table_total']} commands in the test table.")
        A("")
        A(f"`{bar(ep['have'], ep['total'])}` driver-native {fmt_pct(ep['have'], ep['total'])}")
    A("")
    groups = {}
    for n in ep["missing"]:
        m = re.match(r"vk(Cmd)?([A-Z][a-z]+)", n)
        groups.setdefault((m.group(1) or "") + m.group(2) if m else "other", []).append(n)
    if ep["missing"]:
        A(f"**Still missing ({len(ep['missing'])}):** neither a `mali_<Name>` implementation nor a `vk_common_*`")
        A("object in the built ICD — the command would resolve to the entry-point stub. Grouped by API prefix:")
        A("")
        for g in sorted(groups, key=lambda k: (-len(groups[k]), k)):
            A(f"- `vk{g}` — {len(groups[g])}: {', '.join(x[2:] for x in groups[g])}")
        A("")
    else:
        A("**Still missing: none** — every core command in the table resolves to driver code or the Mesa runtime.")
        A("")

    A("## Extensions")
    A("")
    A(f"`{ex['have']} supported + {ex['partial']} partial / {ex['total']}` reference extensions = ")
    A(f"{fmt_pct(ex['have'], ex['total'])} fully supported, {fmt_pct(ex['have'] + ex['partial'], ex['total'])} advertised.")
    A(f"Instance table: {ex['instance']} entries, device table: {ex['device']} entries, {ex['advertised']} advertised in total.")
    A("")
    A("Legend: `#` supported, `+` advertised but partial (the table comment qualifies it), `.` missing.")
    A("")
    names = sorted(ex["cells"])
    CELL = {"supported": "#", "partial": "+", "missing": "."}
    for i in range(0, len(names), 6):
        chunk = names[i:i + 6]
        A("`" + "".join(CELL[ex["cells"][n]] for n in chunk) + "` " + " ".join(
            f"{n.replace('VK_', '')}{'' if ex['cells'][n] == 'supported' else ' (' + ex['cells'][n][0] + ')'}" for n in chunk))
    A("")
    if ex["partial_list"]:
        A("**Advertised but partial** (the driver's own comment says what is missing):")
        A("")
        A("| Extension | Why partial |")
        A("|---|---|")
        for name, _kw, note in ex["partial_list"]:
            A(f"| `{name}` | {note.replace('|', '/')} |")
        A("")
    if ex["missing"]:
        A(f"**Still missing ({len(ex['missing'])}):** " + ", ".join(f"`{n}`" for n in ex["missing"]))
        A("")
    if ex["required_missing"]:
        A("Required by DXVK 3.x and not advertised: " + ", ".join(f"`{n}`" for n in ex["required_missing"]))
        A("")

    A("## Formats")
    A("")
    if fm.get("have") is None:
        A(f"**unknown** — {fm['per_arch']['v13'].get('reason', 'no data')}")
    else:
        v13 = fm["per_arch"]["v13"]
        A(f"`{v13['have']}/{FMT_CORE13_LAST}` Vulkan 1.3 core formats accepted on the fake v13 GPU = "
          f"{fmt_pct(v13['have'], FMT_CORE13_LAST)} (v11: {fmt_pct(fm['per_arch']['v11'].get('have'), FMT_CORE13_LAST)}).")
        A(f"Inside the driver's own `1..{FMT_DRIVER_LAST}` scan range: `{v13['have_in_range']}/{FMT_DRIVER_LAST}` = "
          f"{fmt_pct(v13['have_in_range'], FMT_DRIVER_LAST)}.")
        A("")
        A("| Property (v13) | Formats | % of 184 |")
        A("|---|---:|---:|")
        for label, key in [("sampled image", "sampled"), ("sampled + linear filter", "linear_filter"),
                           ("color attachment", "color_attachment"), ("color attachment blend", "color_blend"),
                           ("depth/stencil attachment", "depth_stencil")]:
            A(f"| {label} | {v13[key]} | {fmt_pct(v13[key], FMT_CORE13_LAST)} |")
        A("")
        gap = v13["missing"]
        A(f"**Still missing ({len(gap)}):** `{ranges(gap)}`")
        A("")
        A("Format numbers are `VkFormat` enum values; 157..184 are the ASTC block formats, which")
        A("`vk/src/mali_format.c` documents as a gap (no ASTC plane descriptor yet).")
    A("")

    A("## Features")
    A("")
    A(f"`{ft['have']}/{ft['total']}` core feature bits set true in the ONE feature table = {fmt_pct(ft['have'], ft['total'])}")
    A(f"(+{len(ft['extension_true'])} extension/1.1/1.2 bits true on top of the core 1.0 + 1.3 core sets).")
    A(f"`{bar(ft['have'], ft['total'])}`")
    A("")
    A(f"**Still false ({len(ft['missing'])} of {ft['total']}):** " + ", ".join(f"`{f}`" for f in ft["missing"]))
    A("")
    if ft["extension_true"]:
        A("Extension feature bits true: " + ", ".join(f"`{f}`" for f in ft["extension_true"]))
        A("")

    A("## Shader lowering gaps")
    A("")
    A("Omitted: `tools/shaderdb` measures instruction counts, registers and compile time, not unsupported")
    A("SPIR-V ops, and no other in-tree data records them. Reporting a number here would be invented, so")
    A("this panel says nothing rather than 0% or 100%.")
    A("")

    A("## Gate / tests")
    A("")
    if ts.get("known"):
        A(f"`{ts['have']}/{ts['total']}` `vk/check.sh` cases pass = {fmt_pct(ts['have'], ts['total'])} "
          f"(v11: {ts['v11']}, v13: {ts['v13']}, arch-independent: {ts['other']}).")
        if ts["fails"]:
            A("")
            A("Failing: " + ", ".join(f"`{n}`" for n in ts["fails"]))
    else:
        A(f"Pass/fail **unknown** — {ts['reason']}. The suite defines {ts['static_cases']} cases "
          f"({ts['static_detail']}); rerun `make status`, which writes `vk/out/native/check.log`.")
    A("")

    A("## Footer: what each denominator counts as")
    A("")
    A(f"- **Entry points** {ep['total']}: the core (non-extension) commands in `vk/tests/dxvk_api_table.h`, the")
    A("  app-visible Vulkan 1.3 command set the tests use (generated from vk.xml by `vk/tests/gen_dxvk_api.py`).")
    A("  It is not the whole registry: platform WSI and extension entry points are excluded. Numerator: a")
    A("  `mali_<Name>` definition (nm on the built ICD, or a source scan without a build); \"answered\" adds")
    A("  the commands whose `vk_common_<Name>` object is linked in, i.e. Mesa's runtime serves them.")
    A(f"- **Extensions** {ex['total']}: the union of both extension tables in the tree and DXVK 3.x's required")
    A("  list (`dxvk_exts` in `vk/tests/t_dxvk_caps.c`). \"Partial\" means the table's own comment qualifies the")
    A("  support (\"only\", \"smoke\", …), not a separate test result.")
    A(f"- **Formats** {FMT_CORE13_LAST}: `VK_FORMAT_ASTC_12x12_SRGB_BLOCK`, the last Vulkan 1.3 core format. The")
    A(f"  driver scans `1..{FMT_DRIVER_LAST}` on purpose; the panel also shows that narrower range.")
    A(f"- **Features** {ft['total']}: the 55 `VkPhysicalDeviceFeatures` bits plus the 15")
    A("  `VkPhysicalDeviceVulkan13Features` bits, listed in `tools/status/features-vk13.txt` (generated from")
    A("  vk.xml). 1.1/1.2 and extension feature bits are reported separately, not in the denominator.")
    A("- **Tests**: case results from `vk/out/native/check.log`; a case counts once per named run, so the")
    A("  v11/v13 loop cases count twice.")
    A("- Anything the tree cannot prove is printed as *unknown*, never as 0% or 100%.")
    A("")
    return "\n".join(L)


# ------------------------------------------------------------------ main
def collect(log_path):
    return {
        "entry_points": panel_entrypoints(),
        "extensions": panel_extensions(),
        "formats": panel_formats(),
        "features": panel_features(),
        "tests": panel_tests(log_path),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="exit 1 if regenerating would change a committed file")
    ap.add_argument("--check-log", default="vk/out/native/check.log", help="vk/check.sh output to count (default: %(default)s)")
    ap.add_argument("--probe", nargs=3, help=argparse.SUPPRESS)
    args = ap.parse_args()

    # The commit that last changed the driver inputs, not HEAD: the page and its history are committed
    # together with the tool, so HEAD would move under the page and --check would never settle.
    sha = git("log", "-1", "--format=%h", "--", "vk") or "unknown"
    date = git("log", "-1", "--format=%cI", "--", "vk") or "unknown"
    dirty = bool(git("status", "--porcelain", "--untracked-files=no", "--", "vk"))

    panels = collect(args.check_log)

    hist = load_history()
    entry = history_entry(panels, sha, date, dirty)
    sig = {k: v for k, v in entry.items() if k not in ("date",)}
    if not any({k: v for k, v in h.items() if k != "date"} == sig for h in hist):
        hist.append(entry)
    first = hist[0] if hist else entry

    md = render(panels, sha, date, dirty, first, entry) + "\n"

    data = {
        "commit": sha,
        "date": date,
        "dirty": dirty,
        "panels": {k: {kk: vv for kk, vv in v.items() if kk not in ("cells", "supported", "have_names")}
                   for k, v in panels.items()},
        "history_entries": len(hist),
        "generator": "tools/status/status.py",
    }
    js = json.dumps(data, indent=1, sort_keys=True) + "\n"

    if args.check:
        bad = []
        if not MD.is_file() or MD.read_text() != md:
            bad.append(str(MD.relative_to(ROOT)))
        if not JSONF.is_file() or JSONF.read_text() != js:
            bad.append(str(JSONF.relative_to(ROOT)))
        if bad:
            print("stale: " + ", ".join(bad) + " (run: tools/status/status.py)")
            return 1
        print("docs/STATUS.md and docs/status.json are up to date")
        return 0

    MD.write_text(md)
    JSONF.write_text(js)
    if not HIST.is_file() or hist[-1] is not entry:
        HIST.write_text("".join(json.dumps(h, sort_keys=True) + "\n" for h in hist))
    print(f"wrote {MD.relative_to(ROOT)}, {JSONF.relative_to(ROOT)}, {HIST.relative_to(ROOT)}")


if __name__ == "__main__":
    sys.exit(main())
