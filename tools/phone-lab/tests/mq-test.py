#!/usr/bin/env python3
"""Host-only queue tests with fake smoke/soak binaries."""
import json
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import time

mq = Path(__file__).resolve().parents[1] / "bin/mq"
loader = importlib.machinery.SourceFileLoader("mq_test_module", str(mq))
spec = importlib.util.spec_from_loader(loader.name, loader)
module = importlib.util.module_from_spec(spec)
loader.exec_module(module)
for verdict in ("REFUSED", "IMPORT_HUNG", "INTERRUPTED", "INVALID_RESULT"):
    assert module.blocks({"verdict": verdict}, "")
assert module.blocks({"restored": False}, "")
assert module.blocks({"reason": "PHONE_LOCKED by someone"}, "")
assert module.blocks({}, "REFUSED: PHONE_LOCKED by someone")
assert not module.blocks({"verdict": "SHELF_OK", "restored": True}, "")
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    (root / "phones.conf").write_text("a|stub|pkg|60\nb|stub|pkg|60\n")
    stub = """#!/usr/bin/env python3
import json,sys,time
time.sleep(.35)
if '--refuse' in sys.argv: print(json.dumps({'verdict':'REFUSED','reason':'PHONE_LOCKED'}))
else: print(json.dumps({'verdict':'SHELF_OK','restored':True}))
"""
    for tool in ("smoke", "soak"):
        p = root / "bin" / tool
        p.write_text(stub)
        p.chmod(0o755)
    env = dict(os.environ, M=str(root))
    def call(*args, check=True):
        return subprocess.run([str(mq), *args], env=env, text=True, capture_output=True,
                              timeout=10, check=check)
    def submit(phone, *args):
        return json.loads(call("submit", phone, "--", *args).stdout)["id"]
    def wait(job):
        return json.loads(call("wait", job, "--timeout", "5").stdout)
    assert call("submit", "a", "--", "import", check=False).returncode
    (root / "queue").mkdir(exist_ok=True)
    (root / "queue/STOP").touch()
    first = submit("a", "smoke")
    other = submit("b", "soak")
    bad = submit("a", "smoke", "--refuse")
    after = submit("a", "smoke")
    daemon = subprocess.Popen([str(mq), "run"], env=env, stdout=subprocess.DEVNULL,
                              stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(.3)
        assert not (root / "queue/results.jsonl").exists()
        (root / "queue/STOP").unlink()
        a, b, c = wait(first), wait(other), wait(bad)
        assert abs(a["started"] - b["started"]) < .3
        assert c["started"] >= a["finished"]
        assert c["blocked"] and (root / "queue/a.blocked").exists()
        assert call("wait", after, "--timeout", ".1", check=False).returncode
        status = json.loads(call("status").stdout)
        assert next(x for x in status["phones"] if x["phone"] == "a")["queued"] == 1
    finally:
        daemon.terminate()
        daemon.wait(timeout=5)
    # Crash recovery must block an uncertain in-flight job, not replay it.
    (root / "queue/b.inflight").write_text(json.dumps({"id": "uncertain", "phone": "b"}))
    daemon = subprocess.Popen([str(mq), "run"], env=env, stdout=subprocess.DEVNULL,
                              stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 4
        while not (root / "queue/b.blocked").exists() and time.monotonic() < deadline:
            time.sleep(.05)
        assert (root / "queue/b.blocked").exists()
    finally:
        daemon.terminate()
        daemon.wait(timeout=5)
print("mq: STOP, allowlist, parallel/FIFO, blocking, wait and crash recovery OK")
