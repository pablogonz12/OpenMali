#!/usr/bin/env python3
import os, sys, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gpuablib import rotate, quantile, iqr, parse, summarize

class T(unittest.TestCase):
    def test_rotate(self):
        v = ["a", "b", "c"]
        self.assertEqual([rotate(v, r) for r in range(4)], [["a","b","c"], ["b","c","a"], ["c","a","b"], ["a","b","c"]])
        first = [rotate(v, r)[0] for r in range(6)]; last = [rotate(v, r)[-1] for r in range(6)]
        self.assertEqual({x: first.count(x) for x in v}, {"a": 2, "b": 2, "c": 2})
        self.assertEqual({x: last.count(x) for x in v}, {"a": 2, "b": 2, "c": 2})
        self.assertEqual(v, ["a", "b", "c"])          # input untouched
        self.assertEqual(rotate(["x"], 5), ["x"])
    def test_quantile(self):
        self.assertEqual(quantile([1, 2, 3, 4, 5], .5), 3)
        self.assertAlmostEqual(quantile([1, 2, 3, 4], .25), 1.75)
        self.assertAlmostEqual(iqr([1, 2, 3, 4, 5]), 2)
        self.assertEqual(iqr([7]), 0)
    LOG = "".join(f"RUN {n} cube {r} temp={t}\n{{\"fps\":{f}\nSUM gpu={g} n=10\n" for n, r, t, f, g in [
        ("ours", 0, 380, 500, 0.60), ("bridge", 0, 380, 450, 0.50),
        ("ours", 1, 390, 500, 0.66), ("bridge", 1, 390, 450, 0.55),
        ("ours", 2, 450, 500, 9.99), ("bridge", 2, 450, 450, 9.99)])
    def test_parse_drops_hot(self):
        res = parse(self.LOG.replace('{"fps"', '"fps"'), 43.0)
        self.assertEqual(res[("cube", "ours")], [(0, 0.60, 500.0), (1, 0.66, 500.0)])   # round 2 at 45 C dropped
    def test_summarize_paired(self):
        res = parse(self.LOG, 43.0); s = summarize(res, ["cube"], ["ours", "bridge"])
        self.assertAlmostEqual(s[("cube", "ours")]["med"], 0.63); self.assertEqual(s[("cube", "ours")]["ratio"], 1.0)
        self.assertAlmostEqual(s[("cube", "bridge")]["ratio"], (0.5 / 0.6 + 0.55 / 0.66) / 2)
        self.assertEqual(s[("cube", "ours")]["n"], 2)
    def test_missing_variant(self):
        self.assertEqual(summarize({}, ["cube"], ["a"]), {})
if __name__ == "__main__": unittest.main()
