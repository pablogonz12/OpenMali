#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A's draw must be kicked on A, even after a second device is created."""
import json
import os
import subprocess
import sys
import tempfile

draw, icd, shaders = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="mali-two-device-") as tmp:
    dump = os.path.join(tmp, "dump.jsonl")
    env = dict(os.environ, MALI_BACKEND="fake", MALI_FAKE_GPU="v11",
               MALI_FAKE_DISJOINT_VA="1", MALI_FAKE_CHECK_CS_VA="1",
               MALI_FAKE_DUMP=dump)
    subprocess.run([draw, "--direct", icd, "--spv-dir", shaders,
                    "--second-device", "--only", "triangle"],
                   env=env, check=True, timeout=120)
    with open(dump, encoding="utf-8") as f:
        kicks = [x for x in map(json.loads, f) if x["ev"] == "kick"]
    if not kicks or any(kick["dev"] != 0 for kick in kicks):
        raise SystemExit(f"expected only device A kicks; got {kicks}")
