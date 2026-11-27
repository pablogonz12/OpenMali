#!/usr/bin/env python3
"""Host-only mutual exclusion checks with a temporary lock directory."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as d:
    env = dict(os.environ, M=d)
    holder = subprocess.Popen(
        ["bash", "-c", '. "$1"; phone_lock x4; echo READY; read -r', "holder", str(root / "bin/lib.sh")],
        env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert holder.stdout.readline().strip() == "READY"
        # No connection to a phone: contention is checked before phone().
        for tool, args in (("smoke", ["x4", "--no-launch"]),
                           ("soak", ["x4", "--minutes", "2"]),
                           ("bundle", ["x4", "missing.so"]),
                           *(("ddc", ["x4", command]) for command in ("start", "run", "launch", "stop"))):
            # soak invokes smoke via M, so point its bin to the actual scripts.
            Path(d, "bin").symlink_to(root / "bin", target_is_directory=True) if not Path(d, "bin").exists() else None
            p = subprocess.run([str(root / "bin" / tool), *args], env=env,
                               text=True, capture_output=True, timeout=10)
            assert p.returncode != 0 and "REFUSED: PHONE_LOCKED by" in p.stderr, (tool, p.stdout, p.stderr)
            assert "since" in p.stderr
    finally:
        holder.stdin.write("\n")
        holder.stdin.flush()
        holder.wait(timeout=10)
print("phone lock: smoke, soak and ddc stop refused under contention")
