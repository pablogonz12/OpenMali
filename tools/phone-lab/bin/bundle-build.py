#!/usr/bin/env python3
"""Internal host-side DroidDeck pair bundle builder."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import zipfile

M = Path(os.environ.get("M", Path.home() / ".config/opencode/mali-orchestrator"))
CACHE = Path(os.environ.get("CACHE", Path.home() / ".cache/mali-orchestrator"))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def registry(phone):
    p = subprocess.run([str(M / "bin/ddc"), phone, "prefs", "get", "driver_bundles"],
                       capture_output=True, text=True, timeout=40, check=True)
    return json.loads(p.stdout[p.stdout.index("{"):])


def contains(value, wanted):
    if isinstance(value, dict):
        return wanted in value or any(contains(v, wanted) for v in value.values())
    if isinstance(value, list):
        return any(contains(v, wanted) for v in value)
    if isinstance(value, str):
        if value == wanted:
            return True
        try:
            return contains(json.loads(value), wanted)
        except (ValueError, RecursionError):
            return False
    return False


def android(phone):
    config = json.loads((M / "android-cache" / (phone + ".json")).read_text())
    expected = config["sha256"]
    with tarfile.open(Path(config["tar"]).expanduser()) as archive:
        files = {}
        for member in archive.getmembers():
            name = member.name.removeprefix("./")
            if not member.isfile():
                continue
            if "/" in name or name in ("", ".", "..") or member.size > 100_000_000:
                raise ValueError("unsafe Android cache member")
            files[name] = archive.extractfile(member).read()
    required = {"libvulkan_panfrost.so", "meta.json"}
    if not required <= files.keys():
        raise ValueError("incomplete cached Android half")
    selected = required | ({"libdrm_full.so"} if "libdrm_full.so" in files else set())
    for name in selected:
        if not re.fullmatch(r"[0-9a-f]{64}", expected.get(name, "")) or sha(files[name]) != expected[name]:
            raise ValueError("Android hash missing or mismatched: " + name)
    return config, {name: files[name] for name in selected}


def main(args):
    if len(args) not in (2, 4) or (len(args) == 4 and args[2] != "--tag"):
        raise ValueError("usage: bundle <phone> <library> [--tag x]")
    phone, library = args[:2]
    tag = args[3] if len(args) == 4 else "Build"
    if not re.fullmatch(r"[A-Za-z0-9]{1,24}", tag):
        raise ValueError("tag must be 1-24 ASCII letters or digits")
    data = Path(library).read_bytes()
    if not data:
        raise ValueError("empty library")
    digest = sha(data)
    config, android_files = android(phone)
    gpu = config["gpu"]
    if not re.fullmatch(r"[A-Za-z0-9]+", gpu):
        raise ValueError("invalid GPU tag in Android cache configuration")
    bundle_id = f"PanVK-Mali-{gpu}-OpenMali{tag}{digest[:8]}{phone}"
    if contains(registry(phone), bundle_id):
        raise ValueError("bundle ID already registered: " + bundle_id)
    linux_name = f"OpenMali-{tag}-{digest[:8]}-{phone}"
    manifest = {"package": linux_name + " pair", "version": digest, "tag": bundle_id,
                "drivers": {"android": {"path": "android"}, "linux": {"path": "linux"}}}
    meta = {"kind": "linux-vulkan-icd", "name": linux_name, "driverVersion": digest[:12], "minGlibc": "2.38"}
    target = CACHE / "import" / (bundle_id + ".zip")
    target.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(target, "x", compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=2))
        z.writestr("linux/meta.json", json.dumps(meta, indent=2))
        z.writestr("linux/libvulkan_panfrost.so", data)
        for name, content in sorted(android_files.items()):
            z.writestr("android/" + name, content)
    print(json.dumps({"phone": phone, "id": bundle_id, "linux_name": linux_name,
                      "sha256": digest, "zip": str(target), "zip_sha256": sha(target.read_bytes())}))


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (ValueError, KeyError, OSError, subprocess.SubprocessError, tarfile.TarError, zipfile.BadZipFile) as exc:
        print("REFUSED: " + str(exc), file=sys.stderr)
        sys.exit(1)
