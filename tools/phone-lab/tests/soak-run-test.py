#!/usr/bin/env python3
"""Exercise soak with fake clock and fake ddc, no phone."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import threading
import time

M = Path(__file__).resolve().parents[1]
os.environ["M"] = str(M)
spec = importlib.util.spec_from_file_location("soakrun", M / "bin/soak-run.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
now = [0.]
old = mod.schedule.run
old_pacing = mod.schedule.pacing
reader_started = threading.Event()
def fake_sleep(s):
    assert reader_started.wait(5), "background reader did not start"
    now[0] += s
mod.schedule.run = lambda minutes, cb: old(minutes, cb, lambda: now[0], fake_sleep)
mod.schedule.pacing = lambda duration, interval, cb: old_pacing(
    duration, interval, cb, lambda: now[0], fake_sleep)
calls = []
input_times = []
def fake(args, timeout=25):
    calls.append(args)
    if args[0].endswith("/img"):
        Path(args[3]).write_bytes(b"jpg")
        return ""
    cmd = args[2:]
    if cmd[:2] == ["input", "pad"]:
        input_times.append((cmd[2], now[0]))
    if cmd[0] == "state":
        return json.dumps({"session": {"running": True}})
    if cmd[0] == "screenshot":
        Path(cmd[1]).write_bytes(b"png")
    if cmd[0] == "guest":
        return json.dumps({"stdout": {"text": "123 /steamwebhelper --type=gpu-process\n"}})
    if cmd[0] == "logs":
        if not reader_started.is_set():
            reader_started.set()
            time.sleep(20)  # Deliberately much slower than the input loop.
        Path(cmd[2], "steam.log").write_text("[ gpu_compositing ]: enabled\n")
    if cmd[0] == "pacing":
        return '{"fpsOnScreen":7,"frames":210,"presentToPresentMs":{"10":94,"20":5,"30":1}}'
    return "{}"
mod.command = fake
with tempfile.TemporaryDirectory() as d:
    assert mod.run("x4", d, 2) == 0
    result = json.loads(Path(d, "soak-data.json").read_text())
    assert len(result["screenshots"]) == 4 and len(result["pacing"]) == 3
    assert result["gpu_compositing"] == "enabled"
    assert result["pacing_screenshot"].endswith("pre-pacing.jpg")
    pads = [x[4] for x in calls if len(x) > 4 and x[2:4] == ["input", "pad"]]
    assert pads == ["right", "right", "right", "right", "down", "b"], pads
    assert [t for action, t in input_times if action == "right"] == [30, 60, 90, 120]
    guest = [x[2:] for x in calls if x[2:3] == ["guest"]]
    assert guest and all(x[:4] == ["guest", "--", "sh", "-c"] and len(x) == 5 and
                         x[4] == "ps -eo pid,args | grep -E '[s]teamwebhelper.*--type=gpu-process' || true"
                         for x in guest), guest
    assert "sample" in Path(d, "timeline.tsv").read_text()
    for line in Path(d, "timeline.tsv").read_text().splitlines()[1:]:
        due, actual, action, _ = line.split("\t", 3)
        if action == "right":
            assert abs(float(due) - float(actual)) <= 1, line
now[0] = 0
input_times.clear()
with tempfile.TemporaryDirectory() as d:
    assert mod.run("x4", d, 0, 5) == 0
    result = json.loads(Path(d, "soak-data.json").read_text())
    assert result["scenario"] == "steam_shelf_scroll_right_every_5s"
    assert input_times[0] == ("b", 0)
    assert [t for action, t in input_times if action == "right"] == [
        5, 10, 15, 20, 25, 35, 40, 45, 50, 55, 65, 70, 75, 80, 85]
    assert len(result["pacing"]) == 3
    for rep in result["pacing"]:
        assert (rep["displayed_fps"], rep["frames"], rep["p95_interval_ms"], rep["p99_interval_ms"]) == (7, 210, 20, 20)
    assert Path(result["pacing_screenshot"]).name == "pre-pacing.jpg"
print("fake ddc: 20-second log stall did not delay input deadlines")
