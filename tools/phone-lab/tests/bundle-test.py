#!/usr/bin/env python3
"""Exercise bundle creation with a fake controller and synthetic cache."""
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import zipfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    m = tmp / "lab"
    (m / "bin").mkdir(parents=True)
    (m / "android-cache").mkdir()
    ddc = m / "bin/ddc"
    ddc.write_text('#!/bin/sh\ncat "$M/registry.json"\n')
    ddc.chmod(0o755)
    (m / "registry.json").write_text('{"values":{}}')
    files = {"libvulkan_panfrost.so": b"android", "meta.json": b"{}"}
    archive = tmp / "display.tar"
    with tarfile.open(archive, "w") as t:
        for name, data in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            t.addfile(info, io.BytesIO(data))
    config = {"tar": str(archive), "gpu": "G710",
              "sha256": {k: hashlib.sha256(v).hexdigest() for k, v in files.items()}}
    (m / "android-cache/test.json").write_text(json.dumps(config))
    lib = tmp / "lib.so"
    lib.write_bytes(b"linux")
    env = dict(os.environ, M=str(m), CACHE=str(tmp / "cache"))
    args = [str(root / "bin/bundle"), "test", str(lib)]
    p = subprocess.run(args, env=env, text=True, capture_output=True, timeout=30, check=True)
    result = json.loads(p.stdout)
    with zipfile.ZipFile(result["zip"]) as z:
        assert z.read("linux/libvulkan_panfrost.so") == b"linux"
        assert z.read("android/libvulkan_panfrost.so") == b"android"
    (m / "registry.json").write_text(json.dumps({"values": {result["id"]: "{}"}}))
    p = subprocess.run(args, env=env, text=True, capture_output=True, timeout=30)
    assert p.returncode and "already registered" in p.stderr
print("synthetic pair bundle and collision checks OK")
