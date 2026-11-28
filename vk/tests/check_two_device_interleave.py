#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Two live VkDevices, interleaved (t_vk_meta --second-device --only interleave).

A records a CLEAR pass, B records a LOAD pass (preload meta draw) with an in-pass fill, B is submitted before A,
A is destroyed while B's fence is still pending and B is submitted again. On the fake backend with disjoint
per-context VA windows, every kick must come from the device that recorded it and every address golden.py can
reach from a kick (RUN_* register pairs, descriptors, SRT entries, sync addresses, attachments, tiler heap) must
be a BO of that same context: a foreign address is "not inside any BO (it is a BO of devN)".
"""
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
meta, icd = sys.argv[1:3]
gpus = sys.argv[3:] or ["v11", "v13"]
WINDOW = 1 << 32
VA_LO = 0x5f00000000
NDEV = 2

os.environ["GOLDEN_VA_HI"] = "%x" % (VA_LO + NDEV * WINDOW)
import golden  # noqa: E402  (reads GOLDEN_VA_HI at import)

fail = []
for gpu in gpus:
    with tempfile.TemporaryDirectory(prefix="mali-interleave-") as tmp:
        dump = os.path.join(tmp, "dump.jsonl")
        env = dict(os.environ, MALI_BACKEND="fake", MALI_FAKE_GPU=gpu, MALI_FAKE_DISJOINT_VA="1",
                   MALI_FAKE_CHECK_CS_VA="1", MALI_FAKE_SLOW_US="20000,600000", MALI_FAKE_DUMP=dump)
        p = subprocess.run([meta, "--direct", icd, "--second-device", "--only", "interleave"], env=env,
                           capture_output=True, text=True, timeout=120)
        if p.returncode != 0 or '"test":"interleave"' not in p.stdout:
            fail.append("%s: t_vk_meta rc=%d\n%s%s" % (gpu, p.returncode, p.stdout[-2000:], p.stderr[-2000:]))
            continue
        if "B fence pending when A is destroyed" not in p.stdout:
            fail.append("%s: B's fence had signalled before A was destroyed (scenario not exercised)" % gpu)
        kicks, rings = {}, []
        with open(dump, encoding="utf-8") as f:
            for e in map(json.loads, f):
                if e["ev"] == "kick":
                    kicks[e["dev"]] = kicks.get(e["dev"], 0) + 1
                elif e["ev"] == "ring":
                    rings.append(e)
        # A: one submit; B: two submits (each submit may be more than one kick).
        if kicks.get(0, 0) < 1 or kicks.get(1, 0) < 2 or set(kicks) != {0, 1}:
            fail.append("%s: expected kicks on dev0 (>=1) and dev1 (>=2) only, got %s" % (gpu, kicks))
        # Direct immediates: any word that lies in some device's window must lie in the kicking device's window.
        for e in rings:
            d = e["dev"]
            hexs = e["hex"]
            for i in range(0, len(hexs), 16):
                w = int.from_bytes(bytes.fromhex(hexs[i:i + 16]), "little")
                va = w & ((1 << 48) - 1)
                if VA_LO <= va < VA_LO + NDEV * WINDOW and (va - VA_LO) // WINDOW != d:
                    fail.append("%s: dev%d ring word %d = %#x is in dev%d's window" %
                                (gpu, d, i // 16, va, (va - VA_LO) // WINDOW))
                    break
        os.environ["GOLDEN_ARCH"] = gpu[1:]
        n, viol = golden.process(dump, os.path.join(tmp, "dump.txt"))
        rva = sorted(set(v for v in viol if "R-VA" in v))
        if n < 3 or rva:
            fail.append("%s: golden walked %d kicks, %d R-VA violations:\n  %s" % (gpu, n, len(rva), "\n  ".join(rva[:12])))
        else:
            print("%s: %d kicks (%s), every reachable address owned by its kicking device" %
                  (gpu, n, ", ".join("dev%d=%d" % kv for kv in sorted(kicks.items()))))

# MALI_SUBMIT_DBG names the device of a lost submit: inject a fault into the first submit (B's, device #1) and
# require the lost line to carry B's serial and the VkDevice its creation line printed.
import re  # noqa: E402
env = dict(os.environ, MALI_BACKEND="fake", MALI_FAKE_GPU=gpus[0], MALI_FAULT_INJECT="1", MALI_SUBMIT_DBG="1")
p = subprocess.run([meta, "--direct", icd, "--second-device", "--only", "interleave"], env=env,
                   capture_output=True, text=True, timeout=120)
created = dict(re.findall(r"mali_vk: device #(\d+) VkDevice=(\S+) kdev=\S+ created", p.stderr))
lost = re.findall(r"mali_vk: submit lost: device #(\d+) VkDevice=(\S+) .*step=(\w+)", p.stderr)
if p.returncode == 0 or not lost or lost[0][0] != "1" or created.get("1") != lost[0][1]:
    fail.append("fault injection: want the first lost submit on device #1 (B) with its creation VkDevice; "
                "created=%s lost=%s rc=%d\n%s" % (created, lost, p.returncode, p.stderr[-1500:]))
else:
    print("fault injection: lost submit names device #%s VkDevice=%s step=%s" % lost[0])

if fail:
    raise SystemExit("two-device interleave FAILED:\n" + "\n".join(fail))
