#!/usr/bin/env python3
"""Fixture test: bench2 metrics()/agg() carry 1% low, >25 ms and >50 ms, and report() prints them for offscreen runs too."""
import importlib.machinery, importlib.util, json, os, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ld = importlib.machinery.SourceFileLoader("bench2mod", os.path.join(HERE, "bench2"))
B = importlib.util.module_from_spec(importlib.util.spec_from_loader("bench2mod", ld))
ld.exec_module(B)


def run(fps, low, o25, o50):
    return {"pass": True, "mdbench": {"fps": fps, "fps_1pct_low": low, "over_25ms": o25, "over_50ms": o50, "frames_measured": 100,
            "cpu_ms": {"record": {"mean": 0.5}, "submit": {"mean": 0.25}}, "gpu_ms": {"mean": 1.5},
            "frame_time_ms": {"all": {"p50": 2.0, "p99": 9.0}}, "present_mode": "mailbox", "swapchain_images": 0, "width": 1280, "height": 720,
            "config": {"frames_in_flight": 2}, "device": {"driver_info": "x"}, "load": {"draws": 3000}}}


class T(unittest.TestCase):
    def test_metrics_and_agg(self):
        ms = [B.metrics(run(500, 80, 7, 3), "ours"), B.metrics(run(520, 90, 5, 1), "ours"), B.metrics(run(510, 85, 6, 2), "ours")]
        self.assertEqual((ms[0]["over25"], ms[0]["over50"], ms[0]["low1"]), (7, 3, 80))
        a = B.agg(ms)
        self.assertEqual((a["low1"], a["over25"], a["over50"]), (85, 6, 2))

    def test_report_has_offscreen_columns(self):
        pres = dict(fps=300.0, low1=70.0, over25=1, over50=0, cpu_ms=1.0, gpu_ms=2.0)
        off = dict(fps=600.0, low1=123.4, over25=11, over50=22, cpu_ms=0.5, gpu_ms=1.0)
        row = {"date": "2026-11-19", "phone": "px", "class": "v13", "driver": "ours", "version": "abc", "commit": "abc", "variant": "triangle-3k",
               "bench": "triangle", "presented": pres, "offscreen": off, "best_settings": "s"}
        with tempfile.TemporaryDirectory() as d:
            jl, md = os.path.join(d, "r.jsonl"), os.path.join(d, "R.md")
            open(jl, "w").write(json.dumps(row) + "\n")
            old = (B.JSONL, B.MD, B.MENU, B.PENDING)
            B.JSONL, B.MD, B.MENU, B.PENDING = jl, md, os.path.join(d, "none.jsonl"), os.path.join(d, "none.json")
            try:
                B.report()
            finally:
                B.JSONL, B.MD, B.MENU, B.PENDING = old
            t = open(md).read()
        self.assertIn("off 1% low | off >25 ms | off >50 ms", t)
        self.assertIn("| 600.0 | 123.4 | 11 | 22 | 0.50 | 1.00 |", t)
        for l in t.splitlines():                                    # every table row has the header's column count
            if l.startswith("| px"):
                self.assertEqual(l.count("|"), 19)


if __name__ == "__main__":
    unittest.main()
