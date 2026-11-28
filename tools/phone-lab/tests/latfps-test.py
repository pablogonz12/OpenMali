#!/usr/bin/env python3
"""latfps: the built-in selftest, then `collect` end to end against a fake ddc; never contacts a phone.

The fake ddc answers --list / dumpsys display / --latency-clear itself and runs the real poll loop script with sh,
with a fake `dumpsys` first on PATH that prints a 120 Hz ring computed from the host clock."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

TOOL = Path(__file__).resolve().parents[1] / "bin/latfps"

p = subprocess.run([sys.executable, str(TOOL), "selftest"], text=True, capture_output=True, timeout=120)
assert p.returncode == 0 and "latfps selftest: OK" in p.stdout, p.stdout + p.stderr
print(p.stdout.strip())

FAKE_DDC = r'''#!/usr/bin/env python3
import os, subprocess, sys, time
d = os.environ["FAKE_DIR"]
open(os.path.join(d, "calls.log"), "a").write(repr(sys.argv[1:]) + "\n")
assert sys.argv[2:4] == ["adb", "shell"], sys.argv
s = sys.argv[4]
if s == "dumpsys SurfaceFlinger --list":
    print("RequestedLayerState{SurfaceView[com.x.y/com.x.y.SessionActivity]#91 parentId=12}")
    print("RequestedLayerState{SurfaceView[com.x.y/com.x.y.SessionActivity](BLAST)#92 parentId=91 z=-2}")
    print("RequestedLayerState{StatusBar#40 parentId=3}")
elif s == "dumpsys display":
    print("  mActiveModeId=2\n  mActiveRenderFrameRate=120.0")
elif s.startswith("dumpsys SurfaceFlinger --latency-clear "):
    open(os.path.join(d, "clear"), "w").write(str(time.time_ns()))
else:
    env = dict(os.environ, PATH=os.path.join(d, "bin") + ":" + os.environ["PATH"])
    sys.exit(subprocess.run(["sh", "-c", s], env=env).returncode)
'''
FAKE_DUMPSYS = r'''#!/usr/bin/env python3
import os, sys, time
assert sys.argv[1:3] == ["SurfaceFlinger", "--latency"] and len(sys.argv) == 4, sys.argv
assert sys.argv[3] == "SurfaceView[com.x.y/com.x.y.SessionActivity](BLAST)#92", sys.argv
P = 8333333
c = int(open(os.path.join(os.environ["FAKE_DIR"], "clear")).read())
now = time.time_ns()
n = max(0, (now - c) // P)
frames = [c + i * P for i in range(max(0, n - 127), n)]
print(P)
for _ in range(127 - len(frames)):
    print("0\t0\t0")
for f in frames:
    print("%d\t%d\t%d" % (f - P, f if f + P <= now else 2**63 - 1, f - 1000000))
print()
'''

with tempfile.TemporaryDirectory() as td:
    td = Path(td)
    (td / "bin").mkdir()
    ddc = td / "ddc"
    ddc.write_text(FAKE_DDC); ddc.chmod(0o755)
    ds = td / "bin/dumpsys"
    ds.write_text(FAKE_DUMPSYS); ds.chmod(0o755)
    (td / "phones.conf").write_text("fake|<device-serial>|com.x.y|120\n")
    env = dict(os.environ, LATFPS_DDC=str(ddc), FAKE_DIR=str(td), M=str(td))
    out = td / "run"
    p = subprocess.run([sys.executable, str(TOOL), "collect", "fake", "--seconds", "2", "--interval", "0.1",
                        "--out", str(out)], text=True, capture_output=True, timeout=60, env=env)
    r = json.loads(p.stdout.strip().splitlines()[-1])
    assert p.returncode == 0 and r["verdict"] == "OK", (p.stdout, p.stderr)
    assert r["layer"] == "SurfaceView[com.x.y/com.x.y.SessionActivity](BLAST)#92", r
    assert r["polls"] == 20 and r["missed_vsyncs"] == 0 and r["periods_share_pct"]["1"] == 100.0, r
    assert 119.9 <= r["displayed_fps"] <= 120.1 and r["frames"] > 200, r
    assert r["display_before"] and r["display_after"], r
    raw = (out / "raw.txt").read_text()
    assert raw.count("@@POLL ") == 20 and " host=" in raw and raw.rstrip().endswith("@@END"), raw[:200]
    for f in ("layers.txt", "layer.txt", "meta.json", "analysis.json", "display-before.txt", "display-after.txt"):
        assert (out / f).is_file(), f
    # an ambiguous --layer resolves nothing, and nothing is cleared or polled
    calls = (td / "calls.log").read_text().count("\n")
    p = subprocess.run([sys.executable, str(TOOL), "collect", "fake", "--layer", "SurfaceView", "--out", str(td / "r2")],
                       text=True, capture_output=True, timeout=60, env=env)
    r = json.loads(p.stdout)
    assert p.returncode == 3 and r["verdict"] == "LAYER_UNRESOLVED" and len(r["candidates"]) == 2, p.stdout
    assert (td / "calls.log").read_text().count("\n") == calls + 1   # only the --list call
    # reps over three copies of the same run: stable, median rep reported
    for i in (1, 2, 3):
        subprocess.run(["cp", "-r", str(out), str(td / ("rep%d" % i))], check=True)
    p = subprocess.run([sys.executable, str(TOOL), "reps", *(str(td / ("rep%d" % i)) for i in (1, 2, 3))],
                       text=True, capture_output=True, timeout=60)
    r = json.loads(p.stdout)
    assert p.returncode == 0 and r["verdict"] == "OK" and r["spread_fps"] == 0.0, p.stdout
print("latfps-test: OK (selftest + fake-ddc collect + LAYER_UNRESOLVED + reps)")
