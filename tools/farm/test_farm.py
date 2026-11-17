#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Dry-run tests of queue, pending/resume, comparison and JSON logic (no phone, no build). Run: python3 tools/farm/test_farm.py"""
import json, os, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import farmlib as L  # noqa: E402
import gate  # noqa: E402

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
require_charging = true
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

    def health(self, lock, **h):
        with open(f"{self.d}/state/fake-health-{lock}.json", "w") as f:
            json.dump(h, f)

    def alerts(self):
        p = self.d + "/state/alerts.log"
        return open(p).read().strip().splitlines() if os.path.exists(p) else []

    def test_battery_gate_hysteresis(self):
        self.attach("dev-a")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        self.health("cmf", level=15, temp_c=30.0, thermal=0)
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "pending")
        self.assertIn("charging", self.farm("status")[1])
        self.assertIn("batt 15%", self.farm("status")[1])
        self.health("cmf", level=35, temp_c=30.0, thermal=0)      # above 20 but below 50: still gated
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "pending")
        self.assertEqual(len(self.alerts()), 1)                     # one line per state change, not per tick
        self.health("cmf", level=55, temp_c=30.0, thermal=0)
        self.farmd()
        run = json.loads(self.farm("results", jid)[1])["runs"]["v11"]
        self.assertEqual(run["verdict"], "pass")
        self.assertEqual((run["battery_pct_start"], run["battery_pct_end"], run["temp_c_start"]), (55, 55, 30.0))
        self.assertEqual(len(self.alerts()), 2)
        self.assertIn("charging->ok", self.alerts()[1])

    def test_thermal_gate(self):
        self.attach("dev-b")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v13")
        for h, pending in (({"temp_c": 43.0, "thermal": 0}, True), ({"temp_c": 40.0, "thermal": 0}, True),
                           ({"temp_c": 30.0, "thermal": 3}, True), ({"temp_c": 30.0, "thermal": 2}, True), ({"temp_c": 37.0, "thermal": 1}, False)):
            self.health("poco", level=80, **h)
            self.farmd()
            self.assertEqual(L.find_job(jid)["parts"]["v13"]["state"] == "pending", pending, h)
        self.assertEqual(len(self.alerts()), 2)                     # ok->cooling, cooling->ok

    def test_next_state_pure(self):
        th = dict(gate.DEFAULTS)
        ok = {"level": 80, "temp_c": 30.0, "thermal": 0}
        self.assertEqual(gate.next_state("ok", dict(ok, level=19), th)[0], "charging")
        self.assertEqual(gate.next_state("ok", dict(ok, level=20), th)[0], "ok")
        self.assertEqual(gate.next_state("charging", dict(ok, level=49), th)[0], "charging")
        self.assertEqual(gate.next_state("charging", dict(ok, level=50), th)[0], "ok")
        self.assertEqual(gate.next_state("charging", dict(ok, level=50, temp_c=44.0), th)[0], "cooling")
        self.assertEqual(gate.next_state("ok", dict(ok, level=None, temp_c=None, thermal=None), th)[0], "ok")
        self.assertEqual(gate.parse("  level: 74\n  temperature: 281\nCapacity level: 3\nThermal Status: 2\n"),
                         {"level": 74, "temp_c": 28.1, "thermal": 2, "powered": None})
        self.assertTrue(gate.parse("  AC powered: false\n  USB powered: true\n")["powered"])
        self.assertFalse(gate.parse("  AC powered: false\n  USB powered: false\n")["powered"])
        rc = dict(th, require_charging=True)
        self.assertEqual(gate.next_state("ok", dict(ok, powered=False), rc)[0], "on_battery")
        self.assertEqual(gate.next_state("on_battery", dict(ok, powered=True), rc)[0], "ok")
        self.assertEqual(gate.next_state("ok", dict(ok), th)[0], "ok")      # not required: power ignored

    def test_usb_only_never_wireless(self):
        calls = []
        L.adb_live = lambda: {"adb-SERIAL-B-x._adb-tls-connect._tcp": "device", "203.0.113.5:5555": "device"}
        L.sh = lambda cmd, timeout, **kw: calls.append(cmd) or (0, "")
        usb = {"serial": "SERIAL-B", "transport": "usb", "wireless": "203.0.113.5:5555"}
        self.assertIsNone(L.resolve(usb, connect=True))
        self.assertEqual(calls, [])                                 # no `adb connect`
        both = {"serial": "SERIAL-B", "wireless": "203.0.113.5:5555"}
        self.assertEqual(L.resolve(both), "adb-SERIAL-B-x._adb-tls-connect._tcp")
        L.adb_live = lambda: {"SERIAL-B": "device", "adb-SERIAL-B-x._adb-tls-connect._tcp": "device"}
        self.assertEqual(L.resolve(both), "SERIAL-B")               # USB preferred

    def test_two_wireless_routes_and_wireless_only(self):
        calls = []
        L.sh = lambda cmd, timeout, **kw: calls.append(cmd) or (0, "")
        L.adb_live = lambda: {"198.51.100.7:5555": "device"}
        cmf = {"serial": "S1", "wireless": ["203.0.113.9:5555", "198.51.100.7:5555"]}
        self.assertEqual(L.resolve(cmf), "198.51.100.7:5555")              # second route already listed
        L.adb_live = lambda: {"S1": "device", "203.0.113.1:5555": "device"}
        x4 = {"serial": "S1", "transport": "wireless", "address": "203.0.113.1:5555"}
        self.assertEqual(L.resolve(x4), "203.0.113.1:5555")                    # wireless-only ignores USB
        L.adb_live = lambda: {}
        self.assertIsNone(L.resolve(cmf, connect=True))
        self.assertEqual([c[-1] for c in calls if "connect" in c], ["203.0.113.9:5555", "198.51.100.7:5555"])

    def test_device_loss_returns_to_pending(self):
        import time
        t0 = time.time()
        rc, out = L.run_watched(["sleep", "60"], 100, lambda: False, grace=1, poll=0.5)
        self.assertEqual(rc, 125)
        self.assertLess(time.time() - t0, 10)
        rc, out = L.run_watched(["echo", "hi"], 100, lambda: True)
        self.assertEqual((rc, out.strip()), (0, "hi"))

    def health(self, lock, **kw):
        L.write_json(self.d + "/state/fake-health-" + lock + ".json", kw)

    def test_on_battery_pending_then_charger_runs(self):
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v13")
        self.attach("dev-b")
        self.health("poco", powered=False)
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v13"]["state"], "pending")     # on battery: nothing runs
        self.assertIn("on battery: paused", self.farm("status")[1])
        self.health("poco", powered=True)
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v13"]["state"], "done")

    def test_unplug_mid_job_back_to_pending_lock_released(self):
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v13", "--env", "FAKE_UNPLUG=1")
        self.attach("dev-b")
        self.farmd()
        p = L.find_job(jid)["parts"]["v13"]
        self.assertEqual((p["state"], p["attempts"]), ("pending", 0))               # clean stop, no retry counted
        lk = L.PhoneLock({"alias": "dev-b", "lock": "poco"}, True)
        self.assertTrue(lk.acquire())                                               # lock was released
        lk.release()
        self.assertIn("on battery: paused", self.farm("status")[1])

    def test_disabled_untouched_enable_runs(self):
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        self.attach("dev-a")
        self.assertEqual(self.farm("disable", "dev-a")[0], 0)
        self.assertTrue(os.path.exists(self.d + "/state/disabled/dev-a"))
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "pending")
        self.assertIn("disabled by owner", self.farm("status")[1])
        self.assertEqual(self.farm("enable", "dev-a")[1], "dev-a: enabled")
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "done")
        os.makedirs(self.d + "/state/disabled", exist_ok=True)                      # a plain touch (by lock name) works too
        open(self.d + "/state/disabled/cmf", "w").close()
        _, j2, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        self.farmd()
        self.assertEqual(L.find_job(j2)["parts"]["v11"]["state"], "pending")

    def test_store_route_idempotent(self):
        p = self.d + "/devices.toml"
        self.assertTrue(L.store_route("dev-b", "198.51.100.8:5555", p))
        self.assertFalse(L.store_route("dev-b", "198.51.100.8:5555", p))
        self.assertTrue(L.store_route("dev-b", "198.51.100.9:5555", p))
        _, devs = L.load_devices()
        self.assertEqual(L.wireless_routes(devs["dev-b"]), ["198.51.100.8:5555", "198.51.100.9:5555"])
        self.assertEqual(L.wireless_routes(devs["dev-a"]), [])

    def test_env_validation(self):
        self.assertEqual(self.farm("submit", "HEAD", "--env", "A=b;rm")[0] != 0, True)


if __name__ == "__main__":
    unittest.main()
