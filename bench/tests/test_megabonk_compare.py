import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import megabonk_compare as m
F = Path(__file__).parent / "fixtures" / "megabonk"

class T(unittest.TestCase):
    def test_ours(self):
        r = m.load_run(F / "ours")
        self.assertEqual(r["driver"], "ours"); self.assertEqual(r["problems"], [])
        self.assertEqual(r["gaps_gt25"], 10)      # 33 ms x8 + 200 ms x2; the 25 ms bin is not a gap
        self.assertEqual(r["max_gap_ms"], 200); self.assertEqual(r["max_c"], 38.0)
        self.assertEqual(r["p99_ms"], 24);  self.assertEqual(r["low1pct_fps"], 41.7)
    def test_bridge(self):
        r = m.load_run(F / "bridge")
        self.assertEqual(r["driver"], "bridge"); self.assertEqual(r["gaps_gt25"], 20)
        self.assertEqual(r["low1pct_fps"], 10.0)  # 99th pct lands in the 100 ms bin
    def test_mismatch_flagged(self):
        r = m.load_run(F / "mismatch")
        self.assertTrue(any("mismatch" in p for p in r["problems"]))
    def test_no_evidence_never_attributed(self):
        r = m.load_run(F / "noevidence")
        self.assertEqual(r["driver"], "unknown"); self.assertTrue(any("NO DRIVER" in p for p in r["problems"]))
    def test_table_and_missing_files(self):
        self.assertIn("| ours |", m.table([m.load_run(F / "ours")]).replace("  ", " ").replace("megabonk/", ""))
        r = m.load_run(F / "does-not-exist"); self.assertIn("no pacing.json", r["problems"])
if __name__ == "__main__": unittest.main()
