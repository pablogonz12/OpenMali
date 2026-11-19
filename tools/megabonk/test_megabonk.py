#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host-only tests for tools/megabonk (no phone, no adb): python3 -I tools/megabonk/test_megabonk.py"""
import json, os, subprocess, sys, tempfile, unittest, warnings

warnings.simplefilter("ignore")

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mblib  # noqa: E402

RUN = os.path.join(HERE, "run.sh")
OURS_NO_NAME = "driver_pref=ours\ndebug_pref=on\nvkBridge=false\n"
OURS = "driver_pref=ours\ndebug_pref=on\nvkBridge=false\ndriverName = Mali-open 0.1\n"


class Evidence(unittest.TestCase):
    def kind(self, text):
        return mblib.classify(mblib.parse_evidence(text))

    def test_ours(self):
        self.assertEqual(self.kind(OURS), "ours")

    def test_bridge_and_panvk_refused(self):
        self.assertEqual(self.kind("driver_pref=bridge\ndebug_pref=on\nvkBridge=true\n"), "bridge")
        self.assertEqual(self.kind("driver_pref=panvk\ndebug_pref=on\nvkBridge=false\ndriverName = PanVK\n"), "panvk")
        self.assertEqual(self.kind("vkBridge=true\n"), "bridge")

    def test_unknown_and_conflict(self):
        self.assertEqual(self.kind(""), "unknown")
        self.assertEqual(self.kind("driver_pref=ours\ndebug_pref=on\nvkBridge=false\n"), "unknown")   # selection alone is no proof
        self.assertEqual(self.kind("driver_pref=ours\ndebug_pref=on\nvkBridge=false\ndriverName = PanVK\n"), "conflict")
        self.assertEqual(self.kind("driver_pref=ours\ndebug_pref=on\nvkBridge=true\ndriverName = Mali-open\n"), "conflict")     # MaliBridge still on
        self.assertEqual(self.kind("driver_pref=ours\ndebug_pref=off\nvkBridge=false\ndriverName = Mali-open\n"), "conflict")   # Debug off: selection ignored
        self.assertEqual(self.kind(OURS + "info:  Vulkan: Found device: Mali-G925 (PanVK Mesa 26.3)\n"), "conflict")

    def test_dxvk_found_device_counts(self):
        self.assertEqual(self.kind("info:  Vulkan: Found device: Mali (Mali-open 0.1)\ndriver_pref=ours\ndebug_pref=on\nvkBridge=false\n"), "ours")

    def cli(self, text):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "e.txt")
            open(p, "w").write(text)
            r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "mblib.py"), "evidence", p], capture_output=True, text=True)
            return r.returncode, r.stdout

    def test_configured_icd_is_not_proof(self):
        self.assertEqual(self.kind(OURS_NO_NAME + "env=VK_DRIVER_FILES=/tmp/mali-open.json\n"), "unknown")
        self.assertEqual(self.kind(OURS_NO_NAME + "env=VK_DRIVER_FILES=/x/panvk_icd.json\n"), "conflict")

    def test_cli_refusal(self):
        rc, out = self.cli("driver_pref=bridge\ndebug_pref=on\nvkBridge=true\n")
        self.assertEqual(rc, 3)
        self.assertIn("DRIVER-EVIDENCE kind=bridge", out)
        self.assertIn("REFUSED", out)
        rc, out = self.cli(OURS)
        self.assertEqual(rc, 0)
        self.assertIn("DRIVER-EVIDENCE kind=ours", out)
        self.assertNotIn("REFUSED", out)


class Stats(unittest.TestCase):
    def test_stats(self):
        # 6000 frames in 60 s: 5900 at 8 ms, 80 at 9 ms, 20 at 40 ms
        s = mblib.stats({"frames": 6000, "presentToPresentMs": {"8": 5900, "9": 80, "40": 20}}, 60)
        self.assertAlmostEqual(s["avg_fps"], 100.0)
        self.assertEqual(s["p99_ms"], 9)
        self.assertAlmostEqual(s["pct_le_8_3ms"], 5900 / 6000 * 100)
        self.assertAlmostEqual(s["low1_fps"], 1000 / 9)

    def test_no_frames(self):
        self.assertIsNone(mblib.stats({"presentToPresentMs": {}}, 60))

    def test_errors(self):
        log = "info: ok\nerr:   DXVK: Required feature geometryShader not supported\nVK_ERROR_FEATURE_NOT_PRESENT\ninfo: fine\n"
        self.assertEqual(len(mblib.first_errors(log)), 2)


class DryRun(unittest.TestCase):
    def test_dry_run_prints_everything_and_touches_nothing(self):
        env = dict(os.environ, HOME="/nonexistent-home", PATH="/nonexistent-bin:/usr/bin:/bin")
        r = subprocess.run(["bash", RUN, "poco-x8", "--dry-run", "--wait", "5", "--secs", "7", "--out", "/nonexistent-out/x"],
                           capture_output=True, text=True, env=env)
        self.assertEqual(r.returncode, 0, r.stderr)
        for want in ("thermal", "qpreflight", "debug", "driver", "vkBridge false", "MALI_ADVERTISE_GS=1", "MALI_ADVERTISE_CULL=1", "MALI_ADVERTISE_DESCIDX=1", "MALI_SHADER_CACHE=", "rm -f /tmp/dxvk-logs",
                     "launch 3405340", "sleep 5", "pacing --seconds 7", "mblib.py evidence", "mblib.py stats", "qrestore poco-x8"):
            self.assertIn(want, r.stdout)
        self.assertFalse(os.path.exists("/nonexistent-out"))

    def test_dry_run_flags_disable_shims_and_order(self):
        r = subprocess.run(["bash", RUN, "cmf", "--dry-run", "--no-gs", "--no-cull", "--no-descidx"], capture_output=True, text=True)
        for gone in ("MALI_ADVERTISE_GS", "MALI_ADVERTISE_CULL", "MALI_ADVERTISE_DESCIDX"):
            self.assertNotIn(gone, r.stdout)
        r = subprocess.run(["bash", RUN, "cmf", "--dry-run"], capture_output=True, text=True)
        o = r.stdout
        self.assertGreater(o.index("evidence-pre.txt --allow-unknown"), o.rindex("start steam --wait --timeout"))   # gate after the restart
        self.assertLess(o.index("evidence-pre.txt --allow-unknown"), o.index("launch 3405340"))

    def test_dry_run_farm_hold(self):
        r = subprocess.run(["bash", RUN, "cmf", "--dry-run", "--farm"], capture_output=True, text=True)
        self.assertIn("farm hold cmf --max", r.stdout)
        self.assertNotIn("--farm", r.stdout)


class FakeRun(unittest.TestCase):
    """run.sh against a stub droiddeckctl: bridge evidence must refuse (no fps line) and still restore; ours must print fps."""

    def stub(self, d, driver, vkb):
        vname = "Mali-open 0.1" if driver == "ours" else "Mali bridge"
        root = os.path.join(d, "qr")
        os.makedirs(os.path.join(root, "tools"))
        os.makedirs(os.path.join(root, "scripts", "quick"))
        pacing = json.dumps({"frames": 6000, "presentToPresentMs": {"8": 5900, "9": 100}})
        open(os.path.join(root, "tools", "droiddeckctl"), "w").write(f'''
import sys, json, os
a = sys.argv[1:]
log = open(os.path.join({d!r}, "calls.log"), "a"); log.write(" ".join(a) + "\\n"); log.close()
if "thermal" in a: print(json.dumps({{"batteryC": 31.0}}))
elif "pacing" in a: print({pacing!r})
elif "state" in a: print('{{"focusedApp": 3405340}}')
elif a[-2:] == ["get", "session"]: print(json.dumps({{"vkBridge": {vkb}}}))
elif "vulkaninfo" in " ".join(a): print("driverName = " + {vname!r})
elif "guest" in a and "/root/.malideck/driver" in a and "cat" in a: print({driver!r})
elif "guest" in a and "/root/.malideck/debug" in a and "cat" in a: print("on")
''')
        qr = os.path.join(root, "scripts", "quick", "qrestore")
        open(qr, "w").write(f"#!/bin/sh\necho qrestore $@ >> {d}/calls.log\n")
        os.chmod(qr, 0o755)
        qp = os.path.join(root, "scripts", "quick", "qpreflight")
        open(qp, "w").write("#!/bin/sh\nexit ${QPF_RC:-0}\n")
        os.chmod(qp, 0o755)
        return root

    def go(self, driver, vkb, extra_env=None, disabled=False):
        with tempfile.TemporaryDirectory() as d:
            root = self.stub(d, driver, vkb)
            env = dict(os.environ, MB_QUICK_ROOT=root, ANDROID_SERIAL="fake", MALI_FARM_STATE=os.path.join(d, "st"), PATH="/usr/bin:/bin", **(extra_env or {}))
            if disabled:
                os.makedirs(os.path.join(d, "st", "disabled"))
                open(os.path.join(d, "st", "disabled", "fake"), "w").close()
            r = subprocess.run(["bash", RUN, "fake", "--wait", "0", "--secs", "60", "--out", os.path.join(d, "out")], capture_output=True, text=True, env=env)
            p = os.path.join(d, "calls.log")
            return r, (open(p).read() if os.path.exists(p) else "")

    def test_bridge_refused_but_restored(self):
        r, calls = self.go("bridge", "True")
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        self.assertNotIn("avg fps", r.stdout)
        self.assertIn("REFUSED", r.stdout)
        self.assertNotIn("launch", calls)           # refused before the game starts
        self.assertIn("qrestore fake", calls)

    def test_disabled_device_is_never_touched(self):
        r, calls = self.go("ours", "False", disabled=True)
        self.assertEqual(r.returncode, 5, r.stdout)
        self.assertNotIn("qrestore", calls)
        self.assertNotIn("quit", calls)
        self.assertNotIn("guest", calls)

    def test_qpreflight_failure_stops_but_restores(self):
        r, calls = self.go("ours", "False", {"QPF_RC": "1"})
        self.assertEqual(r.returncode, 7, r.stdout)
        self.assertNotIn("launch", calls)
        self.assertNotIn("start steam", calls)
        self.assertNotIn("avg fps", r.stdout)

    def test_ours_reports_fps(self):
        r, calls = self.go("ours", "False")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("DRIVER-EVIDENCE kind=ours", r.stdout)
        self.assertIn("avg fps 100.0", r.stdout)
        self.assertIn("qrestore fake", calls)


if __name__ == "__main__":
    unittest.main()
