#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Vulkan feature matrix for mali-open (stdlib only).

  extract  primary sources (vk.xml, Vulkan-Profiles, Mesa tree, ANGLE, gpuinfo profiles) -> data/refs.json
  ours     `vulkaninfo --json` capture of our driver                                    -> data/ours-<gpu>.json
  gen      data/*.json -> vk/FEATURE-MATRIX-VULKAN.md + data/gaps.json
  check    a fresh `vulkaninfo --json` capture against data/ours-<gpu>.json: fails (exit 1) when the driver
           stops advertising an extension, a feature bit or a format feature bit that the matrix says it has.

Only `extract` needs the downloaded sources; everything else runs from the committed data/ directory.
See tools/feature-matrix/README.md for the exact commands.
"""
import argparse
import json
import os
import re
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
GPUS = ("v10", "v11", "v13")
GPU_NAME = {"v10": "Mali-G610 (Poco X4 GT)", "v11": "Mali-G615 (CMF)", "v13": "Mali-G925 (X8)"}
CORE_FEATURE_STRUCTS = ["VkPhysicalDeviceFeatures", "VkPhysicalDeviceVulkan11Features",
                        "VkPhysicalDeviceVulkan12Features", "VkPhysicalDeviceVulkan13Features",
                        "VkPhysicalDeviceVulkan14Features"]
# Profiles in matrix column order: (column label, profile name)
PROFILE_COLS = [("1.0", "VP_LUNARG_minimum_requirements_1_0"), ("1.1", "VP_LUNARG_minimum_requirements_1_1"),
                ("1.2", "VP_LUNARG_minimum_requirements_1_2"), ("1.3", "VP_LUNARG_minimum_requirements_1_3"),
                ("1.4", "VP_LUNARG_minimum_requirements_1_4"),
                ("R22", "VP_KHR_roadmap_2022"), ("R24", "VP_KHR_roadmap_2024"),
                ("A21", "VP_ANDROID_baseline_2021"), ("A22", "VP_ANDROID_baseline_2022"),
                ("A15", "VP_ANDROID_15_minimums"), ("A16", "VP_ANDROID_16_minimums"),
                ("LDB24", "VP_LUNARG_desktop_baseline_2024")]
FMT_BITS = [("smp", "SAMPLED_IMAGE"), ("flt", "SAMPLED_IMAGE_FILTER_LINEAR"), ("col", "COLOR_ATTACHMENT"),
            ("bld", "COLOR_ATTACHMENT_BLEND"), ("sto", "STORAGE_IMAGE"), ("ds", "DEPTH_STENCIL_ATTACHMENT"),
            ("vtx", "VERTEX_BUFFER"), ("utb", "UNIFORM_TEXEL_BUFFER"), ("stb", "STORAGE_TEXEL_BUFFER")]
BUFFER_BITS = {"VERTEX_BUFFER", "UNIFORM_TEXEL_BUFFER", "STORAGE_TEXEL_BUFFER", "STORAGE_TEXEL_BUFFER_ATOMIC"}


def load(p):
    with open(p) as f:
        return json.load(f)


def dump(p, obj, compact=False):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w") as f:
        json.dump(obj, f, indent=None if compact else 1, sort_keys=True, separators=(",", ":") if compact else None)
        f.write("\n")


def fmt_bit(b):
    """VK_FORMAT_FEATURE_2_X_BIT / VK_FORMAT_FEATURE_X_BIT -> X"""
    b = re.sub(r"^VK_FORMAT_FEATURE_(2_)?", "", b)
    return re.sub(r"_BIT(_KHR|_EXT)?$", "", b)


# ----------------------------------------------------------------------------------------------- extract
def lines_of(path):
    with open(path, errors="replace") as f:
        return f.read().split("\n")


def extract_registry(xml_path):
    raw = lines_of(xml_path)
    ext_line = {}
    for i, l in enumerate(raw, 1):
        m = re.search(r'<extension name="(VK_\w+)"', l)
        if m:
            ext_line[m.group(1)] = i
    root = ET.parse(xml_path).getroot()
    structs = {}
    for t in root.iter("type"):
        if t.get("category") != "struct":
            continue
        name = t.get("name")
        if t.get("alias"):
            structs[name] = {"alias": t.get("alias")}
            continue
        mem = []
        for m in t.findall("member"):
            if m.get("api") and "vulkan" not in m.get("api").split(","):
                continue
            n = m.find("name").text
            ty = m.find("type").text
            if n in ("sType", "pNext"):
                continue
            mem.append({"name": n, "type": ty, "limittype": m.get("limittype")})
        structs[name] = {"members": mem, "extends": (t.get("structextends") or "").split(",")}
    exts = {}
    for e in root.find("extensions"):
        name = e.get("name")
        sup = (e.get("supported") or "").split(",")
        if "vulkan" not in sup or e.get("type") != "device":
            continue
        feats = []
        for req in e.findall("require"):
            if req.get("api") and "vulkan" not in req.get("api").split(","):
                continue
            for ty in req.findall("type"):
                s = structs.get(ty.get("name"), {})
                if "VkPhysicalDeviceFeatures2" in s.get("extends", []):
                    feats.append(ty.get("name"))
        exts[name] = {"number": int(e.get("number")), "promotedto": e.get("promotedto"),
                      "deprecatedby": e.get("deprecatedby"), "obsoletedby": e.get("obsoletedby"),
                      "provisional": e.get("provisional") == "true", "platform": e.get("platform"),
                      "depends": e.get("depends"), "feature_structs": sorted(set(feats)),
                      "cite": "vk.xml:%d" % ext_line.get(name, 0)}
    core = {}
    for s in CORE_FEATURE_STRUCTS:
        core[s] = [m["name"] for m in structs[s]["members"]]
    feat_members = {}
    for e, d in exts.items():
        mm = []
        for s in d["feature_structs"]:
            st = structs.get(s, {})
            while "alias" in st:
                st = structs.get(st["alias"], {})
            mm += [m["name"] for m in st.get("members", []) if m["type"] == "VkBool32"]
        feat_members[e] = mm
    limittype = {}
    for sname, st in structs.items():
        for m in st.get("members", []):
            if m.get("limittype"):
                limittype[m["name"]] = m["limittype"]
    upper = {e.upper() + "_EXTENSION_NAME": e for e in exts}
    return {"extensions": exts, "core_features": core, "ext_feature_members": feat_members,
            "limittype": limittype, "ext_name_macro": upper}


def flatten_props(d, out, prefix=""):
    for k, v in d.items():
        if isinstance(v, dict):
            flatten_props(v, out)
        else:
            out[k] = v


def extract_profiles(prof_files):
    caps_by_file = {}
    profiles = {}
    for path, rel in prof_files:
        d = load(path)
        for pname, p in d["profiles"].items():
            blocks = []
            for c in p["capabilities"]:
                if isinstance(c, list):
                    for alt in c:
                        blocks.append((alt, "one-of(%s)" % ",".join(c)))
                else:
                    blocks.append((c, None))
            req = {"extensions": {}, "features": {}, "properties": {}, "formats": {}}
            for bname, alt in blocks:
                cap = d["capabilities"][bname]
                cite = "%s#capabilities.%s" % (rel, bname)
                tag = {"cite": cite, "alt": alt}
                for e in cap.get("extensions", {}):
                    req["extensions"].setdefault(e, tag)
                for st, mem in cap.get("features", {}).items():
                    for m, v in mem.items():
                        if v is True:
                            req["features"].setdefault(m, dict(tag, struct=st))
                for st, mem in cap.get("properties", {}).items():
                    flat = {}
                    flatten_props(mem, flat)
                    for m, v in flat.items():
                        req["properties"].setdefault(m, dict(tag, struct=st, value=v))
                for fmt, fp in cap.get("formats", {}).items():
                    bits = set()
                    for st, v in fp.items():
                        for k in ("optimalTilingFeatures", "bufferFeatures"):
                            bits |= {fmt_bit(b) for b in v.get(k, [])}
                    cur = req["formats"].setdefault(fmt, dict(tag, bits=[]))
                    cur["bits"] = sorted(set(cur["bits"]) | bits)
            profiles[pname] = {"file": rel, "api": p.get("api-version"), "parents": p.get("profiles") or [],
                               "req": req}
    return profiles


def eval_c(expr, arch):
    e = expr.strip().rstrip(",")
    e = re.sub(r"\bPAN_ARCH\b", str(arch), e)
    e = e.replace("&&", " and ").replace("||", " or ")
    e = re.sub(r"!(?!=)", " not ", e)
    e = re.sub(r"\btrue\b", "True", e)
    e = re.sub(r"\bfalse\b", "False", e)
    e = re.sub(r"\bhas_gralloc\b", "False", e)
    if re.fullmatch(r"[\s0-9()<>=!TrueFalsandot]+", e):
        try:
            return bool(eval(e, {"__builtins__": {}}, {}))
        except Exception:
            pass
    return "runtime: " + " ".join(expr.split()).rstrip(",")


def parse_c_table(lines, start_pat, prefix_fn, arches):
    """Designated initialiser table: `.NAME = expr,` (multi-line expr allowed), #ifdef-aware."""
    out = {}
    i = next(n for n, l in enumerate(lines) if re.search(start_pat, l))
    ifs = []
    cur = None
    for n in range(i + 1, len(lines)):
        l = lines[n]
        s = l.strip()
        if s.startswith("};"):
            break
        if s.startswith("#if"):
            ifs.append(s)
            continue
        if s.startswith("#endif"):
            ifs and ifs.pop()
            continue
        if s.startswith("#else"):
            if ifs:
                ifs[-1] = "!(" + ifs[-1] + ")"
            continue
        m = re.match(r"\.(\w+)\s*=\s*(.*)$", s)
        if m:
            cur = [prefix_fn(m.group(1)), m.group(2), n + 1, list(ifs)]
        elif cur is not None and s and not s.startswith("/"):
            cur[1] += " " + s
        if cur is not None and cur[1].rstrip().endswith(","):
            name, expr, ln, cond = cur
            expr = re.sub(r"/\*.*?\*/", "", expr)
            vals = {}
            for a in arches:
                v = eval_c(expr, a)
                if any("ANDROID" in c and not c.startswith("!") for c in cond):
                    v = "android-only"
                vals["v%d" % a] = v
            out.setdefault(name, {"vals": vals, "line": ln, "ifdef": cond})
            cur = None
    return out


def extract_panvk(mesa):
    rel = "src/panfrost/vulkan/panvk_vX_physical_device.c"
    L = lines_of(os.path.join(mesa, rel))
    ext = parse_c_table(L, r"\*ext = \(struct vk_device_extension_table\)", lambda n: "VK_" + n, (10, 11, 13))
    feat = parse_c_table(L, r"\*features = \(struct vk_features\)", lambda n: n, (10, 11, 13))
    return {"file": "third_party/mesa/" + rel, "extensions": ext, "features": feat}


def extract_zink(mesa):
    rel = "docs/drivers/zink.rst"
    L = lines_of(os.path.join(mesa, rel))
    vers = {}
    cur = None
    fmtbit = None
    for n, l in enumerate(L, 1):
        if n + 1 <= len(L) and re.match(r"^OpenGL (\d\.\d)$", l) and L[n].startswith("^^^"):
            cur = l.split()[1]
            vers[cur] = {"line": n, "features": {}, "extensions": {}, "limits": {}, "formats": {}}
            continue
        if cur is None:
            continue
        if re.match(r"^\S.*\n?$", l) and not l.startswith("*") and not l.startswith("OpenGL") and L[n:n + 1] and L[n].startswith("---"):
            cur = None
            continue
        for f in re.findall(r":vk-feat:`(\w+)`", l):
            vers[cur]["features"][f] = n
        for e in re.findall(r":ext:`(VK_\w+)`", l):
            vers[cur]["extensions"][e] = n
        m = re.search(r"Formats requiring ``VK_FORMAT_FEATURE_(\w+)_BIT``", l)
        if m:
            fmtbit = m.group(1)
            continue
        m = re.match(r"^\s*\* ``(\w+)`` ≥ (\d+)", l)
        if m:
            vers[cur]["limits"][m.group(1)] = [int(m.group(2)), n]
            continue
        m = re.match(r"^\s*\* ``(VK_FORMAT_\w+)``", l)
        if m and fmtbit:
            vers[cur]["formats"].setdefault(m.group(1), {"bits": [], "line": n})["bits"].append(fmtbit)
            continue
        m = re.match(r"^\s*\* ``([a-z]\w+)``\s*$", l)
        if m:
            vers[cur]["features"][m.group(1)] = n
    used = {}
    rel2 = "src/gallium/drivers/zink/zink_device_info.py"
    for n, l in enumerate(lines_of(os.path.join(mesa, rel2)), 1):
        for e in re.findall(r'Extension\("(VK_\w+)"', l):
            used.setdefault(e, n)
    return {"file": "third_party/mesa/" + rel, "used_file": "third_party/mesa/" + rel2, "gl": vers, "used": used}


def extract_angle(angle_dir, macro):
    out = {"extensions": {}, "features": {}}
    for fn in ("vk_renderer.cpp", "vk_caps_utils.cpp"):
        for n, l in enumerate(lines_of(os.path.join(angle_dir, fn)), 1):
            for m in re.findall(r"\b(VK_[A-Z0-9_]+_EXTENSION_NAME)\b", l):
                if m in macro:
                    out["extensions"].setdefault(macro[m], "%s:%d" % (fn, n))
            for m in re.findall(r"\bm\w*Features\w*\.(\w+)\b", l):
                if m[0].islower() and m not in ("sType", "pNext"):
                    out["features"].setdefault(m, "%s:%d" % (fn, n))
            for m in re.findall(r"\bfeatures\.(\w+)\b", l):
                if fn == "vk_caps_utils.cpp" and m[0].islower():
                    out["features"].setdefault(m, "%s:%d" % (fn, n))
    return out


def extract_steam(path, repo_rel):
    out = {}
    for n, l in enumerate(lines_of(path), 1):
        m = re.match(r'\s*\{"(VK_\w+)", "([^"]*)", ([01])\}', l)
        if m and "gamescope" in m.group(2):
            out[m.group(1)] = {"why": m.group(2), "hard": m.group(3) == "1", "cite": "%s:%d" % (repo_rel, n)}
    return out


def summarise_vulkaninfo(d):
    dev = d["capabilities"]["device"]
    feats = {}
    for st, mem in dev.get("features", {}).items():
        for m, v in mem.items():
            if isinstance(v, bool):
                feats[m] = feats.get(m, False) or v
    props = {}
    for st, mem in dev.get("properties", {}).items():
        flatten_props(mem, props)
    fmts = {}
    for f, fp in dev.get("formats", {}).items():
        opt, buf = set(), set()
        for st, v in fp.items():
            opt |= {fmt_bit(b) for b in v.get("optimalTilingFeatures", [])}
            buf |= {fmt_bit(b) for b in v.get("bufferFeatures", [])}
        fmts[f] = sorted(opt | buf)
    keep = ("apiVersion", "driverVersion", "deviceName", "driverName", "driverInfo", "vendorID", "deviceID")
    limits = {k: v for k, v in props.items() if k not in ("pipelineCacheUUID", "deviceUUID", "driverUUID", "deviceLUID",
                                                           "shaderModuleIdentifierAlgorithmUUID")}
    return {"extensions": sorted(dev.get("extensions", {})), "features": dict(sorted(feats.items())),
            "properties": limits, "formats": fmts, "id": {k: props.get(k) for k in keep},
            "profile": list(d.get("profiles", {}))[:1]}


def cmd_extract(a):
    reg = extract_registry(a.registry)
    pfiles = []
    pr = a.profiles
    for rel in ("Khronos/VP_KHR_roadmap.json", "VP_ANDROID_baseline_2021.json", "VP_ANDROID_baseline_2022.json",
                "VP_ANDROID_15_minimums.json", "VP_ANDROID_16_minimums.json", "LunarG/VP_LUNARG_minimum_requirements.json"):
        pfiles.append((os.path.join(pr, rel), "Vulkan-Profiles/profiles/" + rel))
    pfiles.append((a.desktop_baseline, "generated/VP_LUNARG_desktop_baseline_2024.json (vkprofiles combine of "
                   "Vulkan-Profiles/profiles/LunarG/VP_LUNARG_desktop_baseline_2024/)"))
    profiles = extract_profiles(pfiles)
    blob = {}
    for spec in a.gpuinfo:
        gpu, rid, path = spec.split(":", 2)
        s = summarise_vulkaninfo(load(path))
        s["report"] = "https://vulkan.gpuinfo.org/displayreport.php?id=" + rid
        s["report_id"] = int(rid)
        blob[gpu] = s
    refs = {"sources": {"vk.xml": a.registry_rev, "Vulkan-Profiles": a.profiles_rev, "mesa": a.mesa_rev,
                        "angle": a.angle_rev},
            "registry": reg, "profiles": profiles, "panvk": extract_panvk(a.mesa), "zink": extract_zink(a.mesa),
            "angle": extract_angle(a.angle, reg["ext_name_macro"]),
            "gamescope": extract_steam(a.steam_caps, "vk/tests/t_steam_caps.c"), "blob": blob}
    del refs["registry"]["ext_name_macro"]
    dump(os.path.join(DATA, "refs.json"), refs, compact=True)
    print("wrote", os.path.join(DATA, "refs.json"))


def cmd_ours(a):
    s = summarise_vulkaninfo(load(a.vulkaninfo))
    s["captured_with"] = a.env
    dump(os.path.join(DATA, "ours-%s.json" % a.gpu), s)
    print("wrote", os.path.join(DATA, "ours-%s.json" % a.gpu))


# --------------------------------------------------------------------------------------------------- gen
def profile_chain(profiles, name):
    out = [name]
    for p in profiles.get(name, {}).get("parents", []):
        out += profile_chain(profiles, p)
    return out


def req_by(profiles, kind, item):
    """{column label: tag} for every profile (incl. inherited parents) that requires item."""
    res = {}
    for label, pname in PROFILE_COLS:
        for p in profile_chain(profiles, pname):
            t = profiles.get(p, {}).get("req", {}).get(kind, {}).get(item)
            if t:
                res[label] = t
                break
    return res


GROUP_RULES = [
    ("video", ["video"]),
    ("ray tracing", ["ray_", "acceleration_structure", "opacity_micromap", "displacement_micromap"]),
    ("debug/tooling", ["debug", "tooling", "validation", "performance_query", "pipeline_executable", "device_fault",
                       "address_binding_report", "frame_boundary", "calibrated_timestamps", "pipeline_creation_feedback",
                       "pipeline_binary", "shader_module_identifier", "pipeline_properties"]),
    ("dynamic state", ["dynamic_state", "vertex_input_dynamic", "color_write_enable", "depth_clamp_control",
                       "attachment_feedback_loop_dynamic"]),
    ("sync/presentation", ["swapchain", "present", "surface", "display", "semaphore", "fence", "synchronization",
                           "queue_family_foreign", "global_priority", "hdr_metadata", "full_screen", "acquire",
                           "incremental", "physical_device_drm", "external_memory_acquire"]),
    ("descriptors", ["descriptor", "push_constant", "inline_uniform", "mutable_descriptor", "nested_command"]),
    ("memory/buffers", ["memory", "buffer", "bind_memory", "dedicated", "robust", "pageable", "address", "map_",
                        "pipeline_robustness", "maintenance", "device_group", "zero_initialize_device"]),
    ("shaders/SPIR-V", ["shader", "spirv", "float", "int8", "subgroup", "storage_class", "variable_pointers", "block_layout",
                        "workgroup", "cooperative", "demote", "atomic", "vulkan_memory_model", "relaxed_block",
                        "uniform_buffer_standard", "compute_shader_derivatives", "fragment_shader_barycentric",
                        "expect_assume", "maximal_reconvergence", "quad_control", "untyped", "bfloat",
                        "8bit_storage", "16bit_storage", "draw_parameters", "viewport_index_layer", "post_depth_coverage",
                        "fragment_shader_interlock", "replicated_composites", "relaxed_extended", "spirv_1_4"]),
    ("images/formats", ["format", "image", "sampler", "ycbcr", "astc", "texture", "4444", "border", "filter",
                        "rgba10x6", "compression", "swizzle", "_view", "depth_range", "depth_clip", "depth_clamp",
                        "multisampled_render_to", "attachment_feedback", "legacy_dithering", "load_store_op",
                        "separate_stencil", "fragment_density", "depth_bias", "depth_stencil_resolve"]),
]


def ext_group(name):
    n = name.lower()
    for g, keys in GROUP_RULES:
        if any(k in n for k in keys):
            return g
    return "rendering/pipelines"


def api_tuple(v):
    if isinstance(v, int):
        return (v >> 22, (v >> 12) & 0x3ff)
    m = re.match(r"(\d+)\.(\d+)", str(v))
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


NODE_DEPENDENT = set()
EXT_FEATURES = {}
# promoted extensions whose core form is optional but has no feature struct of its own in vk.xml
CORE_OPTIONAL = {"VK_KHR_draw_indirect_count": ["drawIndirectCount"],
                 "VK_KHR_sampler_mirror_clamp_to_edge": ["samplerMirrorClampToEdge"],
                 "VK_EXT_shader_viewport_index_layer": ["shaderOutputViewportIndex", "shaderOutputLayer"],
                 "VK_EXT_sampler_filter_minmax": ["samplerFilterMinmax"]}


def ours_ext_state(o, ext, info):
    if ext in o["extensions"]:
        return "yes"
    if ext in NODE_DEPENDENT:
        return "node"
    pt = info.get("promotedto") or ""
    m = re.match(r"VK_VERSION_(\d)_(\d)", pt)
    if m and api_tuple(o["id"]["apiVersion"]) >= (int(m.group(1)), int(m.group(2))):
        # a promoted extension is only "there through core" when its (optional) core feature bits are on
        bits = CORE_OPTIONAL.get(ext) or EXT_FEATURES.get(ext) or []
        if bits and not all(o["features"].get(b) for b in bits[:1] + [b for b in bits[1:] if ext in CORE_OPTIONAL]):
            return "no"
        return "core"
    return "no"


def hw_possible(refs, cur, kind, item, gpu, ours_has):
    h = cur["hardware"].get(item)
    if h is None and kind == "ext" and item.startswith("VK_KHR_video"):
        h = cur["hardware"]["VK_KHR_video_queue"]
    if h and h.get(gpu) not in (None, "unknown"):
        return h[gpu], h.get("note", "")
    if ours_has:
        return "yes", ""
    pv = refs["panvk"]["extensions" if kind == "ext" else "features"].get(item, {}).get("vals", {}).get(gpu)
    if pv is True:
        return "yes", "PanVK %s" % gpu
    b = refs["blob"].get(gpu)
    if b and ((kind == "ext" and item in b["extensions"]) or (kind == "feat" and b["features"].get(item))):
        return "yes", "Arm blob"
    return (h or {}).get(gpu, "unknown"), (h or {}).get("note", "")


def mark(v):
    if v is True or v == "yes":
        return "Y"
    if v in ("core", "node"):
        return v
    if v is False or v == "no" or v is None:
        return "-"
    if isinstance(v, str) and v.startswith("runtime"):
        return "rt"
    return str(v)


def check_limit(ltype, req, have):
    """True when our value meets the requirement under the registry limittype (first token of e.g. "min,pot")."""
    if have is None:
        return None
    lt = (ltype or "max").split(",")[0]
    if isinstance(req, list) and not req:
        return True
    if isinstance(req, list) and isinstance(req[0], str):
        return set(req) <= set(have if isinstance(have, list) else [have])
    if isinstance(req, list):
        if not isinstance(have, list) or len(have) != len(req):
            return None
        if lt == "range":
            return have[0] <= req[0] and have[1] >= req[1]
        return all(check_limit(ltype, r, h) for r, h in zip(req, have))
    if isinstance(req, bool):
        return (have is True) if req else True
    if lt == "min":
        return have <= req
    if lt in ("exact", "noauto", "struct"):
        return have == req
    return have >= req


PROP_ALIASES = {"supportedOperations": "subgroupSupportedOperations", "supportedStages": "subgroupSupportedStages",
                "quadOperationsInAllStages": "subgroupQuadOperationsInAllStages"}


def prop(o, m):
    v = o["properties"].get(m)
    if v is None and m in PROP_ALIASES:
        v = o["properties"].get(PROP_ALIASES[m])
    return v


def cmd_gen(a):
    refs = load(os.path.join(DATA, "refs.json"))
    cur = load(os.path.join(DATA, "curated.json"))
    NODE_DEPENDENT.update(cur.get("node_dependent", {}))
    EXT_FEATURES.update(refs["registry"]["ext_feature_members"])
    ours = {g: load(os.path.join(DATA, "ours-%s.json" % g)) for g in GPUS}
    reg = refs["registry"]
    prof = refs["profiles"]
    pv = refs["panvk"]
    zk = refs["zink"]
    an = refs["angle"]
    gs = refs["gamescope"]
    blob = refs["blob"]
    zink_ext = {}
    zink_feat = {}
    for ver, d in sorted(zk["gl"].items()):
        for e in d["extensions"]:
            zink_ext.setdefault(e, "GL" + ver)
        for f in d["features"]:
            zink_feat.setdefault(f, "GL" + ver)
    gaps = {"extensions": [], "features": [], "limits": [], "formats": [], "ext_feature_bits": []}
    counts = {}
    md = []
    w = md.append
    w("# Vulkan feature matrix (generated)")
    w("")
    w("Generated by `tools/feature-matrix/feature_matrix.py gen` from `tools/feature-matrix/data/` -- **do not edit by hand**; "
      "re-run the script (commands in `tools/feature-matrix/README.md`). The work breakdown is `vk/FEATURE-BACKLOG-VULKAN.md`; "
      "DirectX translation layers (DXVK, vkd3d-proton) are covered by `vk/FEATURE-MATRIX-DIRECTX.md`.")
    w("")
    w("## Sources")
    w("")
    w("| source | revision / id | used for |")
    w("|---|---|---|")
    s = refs["sources"]
    w("| KhronosGroup/Vulkan-Docs `xml/vk.xml` | %s | extension list, promotion, feature structs, limit types |" % s["vk.xml"])
    w("| KhronosGroup/Vulkan-Profiles | %s | core minimums 1.0-1.4 (VP_LUNARG_minimum_requirements), roadmap 2022/2024, Android baseline 2021/2022, Android 15/16 minimums, LunarG desktop baseline 2024 (generated with `vkprofiles combine`) |" % s["Vulkan-Profiles"])
    w("| Mesa (third_party/mesa, pin of this repo) | %s | PanVK (`%s`), zink (`%s`, `%s`) |" % (s["mesa"], pv["file"], zk["file"], zk["used_file"]))
    w("| ANGLE chromium/6478 (Chromium 126) | %s | extensions and feature bits ANGLE's Vulkan back end reads (`vk_renderer.cpp`, `vk_caps_utils.cpp`) |" % s["angle"])
    for g in GPUS:
        b = blob.get(g)
        if b:
            # The report comes from another vendor's phone with the same GPU (its device is in the profile name), not from ours.
            w("| Arm proprietary driver, %s (gpuinfo upload from another phone with this GPU, not one of ours) | gpuinfo report %d, %s, `%s` | Arm blob column for %s |" % (
                b["id"].get("deviceName") or GPU_NAME[g].split(" (")[0], b["report_id"], b["profile"][0] if b["profile"] else "", b["id"].get("driverInfo", ""), g))
    for g in GPUS:
        o = ours[g]
        w("| ours %s | `%s`, api %s, captured `%s` | ours columns |" % (g, o["id"]["deviceName"], ".".join(map(str, api_tuple(o["id"]["apiVersion"]))), o.get("captured_with", "")))
    w("| gamescope requirements | `vk/tests/t_steam_caps.c` (list taken from gamescope's vkCreateDevice) | gamescope column |")
    w("")
    w("Legend: `Y` advertised / required, `core` available through the core version we report, `node` advertised only when a DRM node of the GPU exists (phones: yes; fake backend: no), `-` no, `rt` decided at run "
      "time by PanVK (expression in data/refs.json), `(alt)` one of several alternatives in the profile. Profile columns: "
      "1.0-1.4 = core minimum requirements of that Vulkan version, R22/R24 = VP_KHR_roadmap_2022/2024 (R24 includes R22), "
      "A21/A22 = VP_ANDROID_baseline_2021/2022, A15/A16 = VP_ANDROID_15/16_minimums (A16 includes A15 includes A22), "
      "LDB24 = VP_LUNARG_desktop_baseline_2024. zink: the GL version whose requirement list (zink.rst) names it, `use` = "
      "zink uses it when present (zink_device_info.py). hw = can Valhall do it on that GPU (`yes` when we, PanVK or the Arm "
      "blob already expose it there; `emulate`/`no`/`unknown` from data/curated.json).")
    w("")
    w("## Environment switches that change what we advertise")
    w("")
    w("| switch | default | what changes | source | evidence |")
    w("|---|---|---|---|---|")
    for e in cur["env_gates"]:
        w("| `%s` | %s | %s | %s | %s |" % (e["switch"], e["default"], e["affects"], e["source"], e["evidence"]))
    w("")
    w("The ours columns below are the defaults (no switch set) and come from the **fake backend only**. Phone cross-check: "
      "the vulkaninfo text of x4 (v10, 2026-11-28 03:12) and cmf (v11, 03:15), build aae6f8bd8f, DroidDeck layers loaded "
      "(committed by the DirectX lane under `tools/feature-matrix-dx/data/phone/`; there is no v13/x8 capture), lists the "
      "same named device extensions as the fake-backend capture except `VK_EXT_physical_device_drm` (node-dependent, see above) "
      "and `VK_GOOGLE_display_timing` (listed only on the phones; not in our source, so a layer adds it -- HYPOTHESIS); "
      "`timestampPeriod` differs (fake backend 1, phones 76.92). The comparison is partial: 56 of the 128 device-extension "
      "entries in each phone capture print with an empty name, so they cannot be matched.")
    w("")

    # ---------------------------------------------------------------- extensions
    exts = {e: d for e, d in reg["extensions"].items()
            if re.match(r"VK_(KHR|EXT|ARM)_", e) and not d.get("provisional")}
    by_group = {}
    for e, d in exts.items():
        by_group.setdefault(ext_group(e), []).append(e)
    w("## Device extensions (KHR, EXT, ARM; provisional excluded)")
    w("")
    ext_counts = {"have": 0, "missing_possible": 0, "impossible": 0, "missing_unknown": 0, "deprecated_missing": 0}
    order = [g for g, _ in GROUP_RULES if g not in ("video",)] + ["rendering/pipelines", "video"]
    order = ["rendering/pipelines", "dynamic state", "descriptors", "memory/buffers", "images/formats",
             "sync/presentation", "shaders/SPIR-V", "ray tracing", "video", "debug/tooling"]
    for g in order:
        rows = sorted(by_group.get(g, []))
        w("### %s (%d)" % (g, len(rows)))
        w("")
        w("| extension | core in | profiles | zink | ANGLE | gamescope | PanVK v10/v11/v13 | Arm blob v10/v11/v13 | ours v10/v11/v13 | hw v10/v11/v13 | notes |")
        w("|---|---|---|---|---|---|---|---|---|---|---|")
        for e in rows:
            d = exts[e]
            core = (d.get("promotedto") or "").replace("VK_VERSION_", "").replace("_", ".")
            if core and not core[0].isdigit():
                core = "-> " + core
            pr = req_by(prof, "extensions", e)
            prs = " ".join(k + ("(alt)" if t.get("alt") else "") for k, t in pr.items())
            z = zink_ext.get(e) or ("use" if e in zk["used"] else "")
            ang = "Y" if e in an["extensions"] else ""
            gm = ("hard" if gs[e]["hard"] else "opt") if e in gs else ""
            pvv = pv["extensions"].get(e, {}).get("vals", {})
            pvs = "/".join(mark(pvv.get(x)) for x in GPUS) if pvv else "-/-/-"
            bl = "/".join(("Y" if e in blob[x]["extensions"] else "-") if x in blob else "?" for x in GPUS)
            os_ = [ours_ext_state(ours[x], e, d) for x in GPUS]
            hws = []
            notes = []
            for x, st in zip(GPUS, os_):
                hv, nt = hw_possible(refs, cur, "ext", e, x, st != "no")
                hws.append(hv)
                if nt and nt not in notes and not nt.startswith(("PanVK", "Arm")):
                    notes.append(nt)
            if d.get("deprecatedby") is not None or d.get("obsoletedby") is not None:
                notes.insert(0, "deprecated by %s" % (d.get("deprecatedby") or d.get("obsoletedby") or "core"))
            if d.get("platform"):
                notes.insert(0, "platform " + d["platform"])
            w("| `%s` | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                e, core, prs, z, ang, gm, pvs, bl, "/".join(mark(x) for x in os_), "/".join(hws), "; ".join(notes)))
            # gaps + counts (per extension: worst arch decides)
            deprecated = d.get("deprecatedby") is not None or d.get("obsoletedby") is not None
            if all(x != "no" for x in os_):
                ext_counts["have"] += 1
            else:
                missing = [x for x, st in zip(GPUS, os_) if st == "no"]
                hwm = {x: hw for x, hw in zip(GPUS, hws) if x in missing}
                if deprecated:
                    ext_counts["deprecated_missing"] += 1
                elif all(v == "no" for v in hwm.values()):
                    ext_counts["impossible"] += 1
                elif any(v in ("yes", "emulate") for v in hwm.values()):
                    ext_counts["missing_possible"] += 1
                else:
                    ext_counts["missing_unknown"] += 1
                gaps["extensions"].append({"name": e, "group": g, "missing_on": missing, "hw": hwm, "core": core,
                                           "required_by": {k: t["cite"] for k, t in pr.items()},
                                           "zink": z, "angle": an["extensions"].get(e), "gamescope": gs.get(e),
                                           "panvk": pvv, "blob": bl, "deprecated": deprecated,
                                           "platform": d.get("platform"), "cite": d["cite"]})
            for x, st in zip(GPUS, os_):
                if st == "yes":
                    off = [m for m in reg["ext_feature_members"].get(e, []) if ours[x]["features"].get(m) is False]
                    if off:
                        gaps["ext_feature_bits"].append({"extension": e, "gpu": x, "false": off})
        w("")
    counts["extensions"] = ext_counts

    # ---------------------------------------------------------------- core features
    w("## Core feature bits (VkPhysicalDeviceFeatures, Vulkan11/12/13/14Features)")
    w("")
    feat_counts = {"have": 0, "missing_possible": 0, "impossible": 0, "missing_unknown": 0}
    seen = set()
    for st in CORE_FEATURE_STRUCTS:
        w("### %s" % st)
        w("")
        w("| feature | profiles | zink | ANGLE | PanVK v10/v11/v13 | Arm blob v10/v11/v13 | ours v10/v11/v13 | hw v10/v11/v13 | notes |")
        w("|---|---|---|---|---|---|---|---|---|")
        for f in reg["core_features"][st]:
            if f in seen:
                continue
            seen.add(f)
            pr = req_by(prof, "features", f)
            prs = " ".join(k + ("(alt)" if t.get("alt") else "") for k, t in pr.items())
            pvv = pv["features"].get(f, {}).get("vals", {})
            pvs = "/".join(mark(pvv.get(x)) for x in GPUS) if pvv else "-/-/-"
            bl = "/".join(("Y" if blob[x]["features"].get(f) else "-") if x in blob else "?" for x in GPUS)
            os_ = [bool(ours[x]["features"].get(f)) for x in GPUS]
            hws, notes = [], []
            for x, has in zip(GPUS, os_):
                hv, nt = hw_possible(refs, cur, "feat", f, x, has)
                hws.append(hv)
                if nt and nt not in notes and not nt.startswith(("PanVK", "Arm")):
                    notes.append(nt)
            w("| `%s` | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                f, prs, zink_feat.get(f, ""), "Y" if f in an["features"] else "", pvs, bl,
                "/".join(mark(x) for x in os_), "/".join(hws), "; ".join(notes)))
            if all(os_):
                feat_counts["have"] += 1
            else:
                missing = [x for x, h in zip(GPUS, os_) if not h]
                hwm = {x: h for x, h in zip(GPUS, hws) if x in missing}
                if all(v == "no" for v in hwm.values()):
                    feat_counts["impossible"] += 1
                elif any(v in ("yes", "emulate") for v in hwm.values()):
                    feat_counts["missing_possible"] += 1
                else:
                    feat_counts["missing_unknown"] += 1
                gaps["features"].append({"name": f, "struct": st, "missing_on": missing, "hw": hwm,
                                         "required_by": {k: t["cite"] for k, t in pr.items()},
                                         "zink": zink_feat.get(f), "angle": an["features"].get(f),
                                         "panvk": pvv, "blob": bl})
        w("")
    counts["features"] = feat_counts

    # ---------------------------------------------------------------- limits
    w("## Limits and properties the profiles require")
    w("")
    w("Only rows where at least one of our GPUs falls short (or the value is not reported) are listed; the full comparison is in `data/gaps.json`.")
    w("")
    w("| property | limittype | requirement (profile: value) | ours v10 / v11 / v13 | Arm blob v10 / v11 / v13 |")
    w("|---|---|---|---|---|")
    allprops = set()
    for _, pn in PROFILE_COLS:
        for p in profile_chain(prof, pn):
            allprops |= set(prof.get(p, {}).get("req", {}).get("properties", {}))
    for m in sorted(allprops):
        lt = reg["limittype"].get(m)
        reqs = []
        bad = False
        for label, pn in PROFILE_COLS:
            t = None
            for p in profile_chain(prof, pn):
                t = prof.get(p, {}).get("req", {}).get("properties", {}).get(m)
                if t:
                    break
            if not t:
                continue
            res = [check_limit(lt, t["value"], prop(ours[x], m)) for x in GPUS]
            if any(r is not True for r in res):
                bad = True
                reqs.append("%s: %s" % (label, json.dumps(t["value"])))
                gaps["limits"].append({"property": m, "profile": label, "required": t["value"], "limittype": lt,
                                       "ours": {x: prop(ours[x], m) for x in GPUS}, "cite": t["cite"],
                                       "blob": {x: prop(blob[x], m) for x in blob},
                                       "alt": t.get("alt")})
        if bad:
            w("| `%s` | %s | %s | %s | %s |" % (m, lt, "; ".join(reqs),
                                              " / ".join(json.dumps(prop(ours[x], m)) for x in GPUS),
                                              " / ".join(json.dumps(prop(blob[x], m)) if x in blob else "?" for x in GPUS)))
    w("")
    for ver, d in sorted(zk["gl"].items()):
        for lim, (val, ln) in d["limits"].items():
            res = [ours[x]["properties"].get(lim) for x in GPUS]
            if any(r is None or r < val for r in res):
                gaps["limits"].append({"property": lim, "profile": "zink GL" + ver, "required": val, "limittype": "max",
                                       "ours": dict(zip(GPUS, res)), "cite": "%s:%d" % (zk["file"], ln)})
    zl = [g for g in gaps["limits"] if g["profile"].startswith("zink")]
    if zl:
        w("zink limit requirements not met: " + "; ".join("`%s` >= %s for %s (ours %s; %s)" % (
            g["property"], g["required"], g["profile"], "/".join(str(v) for v in g["ours"].values()), g["cite"]) for g in zl))
        w("")

    # ---------------------------------------------------------------- formats
    w("## Formats")
    w("")
    w("Every format that a profile (core minimums, roadmap, Android, desktop baseline) or zink requires. Each cell: "
      "`Y` required and ours has it on all three GPUs, `MISS` required and missing on at least one (see gaps.json), "
      "`y` not required but ours has it, `.` neither. Columns: smp sampled, flt linear filter, col colour attachment, "
      "bld blend, sto storage image, ds depth/stencil attachment, vtx vertex buffer, utb/stb uniform/storage texel buffer.")
    w("")
    w("| format | required by | " + " | ".join(k for k, _ in FMT_BITS) + " |")
    w("|---|---|" + "---|" * len(FMT_BITS))
    freq = {}
    for label, pn in PROFILE_COLS:
        for p in profile_chain(prof, pn):
            for fm, t in prof.get(p, {}).get("req", {}).get("formats", {}).items():
                e = freq.setdefault(fm, {"bits": set(), "by": {}})
                e["bits"] |= set(t["bits"])
                e["by"].setdefault(label, t["cite"])
    for ver, d in sorted(zk["gl"].items()):
        for fm, t in d["formats"].items():
            e = freq.setdefault(fm, {"bits": set(), "by": {}})
            e["bits"] |= set(t["bits"])
            e["by"].setdefault("zinkGL" + ver, "%s:%d" % (zk["file"], t["line"]))
    fmt_counts = {"required_cells": 0, "missing_cells": 0}
    for fm in sorted(freq):
        e = freq[fm]
        cells = []
        for k, bit in FMT_BITS:
            have = [bit in ours[x]["formats"].get(fm, []) for x in GPUS]
            if bit in e["bits"]:
                fmt_counts["required_cells"] += 1
                if all(have):
                    cells.append("Y")
                else:
                    fmt_counts["missing_cells"] += 1
                    cells.append("MISS")
                    gaps["formats"].append({"format": fm, "bit": bit, "missing_on": [x for x, h in zip(GPUS, have) if not h],
                                            "required_by": e["by"],
                                            "blob": {x: bit in blob[x]["formats"].get(fm, []) for x in blob}})
            else:
                cells.append("y" if all(have) else ".")
        # bits required but outside the table columns
        other = sorted(b for b in e["bits"] if b not in {bb for _, bb in FMT_BITS} and not b.startswith(("TRANSFER", "BLIT")))
        for bit in other:
            have = [bit in ours[x]["formats"].get(fm, []) for x in GPUS]
            if not all(have):
                gaps["formats"].append({"format": fm, "bit": bit, "missing_on": [x for x, h in zip(GPUS, have) if not h],
                                        "required_by": e["by"],
                                        "blob": {x: bit in blob[x]["formats"].get(fm, []) for x in blob}})
        w("| `%s` | %s | %s |" % (fm.replace("VK_FORMAT_", ""), " ".join(sorted(e["by"])), " | ".join(cells)))
    w("")
    counts["formats"] = fmt_counts

    # ---------------------------------------------------------------- consumers
    w("## What the consumers get from us")
    w("")

    def has_item(x, item):
        k, n = item.split(":", 1)
        if k == "ext":
            return n in ours[x]["extensions"]
        return bool(ours[x]["features"].get(n))

    w("### zink (OpenGL version per zink.rst requirement list)")
    w("")
    w("| GL version | missing on ours (v10 / v11 / v13) | source |")
    w("|---|---|---|")
    zmax = {x: None for x in GPUS}
    stop = {x: False for x in GPUS}
    for ver, d in sorted(zk["gl"].items()):
        miss = {}
        for x in GPUS:
            mm = [f for f in d["features"] if not ours[x]["features"].get(f)]
            mm += [e for e in d["extensions"] if e not in ours[x]["extensions"]
                   and ours_ext_state(ours[x], e, reg["extensions"].get(e, {})) == "no"]
            mm += ["%s>=%d" % (l, v[0]) for l, v in d["limits"].items() if (ours[x]["properties"].get(l) or 0) < v[0]]
            mm += ["%s:%s" % (f.replace("VK_FORMAT_", ""), "+".join(t["bits"])) for f, t in d["formats"].items()
                   if not set(t["bits"]) <= set(ours[x]["formats"].get(f, []))]
            miss[x] = mm
            if not mm and not stop[x]:
                zmax[x] = ver
            elif mm:
                stop[x] = True
        cell = " / ".join(", ".join("`%s`" % m for m in miss[x]) or "none" for x in GPUS)
        w("| %s | %s | %s:%d |" % (ver, cell, zk["file"], d["line"]))
    w("")
    w("Highest GL version whose whole chain is met: " + ", ".join("%s **%s**" % (x, zmax[x] or "<2.1") for x in GPUS) + ". "
      "(zink can still expose a lower version with extensions; this is the requirement list only.)")
    w("")
    w("### ANGLE (Chromium 126) ES version gates")
    w("")
    ag = cur["angle_gates"]
    w("Source: " + ag["_source"] + ".")
    w("")
    w("| gate | ours v10 / v11 / v13 | cite |")
    w("|---|---|---|")

    def gate_ok(x, g):
        if "limit" in g:
            v = ours[x]["properties"].get(g["limit"])
            return (v is True) if g["min"] is True else (v is not None and v >= g["min"])
        if "feature" in g:
            return bool(ours[x]["features"].get(g["feature"]))
        if "any_of" in g:
            return any(has_item(x, i) for i in g["any_of"])
        if "all_of" in g:
            return all(has_item(x, i) for i in g["all_of"])
    es = {x: "3.2" for x in GPUS}
    for lvl, key, cap in (("ES 3.0", "es30_blockers", "2.0"), ("ES 3.1", "es31_blockers", "3.0"), ("ES 3.2", "es32_needs", "3.1")):
        for g in ag[key]:
            desc = g.get("limit") or g.get("feature") or ("any of " + ", ".join(g["any_of"]) if "any_of" in g else "all of " + ", ".join(g["all_of"]))
            res = [gate_ok(x, g) for x in GPUS]
            for x, r in zip(GPUS, res):
                if not r and api_tuple(es[x]) > api_tuple(cap):
                    es[x] = cap
            w("| %s: %s | %s | %s |" % (lvl, desc, " / ".join("ok" if r else "**FAIL**" for r in res), g["cite"]))
    w("")
    w("Resulting conformant ES version from these gates: " + ", ".join("%s **%s**" % (x, es[x]) for x in GPUS) + ".")
    w("")
    w("## Counts")
    w("")
    w("```")
    w(json.dumps(counts, indent=1))
    w("```")
    w("")
    gaps["counts"] = counts
    gaps["zink_gl_max"] = zmax
    gaps["angle_es_max"] = es
    with open(a.out_md, "w") as f:
        f.write("\n".join(md))
    dump(os.path.join(DATA, "gaps.json"), gaps)
    print("wrote", a.out_md, "and", os.path.join(DATA, "gaps.json"))
    print(json.dumps(counts))


# ------------------------------------------------------------------------------------------------- check
def cmd_check(a):
    base = load(os.path.join(DATA, "ours-%s.json" % a.gpu))
    now = summarise_vulkaninfo(load(a.vulkaninfo))
    identity = ("vendorID", "deviceID", "deviceName", "driverName")
    for label, data in (("baseline", base), ("probe", now)):
        if (not data["extensions"] or not data["features"] or not data["formats"]
                or any(data["id"].get(k) in (None, "") for k in identity)
                or api_tuple(data["id"].get("apiVersion")) == (0, 0)):
            print("feature-matrix %s: incomplete %s data" % (a.gpu, label))
            return 1
    for k in identity:
        if now["id"][k] != base["id"][k]:
            print("feature-matrix %s: wrong probe %s %r (expected %r)" %
                  (a.gpu, k, now["id"][k], base["id"][k]))
            return 1
    lost = []
    lost += ["extension %s" % e for e in base["extensions"] if e not in now["extensions"] and e not in a.ignore]
    # A feature the probe does not report at all comes from a vulkaninfo too old to know its struct (CI's distro
    # vulkan-tools); the owning extension and apiVersion are still checked. Reported as false is a regression.
    unreported = [f for f, v in base["features"].items() if v and f not in now["features"]]
    lost += ["feature %s" % f for f, v in base["features"].items() if v and f in now["features"] and not now["features"][f]]
    for fm, bits in base["formats"].items():
        for b in bits:
            if b not in now["formats"].get(fm, []):
                lost.append("format %s %s" % (fm, b))
    if api_tuple(now["id"]["apiVersion"]) < api_tuple(base["id"]["apiVersion"]):
        lost.append("apiVersion")
    gained = [e for e in now["extensions"] if e not in base["extensions"]]
    gained += [f for f, v in now["features"].items() if v and not base["features"].get(f)]
    if lost:
        print("feature-matrix %s: the driver no longer advertises %d item(s) the matrix records as present:" % (a.gpu, len(lost)))
        for l in lost[:40]:
            print("   " + l)
        print("   (if this is intended, re-capture with `feature_matrix.py ours` and regenerate the matrix)")
        return 1
    msg = "feature-matrix %s: all %d extensions, %d feature bits, %d format bits still advertised" % (
        a.gpu, len(base["extensions"]), sum(1 for v in base["features"].values() if v),
        sum(len(v) for v in base["formats"].values()))
    if unreported:
        msg += "; %d feature bit(s) not reported by this vulkaninfo, not checked: %s" % (len(unreported), ", ".join(unreported[:8]))
    if gained:
        msg += "; %d new item(s) not yet in the matrix (re-capture): %s" % (len(gained), ", ".join(gained[:8]))
    print(msg)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    e = sp.add_parser("extract")
    e.add_argument("--registry", required=True)
    e.add_argument("--registry-rev", default="")
    e.add_argument("--profiles", required=True, help="Vulkan-Profiles/profiles directory")
    e.add_argument("--profiles-rev", default="")
    e.add_argument("--desktop-baseline", required=True, help="VP_LUNARG_desktop_baseline_2024.json from vkprofiles combine")
    e.add_argument("--mesa", required=True)
    e.add_argument("--mesa-rev", default="")
    e.add_argument("--angle", required=True, help="directory with vk_renderer.cpp and vk_caps_utils.cpp")
    e.add_argument("--angle-rev", default="")
    e.add_argument("--gpuinfo", action="append", default=[], help="gpu:report_id:path-to-getprofile.json")
    e.add_argument("--steam-caps", required=True)
    o = sp.add_parser("ours")
    o.add_argument("--gpu", required=True, choices=GPUS)
    o.add_argument("--vulkaninfo", required=True)
    o.add_argument("--env", default="MALI_BACKEND=fake, no MALI_EXPERIMENTAL_* set")
    g = sp.add_parser("gen")
    g.add_argument("--out-md", default=os.path.join(HERE, "..", "..", "vk", "FEATURE-MATRIX-VULKAN.md"))
    c = sp.add_parser("check")
    c.add_argument("--gpu", required=True, choices=GPUS)
    c.add_argument("--vulkaninfo", required=True)
    c.add_argument("--ignore", action="append", default=[], help="extension that may legitimately be absent")
    a = ap.parse_args()
    return {"extract": cmd_extract, "ours": cmd_ours, "gen": cmd_gen, "check": cmd_check}[a.cmd](a) or 0


if __name__ == "__main__":
    sys.exit(main())
