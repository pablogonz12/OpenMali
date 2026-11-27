#!/usr/bin/env python3
"""Exercise soak argument forwarding with a fake smoke."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

soak = Path(__file__).resolve().parents[1] / "bin/soak"
with tempfile.TemporaryDirectory() as d:
    root = Path(d)
    (root / "bin").mkdir()
    stub = root / "bin/smoke"
    stub.write_text("#!/usr/bin/env python3\nimport json,sys\nprint(json.dumps(sys.argv[1:]))\n")
    stub.chmod(0o755)
    env = dict(os.environ, M=d)
    p = subprocess.run([str(soak), "x4", "--minutes", "0", "--pacing-scroll", "5"],
                       env=env, text=True, capture_output=True, check=True)
    assert json.loads(p.stdout) == ["x4", "--no-launch", "--soak-internal", "0",
                                    "--soak-label-internal", "", "--pacing-scroll", "5"]
    p = subprocess.run([str(soak), "x4", "--minutes", "0", "--pacing-scroll", "0"],
                       env=env, text=True, capture_output=True)
    assert p.returncode == 2
print("soak CLI: zero-minute forwarding and scroll validation OK")
