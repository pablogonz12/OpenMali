#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""featuremap — one chart of every API feature our consumers need, generated from public sources.

    python3 tools/featuremap.py [--missing CONSUMER] [--freeze] [--audit] [--no-fetch] [--out DIR]

Writes docs/featuremap.md and docs/featuremap.json. Python 3.8+ stdlib only.

Why it exists: capability work used to be chosen one failed device run at a time. Every requirement
here is read from the consumer's own public source at a pinned revision (file:line, verified by a
`match` string on that line at generation time), compared against what our driver reports
(M/phones/<phone>/caps-driver.txt) and what the vendor bridge reports (caps-bridge.txt), and
annotated with the hardware class / lane-day cost from M/notes. A row whose source line is gone is
DROPPED and listed under Gaps; nothing is guessed.

Nothing third-party is vendored: sources are fetched read-only into ~/.cache/featuremap/src (or read
from an existing local tree), and the output header names URL, revision, date and licence per source.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
import urllib.request
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "featuremap")
REPO = os.path.dirname(HERE)
M = os.path.expanduser("~/.config/opencode/mali-orchestrator")
GEN_VERSION = "1"

STATUS_ORDER = {"OK": 0, "OK(opt)": 0, "GATED": 1, "MISSING": 2, "MISS(opt)": 2,
                "CANNOT": 3, "?": 2, "DROPPED": 4}


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def log(msg):
    print(msg, file=sys.stderr)


# --------------------------------------------------------------------------------------------------
# sources: fetch read-only, verify sha256, never vendor

class Sources:
    def __init__(self, lock, no_fetch=False):
        self.lock = lock
        self.no_fetch = no_fetch
        self.by_id = {s["id"]: s for s in lock["sources"]}
        self.fetched, self.local, self.failed = [], [], []
        self.cache = os.path.expanduser(lock["cache"])
        os.makedirs(self.cache, exist_ok=True)

    def path(self, sid, rel):
        """Local path of `rel` inside source `sid`, fetching if needed. None when unavailable."""
        s = self.by_id[sid]
        flat = os.path.basename(rel)
        dest = os.path.join(self.cache, sid + "-" + flat) if sid not in ("vk.xml", "gl.xml") \
            else os.path.join(self.cache, s["dest"])
        if os.path.exists(dest) and os.path.getsize(dest) > 0:
            return dest
        # 1. an existing local tree, recorded by revision (never copied into the repo)
        cand = None
        if s.get("local") and rel == s.get("dest", rel):
            cand = os.path.expanduser(s["local"])
        elif s.get("local_tree"):
            cand = os.path.join(os.path.expanduser(s["local_tree"]), rel)
        if cand and os.path.exists(cand):
            self.local.append((sid, rel, cand))
            return cand
        if self.no_fetch:
            self.failed.append((sid, rel, "absent locally and --no-fetch"))
            return None
        # 2. fetch read-only at the pinned revision
        url = s.get("raw", s.get("url"))
        if "{" in url:
            url = url.format(rev=s["rev"])
        url = url + rel if s.get("raw") else s["url"]
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "featuremap/1"})
            with urllib.request.urlopen(req, timeout=60) as r:
                blob = r.read()
            with open(dest, "wb") as f:
                f.write(blob)
            self.fetched.append((sid, rel, url, hashlib.sha256(blob).hexdigest()[:16]))
            return dest
        except Exception as e:                                    # noqa: BLE001 - report, do not crash
            self.failed.append((sid, rel, "%s: %s" % (url, e)))
            return None

    def sha(self, sid, rel):
        p = self.path(sid, rel)
        if not p:
            return ""
        with open(p, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()[:16]


# --------------------------------------------------------------------------------------------------
# Vulkan / GL registries

EXT_STRUCTS = ("VkPhysicalDevice",)


class Registry:
    """vk.xml: entry points, extensions, feature-struct members (with dependencies) and limits."""

    def __init__(self, path):
        self.ok = False
        self.extensions = {}      # VK_KHR_x -> {"type", "depends", "requires", "features", "limit"}
        self.member_struct = {}   # member name -> struct name (first feature/properties/limits struct)
        self.feature_names = set()
        self.limit_names = set()
        self.commands = set()
        if not path:
            return
        try:
            root = ET.parse(path).getroot()
        except Exception as e:                                    # noqa: BLE001
            log("featuremap: vk.xml unreadable (%s); falling back to no registry" % e)
            return
        self.ok = True
        for cmd in root.findall("./commands/command"):
            n = cmd.get("name") or (cmd.findtext("proto/name") or "")
            if n:
                self.commands.add(n)
        for ext in root.findall("./extensions/extension"):
            name = ext.get("name")
            req = []
            for r in ext.findall("require"):
                req.extend(x.get("name") for x in r.findall("enum") if x.get("name"))
            self.extensions[name] = {
                "type": (ext.get("type") or "").lower(),
                "depends": ext.get("depends") or "",
                "requires": ext.get("requires") or ext.get("requiresCore") or "",
                "features": [r for r in req if r not in self.commands],
                "commands": [r for r in req if r in self.commands],
            }
        for t in root.findall("./types/type"):
            nm = t.get("name") or ""
            if not (nm.startswith(EXT_STRUCTS) and ("Features" in nm or "Properties" in nm or "Limits" in nm)):
                continue
            for m in t.findall("member"):
                mn = (m.findtext("name") or "").strip()
                if mn and mn not in ("sType", "pNext") and mn not in self.member_struct:
                    self.member_struct[mn] = nm
            if nm in ("VkPhysicalDeviceFeatures", "VkPhysicalDeviceVulkan11Features",
                      "VkPhysicalDeviceVulkan12Features", "VkPhysicalDeviceVulkan13Features",
                      "VkPhysicalDeviceVulkan14Features"):
                for m in t.findall("member"):
                    mn = (m.findtext("name") or "").strip()
                    if mn and mn not in ("sType", "pNext"):
                        self.feature_names.add(mn)
            if nm == "VkPhysicalDeviceLimits":
                for m in t.findall("member"):
                    mn = (m.findtext("name") or "").strip()
                    if mn:
                        self.limit_names.add(mn)

    def ext_for_field(self, field):
        """extDepthClipEnable -> VK_EXT_depth_clip_enable (via the registry's own extension names)."""
        best = None
        for name in self.extensions:
            parts = name.split("_")
            if len(parts) < 3:
                continue
            camel = parts[1].lower() + "".join(w[:1].upper() + w[1:] for w in parts[2:])
            if camel == field:
                return name
            if camel == field.replace("Extension", ""):
                best = name
        return best

    def section_for(self, member):
        return self.member_struct.get(member, "")


class GLRegistry:
    """gl.xml: which ARB_/EXT_ extension names exist (validates the wined3d GL side)."""

    def __init__(self, path):
        self.extensions = set()
        self.ok = False
        if not path:
            return
        try:
            for line in open(path, "r", encoding="utf-8", errors="replace"):
                m = re.search(r'<extension name="(GL_[A-Za-z0-9_]+)"', line)
                if m:
                    self.extensions.add(m.group(1))
            self.ok = bool(self.extensions)
        except OSError as e:
            log("featuremap: gl.xml unreadable (%s)" % e)


# --------------------------------------------------------------------------------------------------
# caps dumps (vulkaninfo text written by bin/smoke)

class Caps:
    def __init__(self, path, label):
        self.label = label
        self.path = path            # with ~, never expanded: the docs are committed
        self.real = os.path.expanduser(path)
        self.present = False
        self.api = ""
        self.features = {}     # member -> (bool, section)
        self.limits = {}
        self.props = {}
        self.extensions = set()
        self.ext_known = False      # the dump lists names, not just revision lines
        self.ext_blank = 0
        if not path or not os.path.exists(self.real):
            return
        self.present = True
        section = ""
        for raw in open(self.real, "r", encoding="utf-8", errors="replace"):
            line = raw.rstrip("\n")
            if not line.strip():
                continue
            if not line.startswith(("\t", " ")) and (
                    line.endswith(":") or re.match(r"^[A-Za-z][\w /()-]*:\s*count = \d+$", line)):
                section = line.split(":")[0].strip()
                continue
            body = line.strip()
            if section.startswith("Device Extensions"):
                m = re.match(r"(VK_[A-Za-z0-9_]+)\s*:\s*extension revision", body)
                if m:
                    self.extensions.add(m.group(1))
                    self.ext_known = True
                elif "extension revision" in body:
                    self.ext_blank += 1
                continue
            if " = " not in body:
                continue
            k, v = body.split(" = ", 1)
            k, v = k.strip(), v.strip()
            if line.startswith(("\t", " ")) and section == "VkPhysicalDeviceProperties":
                self.props[k] = v
                if k == "apiVersion":
                    self.api = v
            if section == "VkPhysicalDeviceLimits":
                try:
                    self.limits[k] = int(v.split()[0])
                except ValueError:
                    pass
            elif v in ("true", "false"):
                # Boolean members of any section, not only *Features: requirements read
                # transformFeedbackQueries from a VkPhysicalDevice...PropertiesEXT section, and a
                # Boolean dropped here is reported MISSING however the device answers.
                self.features.setdefault(k, (v == "true", section))
        self.version = _ver(self.props.get("apiVersion", ""))

    def feature(self, name, section=""):
        """Look the member up, preferring the struct the consumer's own line named."""
        if name in self.features:
            val, sec = self.features[name]
            if not section or sec == section or "Vulkan1" in section and "Vulkan1" in sec:
                return val
        cands = [(v, s) for k, (v, s) in self.features.items() if k == name]
        return cands[0][0] if cands else None

    def limit(self, name):
        return self.limits.get(name)


def _ver(s):
    m = re.match(r"(\d+)\.(\d+)\.(\d+)", s or "")
    return tuple(int(x) for x in m.groups()) if m else (0, 0, 0)


def gl_version(phone):
    """The GL version the phone's games see, from the last smoke result (machine-readable)."""
    p = os.path.join(M, "phones", phone, "smoke-latest.json")
    try:
        d = load(p)
    except Exception:                                             # noqa: BLE001
        return None, ""
    for line in d.get("gl") or []:
        m = re.search(r"Max core profile version:\s*(\d+\.\d+)", str(line))
        if m:
            return m.group(1), os.path.relpath(p, os.path.expanduser("~"))
    return None, ""


# --------------------------------------------------------------------------------------------------
# rows

class Row:
    def __init__(self, consumer, spec):
        self.consumer = consumer
        self.spec = spec
        self.id = spec["id"]
        self.file = spec.get("file", "")
        self.line = spec.get("line", 0)
        self.match = spec.get("match", "")
        self.verified = False
        self.drift = False
        self.dropped = None
        self.ours = None          # value as measured
        self.ours_txt = "-"
        self.bridge = None
        self.bridge_txt = "-"
        self.status = "?"
        self.test = []
        self.notes = []

    def __repr__(self):
        return "<Row %s %s>" % (self.consumer, self.id)


def _snake(name):
    out = []
    for i, ch in enumerate(name):
        if ch.isupper() and i and not name[i - 1].isupper():
            out.append("_")
        out.append(ch.lower())
    return "".join(out)


def covdb_candidates(row, reg):
    """dEQP case names for a row, derived from vk.xml's struct name — the caller keeps only the ones
    the database actually has, so a stale guess can never produce a test id."""
    out = []
    vk = row.spec.get("vk", "")
    if vk.startswith("VK_"):
        parts = vk.split("_")
        out.append("dEQP-VK.api.info.vulkan1p2_limits_validation." +
                   "_".join(x.lower() for x in parts[1:]))
        return out
    sec = row.spec.get("section") or reg.section_for(vk)
    m = re.match(r"VkPhysicalDevice(.*?)Features(KHR|EXT|AMD|NV)?$", sec or "")
    if not m:
        return out
    core, vendor = m.group(1), m.group(2)
    base = "dEQP-VK.api.info.get_physical_device_properties2.features."
    for name in (_snake(core) + "_features" + ("_" + vendor.lower() if vendor else ""),
                 _snake(core) + "_features"):
        out.append(base + name)
    return out


def read_lines(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read().split("\n")


def verify_source(row, path):
    """The rule: the match must be on the pinned line (else found exactly once elsewhere -> DRIFT)."""
    if not path:
        row.dropped = "source file unavailable"
        return False
    lines = read_lines(path)
    if 1 <= row.line <= len(lines) and row.match in lines[row.line - 1]:
        row.verified = True
        return True
    hits = [i + 1 for i, l in enumerate(lines) if row.match in l]
    if len(hits) == 1:
        row.line = hits[0]
        row.drift = True
        row.verified = True
        row.notes.append("line re-pinned %d (upstream moved)" % hits[0])
        return True
    if hits:
        row.line = hits[0]
        row.drift = True
        row.verified = True
        row.notes.append("match occurs %d times; first at %d" % (len(hits), hits[0]))
        return True
    row.dropped = "match %r not found in %s" % (row.match[:50], row.file)
    return False


def extract_dxvk(spec, path, reg, fname=""):
    """Rows from getFeatureList(): ENABLE_FEATURE(version, name, require) / ENABLE_EXT / ENABLE_EXT_FEATURE."""
    lines = read_lines(path)
    start = 0
    if spec.get("from"):
        for i, l in enumerate(lines):
            if spec["from"] in l:
                start = i
                break
    rows = []
    for i in range(start, len(lines)):
        line = lines[i]
        if spec.get("to") and i > start and spec["to"] in line:
            break
        m = re.search(spec["regex"], line)
        if not m:
            continue
        macro, args = m.group(1), [a.strip() for a in m.group(2).split(",")]
        if macro == "FEATURE" and len(args) == 3:
            ver, name, req = args
            sec = {"core": "VkPhysicalDeviceFeatures", "vk11": "VkPhysicalDeviceVulkan11Features",
                   "vk12": "VkPhysicalDeviceVulkan12Features", "vk13": "VkPhysicalDeviceVulkan13Features",
                   "vk14": "VkPhysicalDeviceVulkan14Features"}.get(ver, "")
            rows.append({"id": "dxvk-" + name, "family": "feature", "vk": name, "section": sec,
                         "file": fname, "ver": ver,
                         "required": req == "true", "line": i + 1, "match": line.strip()})
        elif macro == "EXT" and len(args) == 2:
            field, req = args
            ext = reg.ext_for_field(field) if reg.ok else None
            rows.append({"id": "dxvk-" + field, "family": "extension", "file": fname,
                         "vk": ext or ("VK_" + field), "required": req == "true",
                         "line": i + 1, "match": line.strip(), "field": field})
        elif macro == "EXT_FEATURE" and len(args) == 3:
            field, name, req = args
            rows.append({"id": "dxvk-" + name, "family": "feature", "vk": name, "file": fname,
                         "section": reg.section_for(name) if reg.ok else "",
                         "required": req == "true", "line": i + 1, "match": line.strip(),
                         "ext_field": field})
    if spec.get("pins"):
        for r in rows:
            if r["id"] in spec["pins"]:
                r["line"] = spec["pins"][r["id"]]
    return rows


# --------------------------------------------------------------------------------------------------
# evaluation

def worst_status(statuses):
    vals = [s for s in statuses if s]
    if not vals:
        return "?"
    return max(vals, key=lambda s: STATUS_ORDER.get(s, 2))


def evaluate(row, caps_by_phone, bridge, ann, reg, tests, args):
    sp = row.spec
    fam = sp.get("family", "feature")
    a = ann["requirement"].get(row.id) or ann["vk"].get(sp.get("vk", "")) or {}
    row.ann = a
    if fam == "internal":
        path = os.path.join(REPO, row.file)
        row.verified = verify_source(row, path)
        if not row.verified:
            row.status = "?"
            return
        row.ours = True
        row.ours_txt = "in tree (line verified)"
        extra_ok = True
        for extra in sp.get("expect_present", []):
            f, _, tok = extra.partition(":")
            ep = os.path.join(REPO, f)
            if not os.path.exists(ep) or (tok and tok not in open(ep, encoding="utf-8", errors="replace").read()):
                extra_ok = False
                row.notes.append("missing %s" % extra)
        row.status = "OK" if extra_ok else "MISSING"
        return

    if not row.verified:
        return

    if fam == "constant":
        row.ours = sp.get("value")
        row.ours_txt = "%s (consumer hardcodes it)" % (str(row.ours).lower())
        # a hardcoded true is met by construction: without a status the row and everything derived
        # from it printed "?" and a dependent row counted a non-blocker as a blocker.
        optional = not sp.get("required", True)
        row.status = ("OK(opt)" if optional else "OK") if row.ours else \
            ("MISS(opt)" if optional else "MISSING")
    elif fam == "gl_version":
        got, src = gl_version(args.phone)
        row.ours = got
        row.ours_txt = "%s (smoke gl line)" % got if got else "no smoke gl line"
        row.notes.append("from %s" % src if src else "no smoke result")
        need = str(sp.get("need", "0"))
        try:
            ok = tuple(int(x) for x in got.split(".")) >= tuple(int(x) for x in need.split("."))
        except (AttributeError, ValueError):
            ok = False
        row.status = "OK" if ok else "MISSING"
        return
    elif fam == "derived":
        deps = sp.get("derived_from") or []
        row.ours_txt = "via " + ", ".join(deps)
        statuses = []
        for d in deps:
            dr = args.index.get(d)
            if dr is None:
                row.notes.append("dep %s unknown (dropped?)" % d)
                statuses.append("?")
            else:
                statuses.append(dr.status)
        row.status = worst_status(statuses)
        row.ours = row.status
        if args.phone_caps and args.phone_caps.present:
            row.bridge = None
        return
    elif fam == "limit":
        need = sp.get("need")
        vals = {}
        for ph, c in caps_by_phone.items():
            v = c.limit(sp["vk"]) if c.present else None
            if v is None and sp.get("section"):
                v = c.limit(sp["vk"])
            vals[ph] = v
        row.ours = vals.get(args.phone, next(iter(vals.values()), None))
        row.ours_txt = "/".join("%s=%s" % (ph, vals.get(ph)) for ph in vals) if vals else "-"
        if bridge is not None and bridge.present:
            row.bridge = bridge.limit(sp["vk"])
            row.bridge_txt = str(row.bridge)
        ok = all(v is not None and (v == need if sp.get("op") == "==" else v >= need)
                 for v in vals.values()) if vals else False
        optional = not sp.get("required", True)
        if ok:
            row.status = "OK(opt)" if optional else "OK"
        else:
            cls = a.get("hw", "unknown")
            row.status = "CANNOT" if cls == "impossible" else \
                ("MISS(opt)" if optional else "MISSING")
            if cls == "unknown":
                row.notes.append("hardware class unknown: fill annotations.json")
        return
    elif fam == "extension":
        ext = sp.get("vk", "")
        if not all(c.ext_known for c in caps_by_phone.values() if c.present):
            row.ours = None
            row.ours_txt = "caps dump has no extension names"
            row.notes.append("device-extension section of caps-driver.txt has %d revision lines and "
                             "0 names: cannot answer from this dump"
                             % max(c.ext_blank for c in caps_by_phone.values() if c.present))
            if bridge is not None and bridge.present and bridge.ext_known:
                row.bridge = ext in bridge.extensions
                row.bridge_txt = "yes" if row.bridge else "no"
            row.status = "?"
            return
        got = {ph: (ext in c.extensions) if c.present else None for ph, c in caps_by_phone.items()}
        row.ours = got.get(args.phone, next(iter(got.values()), None))
        row.ours_txt = "%s=%s" % (args.phone, "yes" if row.ours else "no") if got else "-"
        if bridge is not None and bridge.present:
            row.bridge = ext in bridge.extensions
            row.bridge_txt = "yes" if row.bridge else "no"
        ok = bool(got) and all(got.values())
        optional = not sp.get("required", True)
        if ok:
            row.status = "OK(opt)" if optional else "OK"
        else:
            cls = a.get("hw", "unknown")
            row.status = "CANNOT" if cls == "impossible" else \
                ("MISS(opt)" if optional else "MISSING")
            if cls == "unknown":
                row.notes.append("hardware class unknown: fill annotations.json")
            blanks = max((c.ext_blank for c in caps_by_phone.values() if c.present), default=0)
            if blanks:
                row.notes.append("%d device-extension lines in the caps dump have no name; a "
                                 "nameless line could be this extension" % blanks)
        return
    else:
        name = sp.get("vk") or sp.get("name") or ""
        sec = sp.get("section", "")
        got = {}
        for ph, c in caps_by_phone.items():
            got[ph] = c.feature(name, sec) if c.present else None
        row.ours = got.get(args.phone, next(iter(got.values()), None))
        if row.ours is None and sp.get("requires_any"):
            # "feature OR a limit condition": evaluate the alternatives
            for alt in sp["requires_any"]:
                if "==" in alt:
                    k, v = alt.split("==")
                    if args.phone_caps and args.phone_caps.limit(k.strip()) == int(v):
                        row.ours = True
        row.ours_txt = "%s=%s" % (args.phone, str(row.ours).lower()) if got else "-"
        if bridge is not None and bridge.present:
            row.bridge = bridge.feature(name, sec)
            if row.bridge is None and sp.get("requires_any"):
                row.bridge = False
                for alt in sp["requires_any"]:
                    if "==" in alt:
                        k, v = alt.split("==")
                        if bridge.limit(k.strip()) == int(v):
                            row.bridge = True
            row.bridge_txt = str(row.bridge).lower()
        ok = bool(got) and all(v is True for v in got.values())
        optional = not sp.get("required", True)
        if ok:
            row.status = "OK(opt)" if optional else "OK"
        else:
            cls = a.get("hw", "unknown")
            gated = sp.get("gated_by")
            row.status = "CANNOT" if cls == "impossible" else \
                ("MISS(opt)" if optional else "GATED" if gated else "MISSING")
            if cls == "unknown":
                row.notes.append("hardware class unknown: fill annotations.json")

    if row.spec.get("gated_by") and row.status in ("MISSING", "OK"):
        row.notes.append("gate: " + row.spec["gated_by"])


# --------------------------------------------------------------------------------------------------
# collector

class Args:
    pass


def build(args):
    lock = load(os.path.join(DATA, "sources.lock.json"))
    ann_all = load(os.path.join(DATA, "annotations.json"))
    tests = load(os.path.join(DATA, "tests.json"))
    games = load(os.path.join(DATA, "games.json"))["games"]
    falsify = load(os.path.join(DATA, "falsify.json"))["rows"]
    warnings, gaps = [], []

    src = Sources(lock, args.no_fetch)
    reg = Registry(src.path("vk.xml", "vk.xml"))
    glreg = GLRegistry(src.path("gl.xml", "gl.xml"))

    caps_conf = lock["caps"]
    caps_by_phone, bridges = {}, {}
    for ph in caps_conf["phones"]:
        caps_by_phone[ph] = Caps(caps_conf["driver"].format(phone=ph), ph)
    bridge = None
    for ph in caps_conf["phones"]:
        cand = Caps(caps_conf["bridge"].format(phone=ph), ph)
        if cand.present:
            bridge = cand
            break
    bridge = bridge or Caps("", "-")
    args.phone_caps = caps_by_phone.get(args.phone)

    covdb = os.path.expanduser(caps_conf["covdb"])
    covdb_db = os.path.join(os.path.dirname(os.path.dirname(covdb)), "covdb.sqlite")
    covdb_present = os.path.exists(covdb) and os.path.exists(covdb_db)

    # ---- collect rows
    rows, dropped = [], []
    ann = {"vk": ann_all.get("vk", {}), "requirement": ann_all.get("requirement", {})}
    index = {}
    for fn in sorted(os.listdir(os.path.join(DATA, "consumers"))):
        if not fn.endswith(".json"):
            continue
        c = load(os.path.join(DATA, "consumers", fn))
        name = c["consumer"]
        specs = []
        for ex in c.get("extract", []):
            p = src.path(c["ref"], ex["file"])
            if not p:
                gaps.append("consumer %s: extract source %s unavailable" % (name, ex["file"]))
                continue
            got = extract_dxvk(ex, p, reg, ex["file"])
            if ex.get("pins"):
                pass
            specs.extend(got)
        for r in c.get("explicit", []):
            r = dict(r)
            r.setdefault("id", name + "-" + r["match"][:24])
            specs.append(r)
        for sp in specs:
            row = Row(name, sp)
            row.ann = ann["requirement"].get(row.id) or ann["vk"].get(sp.get("vk", ""), {})
            rows.append(row)
            index[row.id] = row
        c["_rows"] = specs

    # ---- verify sources, evaluate
    args.index = index
    consumers_meta = {}
    for fn in sorted(os.listdir(os.path.join(DATA, "consumers"))):
        if not fn.endswith(".json"):
            continue
        c = load(os.path.join(DATA, "consumers", fn))
        consumers_meta[c["consumer"]] = c
        for row in [r for r in rows if r.consumer == c["consumer"]]:
            p = os.path.join(REPO, row.file) if c["kind"] == "internal" else None
            if c["kind"] != "internal" and not row.verified:
                p = src.path(c["ref"], row.file)
                verify_source(row, p)
            if row.dropped:
                dropped.append(row)
                gaps.append("%s/%s dropped: %s" % (row.consumer, row.id, row.dropped))
                continue
            # validate the token against the registry
            vk = row.spec.get("vk", "")
            if reg.ok and vk:
                if vk.startswith("VK_"):
                    if vk not in reg.extensions:
                        row.notes.append("not a vk.xml extension name")
                elif vk not in reg.feature_names and vk not in reg.limit_names \
                        and vk not in reg.member_struct:
                    row.notes.append("not a vk.xml member")
            glext = row.spec.get("gl_ext", "")
            if glreg.ok and glext.startswith(("ARB_", "EXT_", "OES_")) and \
                    ("GL_" + glext) not in glreg.extensions:
                row.notes.append("not a gl.xml extension")
    # derived rows read other rows' status: evaluate everything else first
    for row in rows:
        if row.dropped or row.spec.get("family") == "derived":
            continue
        evaluate(row, caps_by_phone, bridge, ann, reg, tests, args)
    for row in rows:
        if row.dropped or row.spec.get("family") != "derived":
            continue
        evaluate(row, caps_by_phone, bridge, ann, reg, tests, args)

    # ---- tests
    cases_path = os.path.join(REPO, tests["cases_file"])
    cases_txt = open(cases_path, encoding="utf-8", errors="replace").read() if os.path.exists(cases_path) else ""
    tdir = os.path.join(REPO, tests["tests_dir"])
    for row in rows:
        ids = tests["requirement"].get(row.id) or tests["consumer"].get(row.consumer) or []
        if row.spec.get("test"):
            ids = row.spec["test"]
        good = []
        for t in ids:
            ok = bool(re.search(r'run "%s' % re.escape(t), cases_txt)) or \
                ("--only " + t) in cases_txt or ("only " + t) in cases_txt or \
                os.path.exists(os.path.join(tdir, t + ".c")) or \
                os.path.exists(os.path.join(tdir, "t_" + t + ".c")) or \
                os.path.exists(os.path.join(tdir, "build-" + t + ".sh"))
            if ok:
                good.append(t)
            else:
                row.notes.append("test %s not found" % t)
        row.test = good
        if not good:
            gaps.append("%s/%s has no test" % (row.consumer, row.id))

    # ---- deqp coverage ids: joined only where the exact case exists in covdb
    covdb_hits = 0
    if covdb_present:
        try:
            import sqlite3
            con = sqlite3.connect("file:%s?mode=ro" % covdb_db, uri=True)
            for row in rows:
                if status_cell(row) in ("DROPPED",) or not reg.ok:
                    continue
                cands = covdb_candidates(row, reg)
                for c in cands:
                    got = list(con.execute('select status, count(*) from "case" where name = ? '
                                           'group by status', (c,)))
                    if got:
                        row.covdb = (c, ", ".join("%d %s" % (n, st) for st, n in sorted(got)))
                        covdb_hits += 1
                        break
            con.close()
        except Exception as e:                                    # noqa: BLE001
            gaps.append("covdb unreadable: %s" % e)
    for row in rows:
        if not hasattr(row, "covdb"):
            row.covdb = None

    # ---- results per lane (games)
    lane_rows = {}
    for g in games:
        blocking = set(g.get("blocked_by", []))
        ids = set(blocking)
        # every other hard row of that game's consumer that is not met blocks it too
        for row in rows:
            if row.consumer == g["consumer"] and row.spec.get("required", True) \
                    and status_cell(row) in ("MISSING", "CANNOT"):
                ids.add(row.id)
        lane_rows[g["id"]] = ids

    dependents = {}
    for row in rows:
        for d in row.spec.get("derived_from") or []:
            dependents.setdefault(d, []).append(row.id)

    def games_unlocked(row, seen=None):
        """Directly blocked games, plus those of every row that derives from this one (the zink row
        under a wined3d condition unlocks the same games as the condition)."""
        seen = seen or set()
        if row.id in seen:
            return set()
        seen.add(row.id)
        gs = {g for g, bl in lane_rows.items() if row.id in bl}
        for d in dependents.get(row.id, []):
            other = index.get(d)
            if other is not None:
                gs |= games_unlocked(other, seen)
        return gs

    def mid(cost):
        m = re.findall(r"[\d.]+", cost or "")
        if not m:
            return None
        v = [float(x) for x in m[:2]]
        return sum(v) / len(v) if v else None

    for row in rows:
        a = row.ann
        row.games = len(games_unlocked(row))
        row.cost = a.get("cost", "")
        row.mid = mid(row.cost)

    # ---- falsification
    fals = []
    for f in falsify:
        r = index.get(f["requirement"])
        if r is None:
            fals.append((f, None, "row missing"))
            continue
        if f["check"] == "status":
            got = r.status
        elif f["check"] == "ours":
            got = r.ours
            if isinstance(got, bool):
                got = "true" if got else "false"
            got = str(got)
        else:
            got = "-"
        exp = str(f["expect"])
        fals.append((f, got, "PASS" if got == exp else "FAIL"))

    return dict(lock=lock, src=src, reg=reg, glreg=glreg, rows=rows, dropped=dropped,
                caps=caps_by_phone, bridge=bridge, warnings=warnings, gaps=gaps,
                consumers=consumers_meta, games=games, lane_rows=lane_rows, fals=fals,
                covdb=covdb_present, args=args)


# --------------------------------------------------------------------------------------------------
# output

def status_cell(row):
    s = row.status
    if row.spec.get("gated_by") and s == "MISSING":
        return "GATED"
    return s


def hw_cell(row):
    a = row.ann or {}
    hw = a.get("hw", "unknown")
    cost = a.get("cost", "")
    if not cost and row.spec.get("family") == "internal":
        cost = "in tree"
    return "%s%s" % (hw, (" / " + cost + " d" if cost and cost not in ("in tree",) else ""))


def table_for(rows, extra_cols=()):
    head = "| requirement | vk / gl member | ours | bridge | hardware / cost | test | status |"
    out = [head, "|---|---|---|---|---|---|---|"]
    for r in rows:
        vk = r.spec.get("vk") or r.spec.get("gl_ext") or r.spec.get("name") or "-"
        t = ", ".join(r.test) if r.test else "-"
        if getattr(r, "covdb", None):
            t += " + deqp `%s` (%s)" % (r.covdb[0].replace("dEQP-VK.", ""), r.covdb[1])
        out.append("| `%s`%s | `%s` | %s | %s | %s | %s | **%s** |" % (
            r.id, (" †" if r.drift else ""), vk, r.ours_txt, r.bridge_txt,
            hw_cell(r), t, status_cell(r)))
    return out


def _dump_time(caps):
    mt = [os.path.getmtime(c.real) for c in caps.values() if os.path.exists(c.real)]
    return datetime.datetime.fromtimestamp(max(mt)).strftime("%Y-%m-%d %H:%M") if mt else "no caps dump"


def render(res, args):
    lock, rows = res["lock"], [r for r in res["rows"] if not r.dropped]
    now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    try:
        commit = subprocess.run(["git", "-C", REPO, "rev-parse", "--short", "HEAD"],
                                capture_output=True, text=True, timeout=10).stdout.strip()
    except Exception:                                             # noqa: BLE001
        commit = "?"
    lines = []
    w = lines.append

    w("# featuremap — every API feature our consumers need, where it comes from, and where we stand")
    w("")
    w("Generated by `tools/featuremap.py` (v%s) at %s from **%s**; do not edit by hand — edit "
      "`tools/featuremap/` and regenerate." % (GEN_VERSION, now, commit))
    w("")
    w("One row per capability a consumer requires, read from that consumer's own public source at a "
      "pinned revision (the `file:line` is verified against a `match` string on every run), compared "
      "with what our driver advertises (`M/phones/<phone>/caps-driver.txt`) and what the vendor "
      "bridge advertises (`caps-bridge.txt`). The purpose is that implementation is chosen from this "
      "table instead of from one failed device run at a time.")
    w("")
    w("Status: **OK** met and advertised · **OK(opt)** optional for that consumer and met · **GATED** "
      "met only behind an env gate · **MISSING** required, absent, hardware can do it honestly or by "
      "emulation · **MISS(opt)** optional and absent · **CANNOT** required, absent, and the hardware "
      "has no such thing · **?** no source-line-backed class yet.")
    w("")
    w("## Sources (fetched read-only at generation time; nothing vendored)")
    w("")
    w("| source | revision | date | licence | what we take from it | sha256(16) |")
    w("|---|---|---|---|---|---|")
    for s in lock["sources"]:
        what = s["what"]
        rev = s.get("revname") or s.get("rev")
        shas = []
        for f in s.get("files", [s.get("dest", "")]):
            h = res["src"].sha(s["id"], f) if f else ""
            if h:
                shas.append(h)
        w("| `%s` | `%s` | %s | %s | %s | %s |" % (
            s["id"], rev, s.get("date", "?"), s.get("license", "?"), what,
            ", ".join(sorted(set(shas))[:3]) or "-"))
    w("")
    if res["src"].fetched:
        w("Fetched this run: " + "; ".join("%s/%s" % (a, b) for a, b, _, _ in res["src"].fetched) + ".")
    else:
        w("No fetch needed this run: every source was already in `~/.cache/featuremap/src` or an "
          "existing local tree (recorded above by revision).")
    w("")
    if res["src"].failed:
        w("**Unavailable this run:** " + "; ".join("%s/%s (%s)" % f for f in res["src"].failed) + ".")
        w("")
    w("Our own data: " + ", ".join("`%s` (%s)" % (c.path.replace("~/.config/", "M/"),
                                                 c.api or "no apiVersion")
                                  for c in res["caps"].values()) +
      " (dumped %s); bridge `%s`%s." % (_dump_time(res["caps"]), res["bridge"].path
                            if res["bridge"].present else "-",
                            "" if res["bridge"].present else " (absent: bridge column empty)"))
    w("")
    hits = sum(1 for r in rows if getattr(r, "covdb", None))
    w("Coverage database (`covdb`, `vk/deqp/covdb.sqlite`): **%s**. The test column carries a "
      "`vk/check.sh` case or a `vk/tests/` source always, and a dEQP case id for **%d of %d rows** — "
      "only where the exact case name exists in the database, so the join can never be a guess. The "
      "dEQP id is struct-level (`…properties2.features.<struct>_features` passes on our driver), not "
      "a per-member result." % ("present" if res["covdb"] else "absent", hits, len(rows)))
    w("")

    # ---- per-consumer tables
    w("## The chart, per consumer")
    w("")
    order = ["dxvk", "vkd3d-proton", "wined3d-gl", "wined3d-vk", "zink", "icd-own"]
    for name in order:
        c = res["consumers"].get(name)
        if not c:
            continue
        crows = [r for r in rows if r.consumer == name]
        ok = sum(1 for r in crows if status_cell(r) in ("OK", "OK(opt)"))
        w("### %s — %d rows, %d met" % (c["title"], len(crows), ok))
        w("")
        if c.get("note"):
            w(c["note"])
            w("")
        lines.extend(table_for(crows))
        w("")
        for r in crows:
            if r.notes:
                w("- `%s`: %s" % (r.id, "; ".join(r.notes)))
        if any(r.notes for r in crows):
            w("")
        w("† = the pinned line moved upstream this run (the generator re-pinned it, `--freeze` "
          "commits the new number).")
        w("")

    # ---- Direct3D levels
    w("## Which functions buy which D3D feature level")
    w("")
    w("This is the 'which functions buy which games' view. wined3d's GL backend derives the level "
      "from the conditions in `feature_level_from_caps()` (`dlls/wined3d/adapter_gl.c:1242-1281`, "
      "wine-11.0) — it has **no 11_0 branch**: ask for 11_0 and you get 11_1 or 10_1. Each level row "
      "below lists the chart rows that gate it and whether they are met today.")
    w("")
    levels = [
        ("D3D9 (9_1/9_2/9_3)", ["wined3d-gl32-context"]),
        ("D3D10 / 10_0", ["wined3d-gl32-context", "wined3d-fl10_0-polygon-offset-clamp"]),
        ("D3D10.1", ["wined3d-gl32-context", "wined3d-fl10_0-polygon-offset-clamp",
                     "wined3d-fl10_1-cube-map-array", "wined3d-fl10_1-draw-buffers-blend"]),
        ("D3D11 FL 10_0 (GAME PATH TODAY)", ["wined3d-gl32-context",
                                             "wined3d-fl10_0-polygon-offset-clamp",
                                             "wined3d-fl10_1-cube-map-array",
                                             "wined3d-fl10_1-draw-buffers-blend"]),
        ("D3D11 FL 11_0 → 11_1 (GL route ceiling of the 4-branch)", [
            "wined3d-gl32-context", "wined3d-fl10_0-polygon-offset-clamp",
            "wined3d-fl11_1-draw-indirect", "wined3d-fl11_1-bptc", "wined3d-sm5-compute",
            "wined3d-sm5-cull-distance", "wined3d-sm5-tessellation"]),
        ("D3D11 via DXVK (Vulkan native, no GL)", ["dxvk-geometryShader", "dxvk-shaderCullDistance",
                                                   "dxvk-robustBufferAccess2", "dxvk-descriptorIndexing"]),
        ("D3D12 via vkd3d-proton", ["vkd3d-xfb-queries", "vkd3d-texel-offset-single",
                                    "vkd3d-uniform-texel-offset-single", "vkd3d-push-descriptor",
                                    "vkd3d-robustness2-buffer", "vkd3d-null-descriptor",
                                    "vkd3d-shader-draw-parameters", "vkd3d-maintenance56"]),
    ]
    w("| level / route | gated by | met? | blocked rows (what to buy next) |")
    w("|---|---|---|---|")
    idx = {r.id: r for r in rows}
    for lvl, ids in levels:
        have = [idx[i] for i in ids if i in idx]
        okn = sum(1 for r in have if status_cell(r) in ("OK", "OK(opt)"))
        blocked = [r.id for r in have if status_cell(r) not in ("OK", "OK(opt)")]
        w("| **%s** | %d rows | %d/%d | %s |" % (lvl, len(have), okn, len(have),
                                                  ", ".join("`%s`" % b for b in blocked) or "—"))
    w("")
    w("Capability groups per API level (the same rows, grouped the way a game's engine log names "
      "them):")
    w("")
    w("| group | needs | ours |")
    w("|---|---|---|")
    for grp, ids in [
        ("shader model 4 (FL 10_1)", ["wined3d-fl10_1-cube-map-array", "wined3d-fl10_1-draw-buffers-blend"]),
        ("shader model 5 (FL 11_1)", ["wined3d-sm5-compute", "wined3d-sm5-cull-distance",
                                      "wined3d-sm5-tessellation"]),
        ("compute", ["zink-ARB_compute_shader-max-threads"]),
        ("bindless / D3D12", ["vkd3d-descriptor-heap-scale", "dxvk-descriptorIndexing"]),
        ("geometry stage", ["dxvk-geometryShader", "wined3d-vk-geometry-shader"]),
        ("stream output", ["vkd3d-xfb-queries", "zink-ARB_transform_feedback3"]),
    ]:
        have = [idx[i] for i in ids if i in idx]
        w("| %s | %s | %s |" % (grp, ", ".join("`%s`" % r.id for r in have),
                                ", ".join(status_cell(r) for r in have)))
    w("")

    # ---- falsification
    w("## Falsification of the chart (every run)")
    w("")
    w("Five rows whose measured fact is written in `M/notes/`, re-derived from the machine sources: "
      "if the chart disagrees with a note, the generator or the note is wrong — never the number.")
    w("")
    w("| row | measured fact (note) | chart: source | chart: value | verdict |")
    w("|---|---|---|---|---|")
    idxf = {r.id: r for r in rows}
    for f, got, verdict in res["fals"]:
        r = idxf.get(f["requirement"])
        src = "`%s:%d`" % (r.file, r.line) if r else "-"
        w("| `%s` | %s | %s | `%s`=%s | **%s** |" % (
            f["requirement"], f["note"], src, f["check"], got, verdict))
    w("")
    bad = [f for f, _, v in res["fals"] if v != "PASS"]
    w("**%d/%d rows agree with the notes.**" % (len(res["fals"]) - len(bad), len(res["fals"])))
    w("")

    # ---- top 20
    w("## Top 20 rows by games unlocked per lane-day")
    w("")
    w("Cost is the midpoint of the lane-day range in `annotations.json` (all of which come from "
      "`M/notes/`); games unlocked counts the games in `games.json` whose consumer treats the row as "
      "a hard requirement and whose notes name it as blocking, plus the games of any row that "
      "derives from this one through the consumer chain (zink -> wined3d -> level). A row with no cost is excluded: an "
      "unestimated row is not a cheap row.")
    w("")
    w("| # | row | games | cost (lane-days) | games per lane-day | status |")
    w("|---|---|---|---|---|---|")
    cand = [r for r in rows if r.mid and r.games and status_cell(r) not in ("OK", "OK(opt)")]
    cand.sort(key=lambda r: (-(r.games / r.mid), r.mid, r.id))
    for i, r in enumerate(cand[:20], 1):
        w("| %d | `%s` | %d | %s (%.2f) | %.2f | %s |" % (
            i, r.id, r.games, r.cost, r.mid, r.games / r.mid, status_cell(r)))
    if not cand:
        w("| — | (no row with both a cost and a game) | — | — | — | — |")
    w("")

    # ---- gaps
    w("## Gaps and what is not machine-readable")
    w("")
    w("- **Microsoft's D3D reference pages** have no machine-readable registry, so D3D requirements "
      "are taken from the layers that consume them (DXVK, vkd3d-proton, wined3d), pinned by "
      "revision above. A D3D-level row that no consumer source states is not in this chart.")
    w("- **The Vulkan spec prose** (semantics, valid usage) is not parsed: rows carry the registry "
      "entry and the consumer's line, not the spec paragraph.")
    w("- **Our own hardware classes** (`annotations.json`) are judgements from `M/notes/`, not "
      "machine facts; each names the note. Rows with no note print `unknown`.")
    w("- **deqp ids are struct-level**, not per member: `dEQP-VK.api.info.get_physical_device_properties2"
      ".features.vulkan12_features` PASS says our 1.2 struct was accepted, not that every member is "
      "implemented. Per-feature behaviour still needs the `vk/tests/` case in the same row.")
    w("- **dEQP has no case per feature in every group** (a LIKE search for one requirement matched "
      "5 687 unrelated `texel` cases), so no id is attached when the exact name is absent.")
    w("- **What our phones actually report** comes from the last `bin/smoke` caps dump; a stale dump "
      "gives stale `ours` values (the file mtime is in the header line above).")
    w("")
    if res["gaps"]:
        w("### Rows dropped or incomplete this run")
        w("")
        for g in res["gaps"]:
            w("- " + g)
        w("")
    if not res["gaps"]:
        w("No row was dropped and every row has a test this run.")
    w("")
    blankers = [(ph, c) for ph, c in res["caps"].items() if c.present and c.ext_blank]
    if blankers:
        w("**Data bug found by this generator:** in the *Device Extensions* section of "
          + ", ".join("`phones/%s/caps-driver.txt` %d of %d lines carry no extension name" %
                      (ph, c.ext_blank, len(c.extensions) + c.ext_blank) for ph, c in blankers)
          + ". A row that reports an extension absent is therefore only certain for the named "
            "lines; the nameless ones cannot be read as present or absent. Extension rows print "
            "`?` only when *no* name survived. Reported for `bin/smoke`, not fixed from a host-only "
            "lane.")
        w("")
    w("### Notes built on")
    w("")
    w("`M/REQS.md` (the hand-seeded matrix this replaces), `M/notes/route-decision-2.md`, "
      "`M/notes/feature-level.md`, `M/notes/view-format.md`, `M/notes/txf-fix.md`, "
      "`vk/R4-GAP.md`, and `M/notes/featuremap.md` for this generator itself.")
    w("")
    return "\n".join(lines) + "\n"


def render_json(res):
    rows = []
    for r in res["rows"]:
        rows.append({
            "id": r.id, "consumer": r.consumer, "family": r.spec.get("family"),
            "vk": r.spec.get("vk"), "gl_ext": r.spec.get("gl_ext"),
            "required": r.spec.get("required", True),
            "source": {"ref": res["consumers"][r.consumer].get("ref"), "file": r.file,
                       "line": r.line, "match": r.match, "verified": r.verified,
                       "drift": r.drift},
            "ours": r.ours, "ours_text": r.ours_txt,
            "bridge": r.bridge,
            "hardware": r.ann.get("hw", "unknown"), "cost_days": r.ann.get("cost", ""),
            "unlocks": r.ann.get("unlocks", ""), "note": r.ann.get("note", ""),
            "tests": r.test, "status": status_cell(r), "games_unlocked": r.games,
            "caveats": r.notes,
        })
    return {
        "generated": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "generator": "tools/featuremap.py v%s" % GEN_VERSION,
        "commit": subprocess.run(["git", "-C", REPO, "rev-parse", "HEAD"], capture_output=True,
                                 text=True).stdout.strip(),
        "sources": [{k: s.get(k) for k in ("id", "rev", "revname", "date", "license", "url")}
                    for s in res["lock"]["sources"]],
        "caps": {ph: {"path": c.path, "api": c.api, "present": c.present}
                 for ph, c in res["caps"].items()},
        "bridge": {"path": res["bridge"].path, "present": res["bridge"].present},
        "covdb": res["covdb"],
        "rows": rows,
        "dropped": [{"consumer": r.consumer, "id": r.id, "why": r.dropped} for r in res["dropped"]],
        "falsification": [{"requirement": f["requirement"], "check": f["check"],
                           "expected": f["expect"], "got": got, "verdict": v}
                          for f, got, v in res["fals"]],
        "gaps": res["gaps"],
    }


# --------------------------------------------------------------------------------------------------

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--missing", metavar="CONSUMER",
                    help="print the requirements CONSUMER needs and we lack, numbered, with file:line")
    ap.add_argument("--freeze", action="store_true",
                    help="write re-pinned file:line values back into tools/featuremap/consumers/*.json")
    ap.add_argument("--audit", action="store_true",
                    help="show what upstream changed since the pin (added/removed requirements)")
    ap.add_argument("--no-fetch", action="store_true", help="never touch the network")
    ap.add_argument("--phone", default="x8", help="phone whose caps dump is the 'ours' column (x8)")
    ap.add_argument("--out", default=os.path.join(REPO, "docs"), help="output directory")
    args = ap.parse_args(argv)

    res = build(args)
    rows = res["rows"]

    if args.missing:
        name = args.missing
        gone = [r for r in rows if r.consumer == name and
                status_cell(r) in ("MISSING", "MISS(opt)", "CANNOT", "GATED", "?")]
        if not gone:
            print("featuremap: no missing requirements for %r (or unknown consumer)." % name)
            return 0
        hard = [r for r in gone if r.spec.get("required", True)]
        print("# %s needs %d hard + %d optional thing(s) we lack (generated %s; newest source "
              "revision in docs/featuremap.md header)" % (
                  name, len(hard), len(gone) - len(hard),
                  datetime.datetime.now().strftime("%Y-%m-%d")))
        for i, r in enumerate(sorted(gone, key=lambda r: (not r.spec.get("required", True),
                                                          -r.games, r.id)), 1):
            print("%2d. %-42s %s:%d  %s  ours=%s wanted=%s  [%s]  test=%s" % (
                i, r.id, r.file, r.line, r.spec.get("vk") or r.spec.get("gl_ext") or "",
                r.ours_txt, "true" if r.spec.get("required", True) else "optional",
                status_cell(r), ",".join(r.test) or "-"))
        return 0

    if args.audit:
        changed = 0
        for fn in sorted(os.listdir(os.path.join(DATA, "consumers"))):
            if not fn.endswith(".json"):
                continue
            c = load(os.path.join(DATA, "consumers", fn))
            for ex in c.get("extract", []):
                p = res["src"].path(c["ref"], ex["file"])
                if not p:
                    continue
                now = {(r["id"], r["line"]) for r in extract_dxvk(ex, p, res["reg"], ex["file"])}
                pins = ex.get("pins") or {}
                for rid, ln in sorted(now):
                    if rid in pins and pins[rid] != ln:
                        changed += 1
                        print("%s: %s pinned at %d, upstream %d" % (c["consumer"], rid, pins[rid], ln))
        print("featuremap --audit: %d changed line(s)" % changed)
        return 0

    md = render(res, args)
    os.makedirs(args.out, exist_ok=True)
    md_path = os.path.join(args.out, "featuremap.md")
    js_path = os.path.join(args.out, "featuremap.json")
    with open(md_path, "w", encoding="utf-8") as f:
        f.write(md)
    with open(js_path, "w", encoding="utf-8") as f:
        json.dump(render_json(res), f, indent=1, sort_keys=False)
        f.write("\n")

    if args.freeze:
        n = 0
        for fn in sorted(os.listdir(os.path.join(DATA, "consumers"))):
            if not fn.endswith(".json"):
                continue
            path = os.path.join(DATA, "consumers", fn)
            c = load(path)
            crows = {r.id: r for r in rows if r.consumer == c["consumer"]}
            for ex in c.get("extract", []):
                pins = ex.setdefault("pins", {})
                for rid, r in crows.items():
                    if r.line and pins.get(rid) != r.line:
                        pins[rid] = r.line
                        n += 1
            for i, sp in enumerate(c.get("explicit", [])):
                r = crows.get(sp["id"])
                if r and r.line and sp.get("line") != r.line:
                    c["explicit"][i]["line"] = r.line
                    n += 1
            with open(path, "w", encoding="utf-8") as f:
                json.dump(c, f, indent=2)
                f.write("\n")
        print("featuremap --freeze: %d line(s) pinned" % n)

    total = len(rows)
    ok = sum(1 for r in rows if status_cell(r) in ("OK", "OK(opt)"))
    bad = [f for f, _, v in res["fals"] if v != "PASS"]
    print("featuremap: %d rows (%d met, %d dropped) -> %s, %s; falsification %d/%d" % (
        total, ok, len(res["dropped"]), os.path.relpath(md_path, REPO),
        os.path.relpath(js_path, REPO), len(res["fals"]) - len(bad), len(res["fals"])))
    for g in res["gaps"][:20]:
        print("  gap: " + g)
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
