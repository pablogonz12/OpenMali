#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Dry-run tests of queue, pending/resume, comparison and JSON logic (no phone, no build). Run: python3 tools/farm/test_farm.py"""
import json, os, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import farmlib as L  # noqa: E402

TOML = """quick_root = "/nonexistent"
[[device]]
alias = "dev-a"
class = "v11"
serial = "SERIAL-A"
lock = "cmf"
baseline = "cmf"
[[device]]
alias = "dev-b"
class = "v13"
serial = "SERIAL-B"
lock = "poco"
baseline = "poco"
"""


class T(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        open(os.path.join(self.d, "devices.toml"), "w").write(TOML)
        self.env = dict(os.environ, MALI_FARM_STATE=self.d + "/state", MALI_FARM_DEVICES=self.d + "/devices.toml",
                        MALI_FARM_DOWN_AFTER="1")
        os.environ.update({k: self.env[k] for k in ("MALI_FARM_STATE", "MALI_FARM_DEVICES")})
        os.makedirs(self.d + "/state", exist_ok=True)

    def attach(self, *names):
        with open(self.d + "/state/fake-attached", "w") as f:
            f.write("\n".join(names))

    def farm(self, *a):
        r = subprocess.run([sys.executable, HERE + "/farm", *a], capture_output=True, text=True, env=self.env)
        return r.returncode, r.stdout.strip(), r.stderr

    def farmd(self):
        r = subprocess.run([sys.executable, HERE + "/farmd", "--fake", "--once"], capture_output=True, text=True, env=self.env, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def test_pending_resume_and_json(self):
        rc, jid, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11,v13", "--env", "FAKE_REGRESS=2 FAKE_FIX=1")
        self.assertEqual(rc, 0)
        self.attach("dev-a")                                    # only v11 present
        self.farmd()
        self.assertEqual(self.farm("wait", jid, "--timeout", "1")[0], 3)    # v13 still pending -> timeout
        j = L.find_job(jid)
        self.assertEqual(j["parts"]["v11"]["state"], "done")
        self.assertEqual(j["parts"]["v13"]["state"], "pending")
        self.assertEqual(L.job_verdict(j), "pending")
        self.attach("dev-a", "dev-b")                           # phone reappears -> picked up
        self.farmd()
        rc, out, _ = self.farm("wait", jid, "--timeout", "5")
        self.assertEqual((rc, out), (1, "fail"))                # 2 new failures
        res = json.loads(self.farm("results", jid)[1])
        run = res["runs"]["v13"]
        for k in ("job", "commit", "dirty", "so_sha256", "env", "suite", "device", "gpu", "kbase", "battery_temp_c",
                  "counts", "new_failures", "newly_passing", "verdict", "wall_s"):
            self.assertIn(k, run)
        self.assertEqual(len(run["new_failures"]), 2)
        self.assertEqual(len(run["newly_passing"]), 1)
        self.assertEqual(run["device"], "dev-b")
        self.assertEqual(len(run["so_sha256"]), 64)

    def test_clean_pass_and_smoke_fail(self):
        self.attach("dev-a", "dev-b")
        _, j1, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11")
        _, j2, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v13", "--env", "FAKE_FAIL=1")
        self.farmd()
        self.assertEqual(self.farm("wait", j1)[1], "pass")
        self.assertEqual(self.farm("wait", j2)[1], "fail")

    def test_lock_busy_keeps_pending(self):
        self.attach("dev-a")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        _, devs = L.load_devices()
        lk = L.PhoneLock(devs["dev-a"], fake=True)
        self.assertTrue(lk.acquire())                           # an "agent" holds the phone
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "pending")
        lk.release()
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "done")

    def test_down_alert(self):
        self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v13")
        self.attach()
        self.farmd()                                            # becomes 'missing'
        import time
        time.sleep(1.2)
        self.farmd()                                            # > DOWN_AFTER -> down + one alert line
        self.farmd()
        lines = open(self.d + "/state/alerts.log").read().strip().splitlines()
        self.assertEqual(len(lines), 1)
        self.assertIn("dev-b", lines[0])
        self.assertNotIn("SERIAL", lines[0])

    def test_compare(self):
        c, nf, npass = L.compare({"a": "Pass", "b": "Pass", "c": "Fail"}, {"a": "Pass", "b": "Crash", "c": "Pass"})
        self.assertEqual((nf, npass), ([["b", "Crash"]], ["c"]))
        c, nf, _ = L.compare({"a": "Pass"}, {}, ["a"])
        self.assertEqual(nf, [["a", "Missing"]])

    def test_env_validation(self):
        self.assertEqual(self.farm("submit", "HEAD", "--env", "A=b;rm")[0] != 0, True)


if __name__ == "__main__":
    unittest.main()
