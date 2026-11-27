#!/usr/bin/env python3
"""Fake UI and clock; no phone. Components node based on a recorded dump."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import zipfile

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("importer", root / "bin/import-run.py")
imp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(imp)
components = {"ok": True, "windows": [{"nodes": [{"tag": "rail-components", "text": "Components"}]}]}
# Picker title is from the pinned source's drivers_pick_any string.
picker = {"ok": True, "windows": [{"nodes": [{"text": imp.PICKER}, {"text": "p.zip"}]}]}
assert imp.classify(picker)[0]
assert not imp.classify(components)[0] and imp.classify(components)[1]
assert imp.registered({"values": {"B": '{"android":"a","linux":"l"}'}}, "B")["linux"] == "l"
assert imp.watch(lambda _: components, lambda _: {"linux": "l"}, "B", 90,
                 clock=lambda: 0, sleep=lambda _: None)[0] == "IMPORTED"
now = [0]
assert imp.watch(lambda _: picker, lambda _: {"linux": "l"}, "B", 90,
                 clock=lambda: now[0], sleep=lambda n: now.__setitem__(0, now[0] + n))[0] == "IMPORT_HUNG"
with tempfile.TemporaryDirectory() as td:
    td = Path(td)
    imp.M = td / "m"
    imp.CACHE = td
    path = td / "p.zip"
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("manifest.json", json.dumps({"tag": "Bundle-X4",
            "drivers": {"linux": {"path": "linux"}}}))
        z.writestr("linux/libvulkan_panfrost.so", b"driver")
    calls = []
    def fake(args, timeout=30):
        args = list(map(str, args))
        calls.append(args)
        if args[0].endswith("phone-snap"):
            return "SNAP=/tmp/fake-snap\n"
        if "driver_bundles" in args:
            return '{"values":{}}'
        if "dump" in args:
            return json.dumps(picker)
        return "{}"
    imp.call = fake
    imp.time.monotonic = lambda: now[0]
    imp.time.sleep = lambda n: now.__setitem__(0, now[0] + n)
    now[0] = 0
    with contextlib.redirect_stdout(io.StringIO()) as result:
        assert imp.main("x4", str(path), False) == 1
    output = json.loads(result.getvalue())
    assert output["verdict"] == "IMPORT_HUNG" and now[0] == 90
    assert sum("keyevent" in args and "BACK" in args for args in calls) == 1
    assert sum("screenshot" in args for args in calls) == 1
    assert sum("click" in args and "p.zip" in args for args in calls) == 1
    assert next(i for i, x in enumerate(calls) if "ledger" in x[0]) < next(i for i, x in enumerate(calls) if "push" in x)
print("import: picker, registry, 90s watchdog, one BACK and no retry OK")
