#!/usr/bin/env python3
"""Host-only fake-clock checks; never contacts a phone."""
import importlib.util
from pathlib import Path

path = Path(__file__).resolve().parents[1] / "bin" / "soak-schedule.py"
spec = importlib.util.spec_from_file_location("schedule", path)
schedule = importlib.util.module_from_spec(spec)
spec.loader.exec_module(schedule)

plan = list(schedule.events(5))
assert [(t, i) for t, a, i in plan if a == "screenshot"] == [
    (0, "start"), (150, "mid"), (300, "end")]
assert [t for t, a, _ in plan if a == "sample"] == list(range(0, 301, 60))
assert [t for t, a, _ in plan if a == "right"] == list(range(30, 301, 30))
assert [t for t, a, _ in plan if a == "down"] == [120, 240]
assert [t for t, a, _ in plan if a == "b"] == [240]
for bad in (0, -1, 1.5, True):
    try:
        list(schedule.events(bad))
        raise AssertionError(bad)
    except ValueError:
        pass

now = [1000.0]
seen = []
def callback(due, actual, action, index):
    seen.append((due, actual, action))
    if action == "right":
        now[0] += 11
def sleep(seconds):
    now[0] += seconds
schedule.run(5, callback, lambda: now[0], sleep)
assert [(due, actual) for due, actual, action in seen if action == "right"] == [
    (x, x) for x in range(30, 301, 30)]
assert any(actual > due for due, actual, action in seen if action == "down")
print("soak schedule: absolute deadlines, lateness, event counts OK")
