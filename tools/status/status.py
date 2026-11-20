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
import tomllib
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
        text = c.read_text(errors="replace")
        for m in re.finditer(r"^mali_(?:v10_|v11_|v13_)?(\w+)\s*\(", text, re.M):
            line = text.count("\n", 0, m.start()) + 1
            defs.setdefault(m.group(1), f"vk/src/{c.name}:{line}")
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
        "where": defs,
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
    def table_line(rel, head):
        s = read(rel)
        return s.count("\n", 0, s.find(head)) + 1
    origin = {}
    for k in inst:
        origin[k] = f"vk/src/mali_instance.c:{table_line('vk/src/mali_instance.c', 'mali_instance_extensions = {')}"
    for k in dev:
        origin.setdefault(k, f"vk/src/mali_physical_device.c:{table_line('vk/src/mali_physical_device.c', 'const struct vk_device_extension_table extensions = {')}")

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
        "origin": origin,
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
    entries, lines = {}, {}
    base = src.count("\n", 0, i) + 1
    for m in re.finditer(r"\.(\w+)\s*=\s*(true|false)\s*,?", block):
        entries[m.group(1)] = m.group(2) == "true"
        lines.setdefault(m.group(1), base + block.count("\n", 0, m.start()))
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
        "lines": lines,
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




# ================================================================== merged item tracker
# The second half of this file merges tools/progress.py (docs/progress.toml: GPU ladder, pipeline pieces,
# roadmap milestones) into the same page, so there is one tracker and one set of numbers.
TRACK = "docs/progress.toml"
FORMATS_FILE = "tools/status/formats-vk13.txt"
README = "README.md"

STATUS_ORDER = ("done", "partial", "missing")
SYM = {"done": "done", "partial": "partial", "missing": "not done"}


def fmt_pct1(have, total):
    return "n/a" if not total else f"{100.0 * have / total:.1f}%"


def verify_ev(ev, status, what):
    """Same rule as tools/progress.py: 'done' needs a file (and token) that exists in this tree."""
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


def tracker_rows():
    """GPU ladder, pipeline pieces and roadmap milestones, each as (panel, name, status, evidence, left)."""
    cfg = tomllib.loads(read(TRACK))
    rows = []
    ladder = cfg["ladder"]
    rung_label = {r["id"]: r["label"] for r in ladder}
    rung_ev = {r["id"]: list(r["ev"]) for r in ladder}
    for g in cfg["gpu"]:
        note = g.get("note", "")
        for r in ladder:
            st = g[r["id"]]
            ev = rung_ev[r["id"]]
            left = ""
            if st == "done":
                left = ""
            elif g.get("branch"):
                left = f"only on {g['branch']}, unmerged: {note}"
            elif st == "partial":
                left = f"{r['label']} is not fully exercised on {g['name']}: {note}"
            else:
                left = f"{r['label']} not implemented/brought up on {g['name']}"
            ev = list(ev)
            if g["tsv"] and r["id"] in ("draw", "compute", "deqp"):
                ev.append(f"vk/deqp/ours-daily-sample-{g['tsv']}.tsv")
            if g.get("branch"):
                ev = [f"branch:{g['branch']}"]
                st = "partial" if st != "none" else "missing"
            elif st == "none":
                st = "missing"
            verify_ev(ev, "done" if st == "done" else "partial", f"gpu {g['id']}.{r['id']}")
            rows.append(("gpu", f"{g['name']} ({g['arch']}): {r['label']}", st, ", ".join(ev), left))

    piece_left = {
        "Dual-source blending": "only on branch feat/msaa; single-source blending is done",
        "MSAA 4x and resolve": "only on branch feat/msaa; no 2x/8x, no MSAA on v13",
        "BC1-BC7 decode (offline; hardware wiring pending)": "decoder tested offline; not wired into the image path",
        "WSI and presentation (Wayland)": "X11 and presentation edge cases (OUT_OF_DATE, fence-poll CSYNC) still open",
        "multiViewport (viewport 0 only without geometry shaders)": "viewport 1+ ignored without geometry shaders; apps using split-screen get one viewport",
        "Render target compression (AFBC)": "not implemented; bandwidth-heavy titles run slower",
        "Robust buffer access (robustness2 bounds clamping)": "not implemented; only nullDescriptor is advertised",
        "Geometry and tessellation shaders (emulated)": "not implemented; DXVK disables effects that need them",
        "Transform feedback (emulated)": "not implemented; apps using it fail pipeline creation",
        "Graphics pipeline library": "not implemented; DXVK has to monolithically recompile pipelines (stutter)",
    }
    for p in cfg["piece"]:
        st = p["status"] if p["status"] != "none" else "missing"
        ev = verify_ev(p["ev"], st, f"piece '{p['name']}'")
        left = "" if st == "done" else piece_left.get(p["name"], "work not finished; see evidence")
        rows.append(("pipeline", p["name"], st, ", ".join(ev), left))

    stages = []
    for line in read("ROADMAP.md").splitlines():
        m = re.match(r"\|\s*(R\d)\s*\|([^|]*)\|\s*(.*?)\s*\|\s*$", line)
        if not m:
            continue
        state = m.group(3).replace("**", "")
        if state.startswith("Out of this tree"):
            continue
        st = "done" if state.startswith("Done") else "missing" if state.startswith("Not started") else "partial"
        stages.append((f"{m.group(1)}: {m.group(2).strip()}", st))
    for name, st in stages:
        ev = ["ROADMAP.md"]
        left = "" if st == "done" else f"stage goal not reached: {name}"
        rows.append(("milestone", name, st, ", ".join(ev), left))
    for m in cfg["milestone"]:
        st = m["status"] if m["status"] != "none" else "missing"
        ev = verify_ev(m["ev"], st, f"milestone '{m['name']}'")
        left = "" if st == "done" else "end-to-end goal not demonstrated in this tree"
        rows.append(("milestone", m["name"], st, ", ".join(ev), left))
    return rows


def override_status(key, default):
    cfg = tomllib.loads(read(TRACK))
    ov = cfg.get("override", {})
    if key in ov:
        return (ov[key]["status"] if ov[key]["status"] != "none" else "missing"), ", ".join(ov[key]["ev"])
    return default[0], default[1]


def feature_left(name):
    why = {
        "robustBufferAccess": "out-of-range buffer accesses are not clamped; robustness is via robustness2 nullDescriptor only",
        "geometryShader": "no geometry path; DXVK drops effects that need it and tiles with GS fail",
        "tessellationShader": "no tessellation; DXVK re-tessellates or disables the effect",
        "textureCompressionETC2": "ETC2 textures are not decoded; DXVK must transcode them",
        "textureCompressionASTC_LDR": "ASTC textures are not decoded; vkd3d-proton/DXVK transcode instead",
        "textureCompressionASTC_HDR": "HDR ASTC textures are not decoded",
        "shaderFloat64": "fp64 shaders fail compilation",
        "sparseBinding": "no sparse residency; apps using it must allocate full images",
        "pipelineStatisticsQuery": "query results for pipeline stats read as zero",
        "largePoints": "point size above 1 is ignored",
        "depthBounds": "depth-bounds test ignored; apps relying on it may show extra fragments",
        "inheritedQueries": "secondary command buffers cannot inherit queries",
    }
    return why.get(name, f"bit stays false: enabling VkPhysicalDeviceFeatures.{name} is not advertised")


def ext_left(name, why_partial):
    consequence = {
        "VK_EXT_graphics_pipeline_library": "DXVK 3.x recompiles whole pipelines instead of linking stages: longer hitches",
        "VK_KHR_pipeline_library": "same: DXVK's pipeline-library fast path is off",
        "VK_EXT_extended_dynamic_state3": "only polygonMode is dynamic; every other EDS3 state re-binds the pipeline (DXVK CPU cost)",
        "VK_EXT_image_drm_format_modifier": "only LINEAR is accepted; tiled/compressed dma-bufs are rejected",
        "VK_EXT_robustness2": "only nullDescriptor; out-of-bounds accesses are not clamped",
        "VK_KHR_line_rasterization": "rectangular lines only; wide/smooth lines use the hardware default",
        "VK_EXT_line_rasterization": "rectangular lines only; wide/smooth lines use the hardware default",
    }
    if why_partial:
        return f"advertised but partial: {why_partial}"
    return consequence.get(name, "not advertised: an app that enables it gets VK_ERROR_EXTENSION_NOT_PRESENT")


def detail_rows(panels):
    """Every panel's items as (panel, name, status, evidence, what-is-left)."""
    ep, ex, fm, ft, ts = (panels["entry_points"], panels["extensions"], panels["formats"],
                          panels["features"], panels["tests"])
    rows = []
    where = ep.get("where", {})
    for n in ep.get("have_names", []):
        st, ev = "done", where.get(n[2:], "vk/src/*.c (source scan)")
        key = "core." + n[2:]
        o, oev = override_status(key, (st, ev))
        if o != st:
            st, ev = o, (oev or ev)
        rows.append(("entrypoints", n, st, ev, "" if st == "done" else "declared but not implemented here"))
    for n in ep.get("runtime_names", []):
        rows.append(("entrypoints", n, "partial", "vk_common_* in the built ICD",
                     "served by Mesa's runtime, not by this driver: no mali_ implementation yet"))
    for n in ep.get("missing", []):
        rows.append(("entrypoints", n, "missing", "not in vk/src, no vk_common_* object",
                     "resolves to the entry-point stub"))
    names = {}
    if (ROOT / FORMATS_FILE).is_file():
        for line in read(FORMATS_FILE).splitlines():
            if line.startswith("#") or not line.strip():
                continue
            num, nm = line.split()
            names[int(num)] = nm
    v13 = fm["per_arch"].get("v13", {})
    if v13.get("known"):
        sup = set(v13["supported"])
        for f in range(1, FMT_CORE13_LAST + 1):
            nm = f"VK_FORMAT_{names.get(f, 'ENUM_' + str(f))}"
            if f in sup:
                rows.append(("formats", f"#{f} {nm}", "done", "runtime vkGetPhysicalDeviceFormatProperties (fake v13)",
                             ""))
            else:
                if f > FMT_DRIVER_LAST:
                    left = "outside the driver's 1..156 scan: ASTC has no plane descriptor (vk/src/mali_format.c)"
                else:
                    left = "format table has no entry: probe returns zero feature bits"
                rows.append(("formats", f"#{f} {nm}", "missing", "vk/src/mali_format.c table + runtime probe", left))
    for n in ft.get("true", []):
        st, ev = "done", f"vk/src/mali_physical_device.c:{ft.get('lines', {}).get(n, 207)}"
        o, oev = override_status("core." + n, (st, ev))
        if o != st:
            st, ev = o, (oev or ev)
        left = "" if st == "done" else "declared partial: only part of the feature is implemented"
        rows.append(("features", n, st, ev, left))
    for n in ft.get("missing", []):
        st, ev = "missing", f"vk/src/mali_physical_device.c:{ft.get('lines', {}).get(n, 207)} (= false)"
        o, oev = override_status("core." + n, (st, ev))
        if o != st:
            st, ev = o, (oev or ev)
        rows.append(("features", n, st, ev, "" if st == "done" else feature_left(n)))
    partial_why = {n: note for n, _kw, note in ex.get("partial_list", [])}
    for n in sorted(ex["cells"]):
        st = {"supported": "done", "partial": "partial", "missing": "missing"}[ex["cells"][n]]
        ev = ex.get("origin", {}).get(n, "vk/src/mali_physical_device.c extension table")
        o, oev = override_status("ext." + n, (st, ev))
        if o != st:
            st, ev = o, (oev or ev)
        rows.append(("extensions", n, st, ev, "" if st == "done" else ext_left(n, partial_why.get(n))))
    return rows


def tally(rows):
    d = sum(1 for r in rows if r[2] == "done")
    p = sum(1 for r in rows if r[2] == "partial")
    m = sum(1 for r in rows if r[2] == "missing")
    return d, p, m


# ------------------------------------------------------------------ history (real, from git + jsonl)
def git_blob(sha, rel):
    try:
        r = subprocess.run(["git", "-C", str(ROOT), "show", f"{sha}:{rel}"], capture_output=True, text=True)
        return r.stdout if r.returncode == 0 else None
    except OSError:
        return None


def count_features(src, ref):
    i = src.find("const struct vk_features features = {")
    if i < 0:
        return None
    j = src.find("\n   };", i)
    block = src[i:j]
    on = set(re.findall(r"\.(\w+)\s*=\s*true", block))
    return len([f for f in ref if f in on])


def count_exts(inst, dev, reference, required):
    blocks = (inst or "") + (dev or "")
    have = partial = 0
    for name in reference:
        short = name[3:]
        m = re.search(rf"^.*\.{re.escape(short)}\s*=\s*true.*$", blocks, re.M)
        if not m or not blocks:
            continue
        if re.search(r"\b((?:[A-Za-z0-9_*]*[A-Z][A-Za-z0-9_*]*)\s+only|partial|smoke|experimental)\b", m.group(0)):
            partial += 1
        else:
            have += 1
    return have, partial


def derived_history(panels, n=30):
    """Recompute the source-derivable panels at past commits. Formats/tests need a built ICD and a log,
    so they only appear in docs/status-history.jsonl, never here. Nothing is interpolated."""
    ref = [l.split()[1] for l in read(FEATURES).splitlines() if l.strip() and not l.startswith("#")]
    table = read("vk/tests/dxvk_api_table.h")
    core = [nn for nn, owner, _ in re.findall(r'\{"(vk\w+)", "([^"]+)", \d, "([^"]*)"\}', table)
            if owner.startswith(CORE_OWNERS)]
    dxvk = re.search(r"static const char \*const dxvk_exts\[\] = \{(.*?)\};",
                     read("vk/tests/t_dxvk_caps.c"), re.S)
    required = re.findall(r'"(VK_\w+)"', dxvk.group(1)) if dxvk else []
    reference = sorted(set(panels["extensions"]["cells"]) | set(required))
    src_files = sorted(p.name for p in (ROOT / "vk" / "src").glob("*.c"))
    log = git("log", "--format=%h|%cs", "-n", str(n), "--", "vk").splitlines()
    out = []
    for line in log:
        if "|" not in line:
            continue
        sha, date = line.split("|", 1)
        defs = set()
        for f in src_files:
            b = git_blob(sha, f"vk/src/{f}")
            if b:
                defs.update(m.group(1) for m in re.finditer(r"^mali_(?:v10_|v11_|v13_)?(\w+)\s*\(", b, re.M))
        ep = len([c for c in core if c[2:] in defs])
        inst = git_blob(sha, "vk/src/mali_instance.c") or ""
        dev = git_blob(sha, "vk/src/mali_physical_device.c") or ""
        ex_have, ex_part = count_exts(inst, dev, reference, required)
        ft = count_features(dev, ref)
        if ft is None:
            continue
        out.append({"commit": sha, "date": date, "entrypoints": ep, "total_ep": len(core),
                    "extensions": ex_have, "ext_partial": ex_part, "total_ext": len(reference),
                    "features": ft, "total_ft": len(ref)})
    out.reverse()
    return out


def spark(values, width=20):
    vals = [v for v in values if v is not None][-width:]
    if len(vals) < 2:
        return "(not enough history yet)"
    lo, hi = min(vals), max(vals)
    if hi == lo:
        return "▄" * len(vals)
    ticks = "▁▂▃▄▅▆▇█"
    return "".join(ticks[round((v - lo) / (hi - lo) * 7)] for v in vals)


def last_changed(paths):
    s = git("log", "-1", "--format=%h %cs", "--", *paths)
    return s or "unknown"


# ------------------------------------------------------------------ rendering
def bar(have, total, width=20):
    if not total:
        return ""
    n = round(width * have / total)
    return "█" * n + "·" * (width - n)


def md_table(rows, cols):
    out = ["| " + " | ".join(c[0] for c in cols) + " |", "|" + "|".join(c[1] for c in cols) + "|"]
    for r in rows:
        out.append("| " + " | ".join(str(x).replace("|", "/") for x in r) + " |")
    return out


def panel_summary(name, rows):
    d, p, m = tally(rows)
    tot = d + p + m
    return {"panel": name, "done": d, "partial": p, "missing": m, "total": tot,
            "strict": fmt_pct1(d, tot), "half": fmt_pct1(d + 0.5 * p, tot)}


def render(panels, sha, date, dirty, hist, series, rows):
    ep, ex, fm, ft, ts = (panels["entry_points"], panels["extensions"], panels["formats"],
                          panels["features"], panels["tests"])
    L = []
    A = L.append
    d, p, m = tally(rows)
    tot = d + p + m
    A("# mali-open driver status")
    A("")
    A(f"Generated by `tools/status/status.py` from the tree at commit `{sha[:7]}`"
      f"{' (dirty tree)' if dirty else ''} ({date}) — **do not generate it by hand**: run `make status`.")
    A("")
    A(f"**{d} of {tot} tracked items are done and backed by evidence ({fmt_pct1(d, tot)} strict).**")
    A(f"Counting partial items as 0.5 gives {fmt_pct1(d + 0.5 * p, tot)}. The strict number is the one to")
    A("quote: an item is `partial` when only part of it works, and the partial line says which part.")
    A("")
    A("Every row below is one item with its evidence and, when it is not done, one line on what is left.")
    A("`docs/progress.toml` is still the hand-maintained tracker list (GPU ladder, pipeline pieces,")
    A("milestones); everything else is measured from the tree, the built ICD and the committed tests.")
    A("")
    A("## Summary")
    A("")
    summ = [panel_summary("Entry points (driver-native)", [r for r in rows if r[0] == "entrypoints"]),
            panel_summary("Extensions", [r for r in rows if r[0] == "extensions"]),
            panel_summary("Formats (Vulkan 1.3 core)", [r for r in rows if r[0] == "formats"]),
            panel_summary("Core 1.0/1.3 feature bits", [r for r in rows if r[0] == "features"]),
            panel_summary("GPU bring-up ladder", [r for r in rows if r[0] == "gpu"]),
            panel_summary("Pipeline pieces", [r for r in rows if r[0] == "pipeline"]),
            panel_summary("Roadmap milestones", [r for r in rows if r[0] == "milestone"])]
    table = [("**All tracked items**", f"**{d}**", f"**{p}**", f"**{m}**", f"**{tot}**",
              f"**{fmt_pct1(d, tot)}**", f"**{fmt_pct1(d + 0.5 * p, tot)}**")]
    table += [(s["panel"], s["done"], s["partial"], s["missing"], s["total"], s["strict"], s["half"]) for s in summ]
    for line in md_table(table, [("Panel", "---"), ("Done", "---:"), ("Partial", "---:"), ("Missing", "---:"),
                                 ("Total", "---:"), ("Strict %", "---:"), ("Partial=0.5 %", "---:")]):
        A(line)
    A("")
    gate = "unknown"
    if ts.get("known"):
        gate = f"{ts['have']}/{ts['total']} pass ({fmt_pct1(ts['have'], ts['total'])})"
    A(f"Gate health (not a work item, so not in the table above): `vk/check.sh` {gate}. dEQP is a")
    A("measurement of the reference GPU, not a checklist item; its failures are below.")
    A("")
    A("History: `docs/status-history.jsonl` records every commit that changed a panel, and the")
    A(f"source-derived series below was recomputed from git at the last {len(series)} commits that touched")
    A("`vk/`, with the same parsers and the same denominators. No entry is invented or interpolated.")
    A("")
    A("## What is not done, and why it matters")
    A("")
    by = {}
    for panel, name, st, ev, left in rows:
        if st != "done":
            by.setdefault(panel, []).append((name, st, left))
    group_titles = {
        "features": "Features (a device-creation bit the app must be allowed to enable)",
        "extensions": "Extensions (an app enabling it gets VK_ERROR_EXTENSION_NOT_PRESENT today)",
        "formats": "Formats (textures in this format cannot be sampled/rendered)",
        "entrypoints": "Entry points (the command resolves to a stub or only to Mesa's runtime)",
        "pipeline": "Pipeline pieces (the draw/compute path is incomplete)",
        "gpu": "Per-GPU bring-up (the ladder rung is not proven on that GPU)",
        "milestone": "Roadmap milestones and end-to-end goals",
    }
    order = ["features", "extensions", "formats", "entrypoints", "pipeline", "gpu", "milestone"]
    for key in order:
        items = by.get(key, [])
        if not items:
            continue
        cnt = {}
        for _, st, _ in items:
            cnt[st] = cnt.get(st, 0) + 1
        A(f"### {group_titles[key]} — {len(items)} open")
        A("")
        A(f"{cnt.get('missing', 0)} not done, {cnt.get('partial', 0)} partial. Largest gaps first:")
        A("")
        picked = items if key in ("pipeline", "milestone") else items[:12]
        for name, st, left in picked:
            A(f"- `{name}` ({SYM[st]}) — {left or 'no reason recorded'}")
        if len(items) > len(picked):
            A(f"- … {len(items) - len(picked)} more in the per-item table below.")
        A("")
    A("### Shader lowering gaps")
    A("")
    A("No honest number exists: `tools/shaderdb` records instruction counts, registers and compile time, not")
    A("unsupported SPIR-V ops, and nothing else in the tree records them. This panel says so instead of")
    A("printing 0% or 100%.")
    A("")
    A("## History")
    A("")
    entries = [h for h in hist if isinstance(h, dict)]
    first = entries[0] if entries else {}
    A(f"`docs/status-history.jsonl` now has {len(entries)} entr{'y' if len(entries) == 1 else 'ies'}. The first is "
      f"`{first.get('commit', '—')}` ({first.get('date', '—')}).")
    A("")
    if len(series) >= 2:
        base, now = series[0], series[-1]
        A(f"Source-derived, from `git show` at each of the last {len(series)} commits that touched `vk/`")
        A("(same parsers and denominators; this series scans the sources, while the panels above use `nm`")
        A("when the ICD is built, so a symbol alias can differ by one). Formats and gate need a build, so")
        A("they are absent here and only appear in the recorded table:")
        A("")
        tbl = []
        for key, label in (("entrypoints", f"Entry points (of {now['total_ep']})"),
                           ("extensions", f"Extensions (of {now['total_ext']})"),
                           ("features", f"Feature bits (of {now['total_ft']})")):
            vals = [s[key] for s in series]
            tbl.append((label, base[key], now[key], f"{now[key] - base[key]:+d}", spark(vals)))
        for line in md_table(tbl, [("Panel", "---"), (f"At {base['commit']}", "---:"),
                                   (f"At {now['commit']}", "---:"), ("Δ", "---:"), ("Trend (old → new)", "---")]):
            A(line)
        A("")
    else:
        A("Not enough commits in the window to draw a trend; the table below is the recorded history.")
        A("")
    lc = [("Entry points", "vk/src/*.c", "vk/src"),
          ("Extensions", "vk/src/mali_instance.c, vk/src/mali_physical_device.c", "vk/src/mali_instance.c vk/src/mali_physical_device.c"),
          ("Features", "vk/src/mali_physical_device.c", "vk/src/mali_physical_device.c"),
          ("Formats", "vk/src/mali_format.c", "vk/src/mali_format.c"),
          ("dEQP", "vk/deqp/ours-daily-sample-*.tsv", "vk/deqp"),
          ("Tracker list", TRACK, TRACK)]
    A("Last changed (from `git log -1` on each panel's inputs):")
    A("")
    for label, human, paths in lc:
        A(f"- **{label}** — {last_changed(paths.split())} — `{human}`")
    A("")
    if entries:
        A("Recorded panels (one line per changed commit; `--check` never writes it):")
        A("")
        tbl = []
        for h in entries[-8:]:
            it = h.get("items", {})
            tbl.append((h.get("commit", "?"), h.get("date", "")[:10],
                        (f"{it['done']}/{it['total']}" if it else "—"),
                        (it.get("half_pct", "—") if it else "—"),
                        f"ep {h.get('entrypoints', {}).get('have', '?')}/{(h.get('entrypoints') or {}).get('total', '?')}",
                        f"ex {h.get('extensions', {}).get('have', '?')}",
                        f"fmt {h.get('formats', {}).get('have', '?')}",
                        f"ft {h.get('features', {}).get('have', '?')}",
                        f"gate {h.get('tests', {}).get('pass', '?')}/{(h.get('tests') or {}).get('total', '?')}"))
        for line in md_table(tbl, [("Commit", "---"), ("Date", "---"), ("Items done", "---:"),
                                   ("With partial", "---:"), ("Entry points", "---"), ("Ext", "---"),
                                   ("Fmt", "---"), ("Feat", "---"), ("Gate", "---")]):
            A(line)
        A("")
        A(f"No item is marked done here without a file (and token) that exists in this tree; the generator")
        A("exits non-zero when an evidence path is missing, so the numbers cannot silently inflate.")
        A("")

    titles = {"gpu": "GPU bring-up ladder", "pipeline": "Pipeline pieces", "milestone": "Roadmap milestones",
              "entrypoints": "Entry points", "extensions": "Extensions", "formats": "Formats (Vulkan 1.3 core)",
              "features": "Core 1.0/1.3 feature bits"}
    sources = {
        "gpu": f"`{TRACK}` `[[ladder]]`/`[[gpu]]`; evidence verified by this script",
        "pipeline": f"`{TRACK}` `[[piece]]`; evidence verified by this script",
        "milestone": f"`ROADMAP.md` stage table plus `{TRACK}` `[[milestone]]`",
        "entrypoints": ep.get("source", ""),
        "extensions": ex.get("source", ""),
        "formats": fm.get("source", ""),
        "features": ft.get("source", ""),
    }
    for key in ("gpu", "pipeline", "milestone", "entrypoints", "extensions", "formats", "features"):
        pr = [r for r in rows if r[0] == key]
        if not pr:
            continue
        s = panel_summary(titles[key], pr)
        A(f"## {titles[key]} — {s['done']}/{s['total']} done, {s['partial']} partial, {s['missing']} not done"
          f" ({s['strict']} strict, {s['half']} with partial=0.5)")
        A("")
        A(f"`{bar(s['done'] + 0.5 * s['partial'], s['total'])}` source: {sources[key]}")
        A("")
        tbl = []
        for panel, name, st, ev, left in pr:
            tbl.append((f"`{name}`", SYM[st], f"`{ev}`" if ev else "—", left or "—"))
        for line in md_table(tbl, [("Item", "---"), ("Status", "---"), ("Evidence", "---"), ("What is left", "---")]):
            A(line)
        A("")

    A("## Gate / tests")
    A("")
    if ts.get("known"):
        A(f"`{ts['have']}/{ts['total']}` `vk/check.sh` cases pass = {fmt_pct1(ts['have'], ts['total'])}"
          f" (v11: {ts['v11']}, v13: {ts['v13']}, arch-independent: {ts['other']}).")
        A("")
        A("These are the offline checks, not phone results; they run on a fake GPU.")
        A("")
    else:
        A(f"Pass/fail **unknown** — {ts['reason']}. The suite defines {ts['static_cases']} cases"
          f" ({ts['static_detail']}); rerun `make status`, which writes `vk/out/native/check.log`.")
        A("")

    A("## dEQP-VK daily sample")
    A("")
    for g in panels.get("deqp", {}).get("runs", []):
        A(f"### {g['gpu']} ({g['arch']}) — `vk/deqp/ours-daily-sample-{g['tsv']}.tsv`")
        A("")
        A(f"{g['Pass']} pass, {g['Fail']} fail, {g['NotSupported']} not supported, {g['Other']} other.")
        A("`NotSupported` cases are expected by design (the driver does not claim the feature); the")
        A("`Fail` list below is the unexpected part.")
        A("")
        if g["fail_groups"]:
            tbl = [(f"`{k}`", n, "`" + g["examples"][k][0] + "`") for k, n in sorted(g["fail_groups"].items(), key=lambda kv: -kv[1])[:20]]
            for line in md_table(tbl, [("Failing area", "---:"), ("Cases", "---:"), ("Example case", "---")]):
                A(line)
            A("")
        if g["notsup_groups"]:
            top = sorted(g["notsup_groups"].items(), key=lambda kv: -kv[1])[:10]
            A("Expected `NotSupported`, top areas: " + ", ".join(f"`{k}` ({n})" for k, n in top) + ".")
            A("")
    A("## Where each number comes from")
    A("")
    A(f"- Entry points: {ep.get('source', '')}. Numerator: `nm` on the built ICD (`mali_*`), plus")
    A("  `vk_common_*` objects for the Mesa runtime; without a build the scan is source-only and the")
    A("  runtime column reads *unknown*.")
    A(f"- Extensions: {ex.get('source', '')}. `partial` comes from the table's own comment (`only`, `smoke`).")
    A("- Formats: `vk/src/mali_format.c` is the table; the per-format answer is the driver's own")
    A("  `vkGetPhysicalDeviceFormatProperties` on the fake GPU, so a format listed but probed as zero")
    A("  features is reported not done.")
    A(f"- Features: {ft.get('source', '')}.")
    A(f"- GPU ladder / pipeline / milestones: `{TRACK}` and `ROADMAP.md`, evidence verified here.")
    A("- dEQP: committed TSV files under `vk/deqp/`; nothing is re-run by this script.")
    A("")
    A("## Denominator rules")
    A("")
    A(f"- Entry points {ep['total']}: core (non-extension) commands in `vk/tests/dxvk_api_table.h`.")
    A(f"- Extensions {ex['total']}: union of both extension tables and DXVK 3.x's required list.")
    A(f"- Formats {FMT_CORE13_LAST}: the last Vulkan 1.3 core `VkFormat`. The driver's own scan is 1..{FMT_DRIVER_LAST}, shown too.")
    A(f"- Features {ft['total']}: 55 core 1.0 + 15 core 1.3 feature bits (`{FEATURES.relative_to(ROOT)}`), not 1.1/1.2/extension bits.")
    A("- Gate cases are not items: they are health checks, so 122/122 does not raise the progress numbers.")
    A("- Partial counts as 0.5 only in the second percentage; the first counts done only.")
    A("")
    return "\n".join(L)


def load_history():
    if not HIST.is_file():
        return []
    out = []
    for line in HIST.read_text().splitlines():
        if line.strip():
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return out


def readme_block(panels, rows):
    d, p, m = tally(rows)
    tot = d + p + m
    L = ["<!-- status:begin (generated by tools/status/status.py; run: make status) -->", ""]
    L.append(f"**{d}/{tot} tracked items done ({fmt_pct1(d, tot)} strict); "
             f"{fmt_pct1(d + 0.5 * p, tot)} if partial items count as 0.5.**")
    L.append("")
    tbl = [("All tracked items", d, p, m, tot, fmt_pct1(d, tot), fmt_pct1(d + 0.5 * p, tot))]
    for name, key in (("Entry points (driver-native)", "entrypoints"), ("Extensions", "extensions"),
                      ("Formats (Vulkan 1.3 core)", "formats"), ("Core feature bits", "features"),
                      ("GPU ladder", "gpu"), ("Pipeline pieces", "pipeline"), ("Milestones", "milestone")):
        pr = [r for r in rows if r[0] == key]
        s = panel_summary(name, pr)
        tbl.append((name, s["done"], s["partial"], s["missing"], s["total"], s["strict"], s["half"]))
    for line in md_table(tbl, [("Panel", "---"), ("Done", "---:"), ("Partial", "---:"), ("Missing", "---:"),
                               ("Total", "---:"), ("Strict %", "---:"), ("Partial=0.5 %", "---:")]):
        L.append(line)
    L.append("")
    L.append("Full per-item tables, evidence, what-is-left and history: [`docs/STATUS.md`](docs/STATUS.md).")
    L.append("`make status` regenerates both; `make status-check` fails when they are stale.")
    L.append("")
    L.append("<!-- status:end -->")
    return "\n".join(L)


def splice_readme(block):
    src = read(README)
    a = src.find("<!-- status:begin")
    b = src.find("<!-- status:end -->")
    if a < 0 or b < 0:
        die("README.md has no status:begin/status:end markers")
    return src[:a] + block + src[b + len("<!-- status:end -->"):]


def panel_deqp(cfg):
    runs = []
    for g in cfg["gpu"]:
        if not g["tsv"]:
            continue
        c = {"Pass": 0, "Fail": 0, "NotSupported": 0, "Other": 0}
        fails, notsup = [], []
        for line in read(f"vk/deqp/ours-daily-sample-{g['tsv']}.tsv").splitlines():
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            k = parts[1]
            c[k if k in c else "Other"] += 1
            if k == "Fail":
                fails.append(parts[0])
            elif k == "NotSupported":
                notsup.append(parts[0])

        def group(names):
            out = {}
            for n in names:
                short = n.replace("dEQP-VK.", "")
                key = ".".join(short.split(".")[:3])
                out[key] = out.get(key, 0) + 1
            return out
        runs.append({"gpu": g["name"], "arch": g["arch"], "tsv": g["tsv"], **c,
                     "fail_groups": group(fails), "notsup_groups": group(notsup),
                     "examples": {k: [n for n in fails if k in n] for k in group(fails)}})
    return {"runs": runs, "source": "committed vk/deqp/ours-daily-sample-*.tsv"}


def history_entry(panels, rows, sha, date, dirty):
    d, p, m = tally(rows)
    tot = d + p + m
    return {
        "date": date,
        "commit": sha[:7],
        "dirty": dirty,
        "items": {"done": d, "partial": p, "missing": m, "total": tot,
                  "strict_pct": round(100.0 * d / tot, 1) if tot else 0.0,
                  "half_pct": round(100.0 * (d + 0.5 * p) / tot, 1) if tot else 0.0},
        "entrypoints": {"have": len(panels["entry_points"]["have_names"]),
                        "runtime": panels["entry_points"].get("runtime"),
                        "total": panels["entry_points"]["total"]},
        "extensions": {"have": panels["extensions"]["have"], "partial": panels["extensions"]["partial"],
                       "total": panels["extensions"]["total"]},
        "formats": {"have": panels["formats"].get("have"), "total": panels["formats"].get("total")},
        "features": {"have": panels["features"]["have"], "total": panels["features"]["total"]},
        "tests": {"pass": panels["tests"].get("have"), "total": panels["tests"].get("total")},
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="exit 1 if regenerating would change a committed file")
    ap.add_argument("--verify-evidence", action="store_true",
                    help="offline: only check that every tracker item's evidence exists (used by vk/check.sh)")
    ap.add_argument("--check-log", default="vk/out/native/check.log")
    args = ap.parse_args()

    if args.verify_evidence:
        rows = tracker_rows()
        print(f"{len(rows)} tracker items, evidence verified")
        return 0

    sha = git("log", "-1", "--format=%h", "--", "vk") or "unknown"
    date = git("log", "-1", "--format=%cI", "--", "vk") or "unknown"
    dirty = bool(git("status", "--porcelain", "--untracked-files=no", "--", "vk"))

    panels = {
        "entry_points": panel_entrypoints(),
        "extensions": panel_extensions(),
        "formats": panel_formats(),
        "features": panel_features(),
        "tests": panel_tests(args.check_log),
    }
    panels["deqp"] = panel_deqp(tomllib.loads(read(TRACK)))
    rows = detail_rows(panels) + tracker_rows()
    hist = load_history()
    entry = history_entry(panels, rows, sha, date, dirty)
    series = derived_history(panels)
    sig = {k: v for k, v in entry.items() if k != "date"}
    changed = not any({k: v for k, v in h.items() if k != "date"} == sig for h in hist)
    if changed:
        hist = hist + [entry]

    md = render(panels, sha, date, dirty, hist, series, rows) + "\n"
    data = {"commit": sha, "date": date, "dirty": dirty,
            "summary": panel_summary("all", rows), "items": [list(r) for r in rows],
            "history_entries": len(hist), "generator": "tools/status/status.py"}
    js = json.dumps(data, indent=1, sort_keys=True) + "\n"
    rd = splice_readme(readme_block(panels, rows))

    if args.check:
        bad = []
        if not MD.is_file() or MD.read_text() != md:
            bad.append(str(MD.relative_to(ROOT)))
        if not JSONF.is_file() or JSONF.read_text() != js:
            bad.append(str(JSONF.relative_to(ROOT)))
        if read(README) != rd:
            bad.append("README.md (status block)")
        if bad:
            print("stale: " + ", ".join(bad) + " (run: tools/status/status.py, needs the built ICD)")
            return 1
        print("docs/STATUS.md, docs/status.json and the README summary are up to date")
        return 0

    MD.write_text(md)
    JSONF.write_text(js)
    ROOT.joinpath(README).write_text(rd)
    if changed:
        HIST.write_text("".join(json.dumps(h, sort_keys=True) + "\n" for h in hist))
    d, p, m = tally(rows)
    print(f"wrote docs/STATUS.md ({len(rows)} items, {d} done / {p} partial / {m} missing = "
          f"{fmt_pct1(d, d + p + m)} strict, {fmt_pct1(d + 0.5 * p, d + p + m)} with partial=0.5), "
          "docs/status.json, README status block, docs/status-history.jsonl")


if __name__ == "__main__":
    sys.exit(main())
