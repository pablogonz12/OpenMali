#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Dry-run tests of queue, pending/resume, comparison and JSON logic (no phone, no build). Run: python3 tools/farm/test_farm.py"""
import hashlib, json, os, subprocess, sys, tempfile, time, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import farmlib as L
import sweeplib as S  # noqa: E402
import gate  # noqa: E402
import benchlib as BL  # noqa: E402

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


FAKE_ADB = r'''#!/usr/bin/env python3
# fake adb for the shell route: the "phone" is a directory ($FAKE_ADB_ROOT); deqp-vk / t_vk_* are simulated
import hashlib, os, re, shutil, sys
R = os.environ["FAKE_ADB_ROOT"]
a = sys.argv[1:]
if a[:1] == ["-s"]:
    a = a[2:]
P = lambda p: R + p
op = a[0]
if op == "push":
    os.makedirs(os.path.dirname(P(a[2])), exist_ok=True)
    if os.environ.get("FAKE_ADB_CORRUPT") == "1" and a[2].endswith(".so"):
        open(P(a[2]), "w").write("tampered")
    else:
        shutil.copy(a[1], P(a[2]))
elif op == "pull":
    if os.path.exists(P(a[1])):
        shutil.copy(P(a[1]), a[2])
    else:
        print("error: remote object does not exist"); sys.exit(1)
elif op == "shell":
    c = a[1]
    m = re.match(r"sha256sum (\S+)", c)
    if m:
        if os.path.exists(P(m.group(1))):
            print(hashlib.sha256(open(P(m.group(1)), "rb").read()).hexdigest(), m.group(1))
    elif c.startswith("ls "):
        print("\n".join(os.listdir(P(c[3:].strip()))))
    elif "dumpsys battery" in c:
        print("  AC powered: true\n  level: 80\n  temperature: 300\nThermal Status: 0")
    elif "deqp-vk" in c:
        cf = re.search(r"--deqp-caselist-file=(\S+)", c).group(1)
        qf = re.search(r"--deqp-log-filename=(\S+)", c).group(1)
        with open(P(qf), "w") as f:
            for case in open(P(cf)).read().split():
                f.write("#beginTestCaseResult %s\n<Result StatusCode=\"Pass\"/>\n#endTestCaseResult\n" % case)
        print("rc=0")
    elif "./t_vk_" in c:
        d = re.match(r"cd (\S+)", c).group(1)
        n = re.search(r"\./(t_vk_\w+)", c).group(1)
        open(P(d) + "/" + n + ".log", "w").write('{"pass":true}\n{"pass":true}\n')
        print("rc=0")
    else:
        for m in re.finditer(r"rm -(?:rf|f) ([^;&]+)", c):
            for p in m.group(1).split():
                shutil.rmtree(P(p), ignore_errors=True) if os.path.isdir(P(p)) else (os.path.exists(P(p)) and os.unlink(P(p)))
        for m in re.finditer(r"mkdir -p (\S+)", c):
            os.makedirs(P(m.group(1)), exist_ok=True)
'''


class ShellRoute(unittest.TestCase):
    """shellroute.py against a fake adb (the phone is a temp dir): per-run .so path, sha256 on the phone, lock guard, resume"""
    def setUp(self):
        self.d = tempfile.mkdtemp()
        os.makedirs(self.d + "/bin")
        open(self.d + "/bin/adb", "w").write(FAKE_ADB)
        os.chmod(self.d + "/bin/adb", 0o755)
        self.root = self.d + "/phone"
        for p in ("/data/local/tmp/g/lib/ld-linux-aarch64.so.1", "/data/local/tmp/g/deqp-vk"):
            os.makedirs(os.path.dirname(self.root + p), exist_ok=True)
            open(self.root + p, "w").write("x")
        self.tree = self.d + "/tree"
        for sub in ("a64-glibc", "a64-bionic"):
            os.makedirs(f"{self.tree}/vk/out/{sub}")
            open(f"{self.tree}/vk/out/{sub}/libvulkan_mali.so", "w").write("driver " + sub)
        os.makedirs(self.tree + "/vk/out/a64-bionic", exist_ok=True)
        for t in ("t_vk_draw", "t_vk_compute", "t_vk_meta"):
            open(f"{self.tree}/vk/out/a64-bionic/{t}", "w").write("x")
        os.makedirs(self.tree + "/vk/shader")
        open(self.tree + "/vk/shader/a.spv", "w").write("x")
        open(self.d + "/cases.txt", "w").write("\n".join(f"dEQP-VK.t.c{i}" for i in range(5)) + "\n")
        self.env = dict(os.environ, PATH=self.d + "/bin:" + os.environ["PATH"], FAKE_ADB_ROOT=self.root, ANDROID_SERIAL="X", QSAFE_HELD="shelltest",
                        MALI_FARM_STATE=self.d + "/state", MALI_FARM_DEVICES=self.d + "/none.toml")

    def run_sr(self, mode, sha_of, extra=(), env=None):
        sha = hashlib.sha256(open(f"{self.tree}/vk/out/{sha_of}/libvulkan_mali.so", "rb").read()).hexdigest()
        r = subprocess.run([sys.executable, "-I", HERE + "/shellroute.py", mode, "--lock", "shelltest", "--tree", self.tree, "--sha", sha, "--run", "r1", *extra],
                           capture_output=True, text=True, env=dict(self.env, **(env or {})), timeout=60)
        return r.returncode, r.stdout

    def test_daily_per_run_so_and_resume(self):
        out = self.d + "/o.tsv"
        rc, o = self.run_sr("daily", "a64-glibc", ["--list", self.d + "/cases.txt", "--out", out, "--chunk", "2"])
        self.assertEqual(rc, 0, o)
        self.assertEqual(open(out).read().count("\tPass\t\n"), 5)
        self.assertTrue(os.path.exists(self.root + "/data/local/tmp/g/icd.r1.so"))      # per-run ICD path, left for farmd's final check
        self.assertIn("SO_ON_PHONE /data/local/tmp/g/icd.r1.so", o)
        rc, o = self.run_sr("daily", "a64-glibc", ["--list", self.d + "/cases.txt", "--out", out])        # resume: nothing left to run
        self.assertEqual(rc, 0)
        self.assertEqual(len(open(out).read().splitlines()), 5)

    def test_daily_foreign_so_is_refused(self):
        rc, o = self.run_sr("daily", "a64-glibc", ["--list", self.d + "/cases.txt", "--out", self.d + "/o2.tsv"], {"FAKE_ADB_CORRUPT": "1"})
        self.assertEqual(rc, 76)
        self.assertFalse(os.path.exists(self.d + "/o2.tsv") and open(self.d + "/o2.tsv").read())

    def test_cpu_busy_pct_ignores_idle_phone_with_high_loadavg(self):
        sys.path.insert(0, HERE)
        import shellroute
        idle = "cpu  1000 0 500 8000 100 0 0 0\ncpu  1000 0 500 8800 100 0 0 0"      # 800 idle ticks of 800: 0 % busy (poco-x4 at load 12)
        busy = "cpu  1000 0 500 8000 100 0 0 0\ncpu  1700 0 600 8100 100 0 0 0"
        self.assertEqual(shellroute.cpu_busy_pct(idle), 0.0)
        self.assertAlmostEqual(shellroute.cpu_busy_pct(busy), 88.89, 1)
        self.assertIsNone(shellroute.cpu_busy_pct("garbage"))

    def test_refuses_without_the_phone_lock(self):
        import fcntl
        fd = open("/tmp/qsafe-shelltest.lock", "a")
        fcntl.flock(fd, fcntl.LOCK_EX)
        try:
            rc, _ = self.run_sr("daily", "a64-glibc", ["--list", self.d + "/cases.txt", "--out", self.d + "/o3.tsv"], {"QSAFE_HELD": ""})
            self.assertEqual(rc, 73)
        finally:
            fd.close()

    def test_smoke(self):
        rc, o = self.run_sr("smoke", "a64-bionic")
        self.assertEqual(rc, 0, o)
        self.assertIn('"test":"summary","ok":true', o)
        self.assertIn('"test":"t_vk_draw","rc":0,"fail_lines":0,"pass_lines":2', o)
        rc, o = self.run_sr("smoke", "a64-bionic", env={"FAKE_ADB_CORRUPT": "1"})
        self.assertEqual(rc, 77)


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
        self.assertEqual(L.wireless_routes(devs["dev-b"]), ["198.51.100.9:5555"])    # follows the CURRENT ip: replaced, not appended
        self.assertEqual(L.wireless_routes(devs["dev-a"]), [])
        open(p, "a").write('\n[[device]]\nalias = "dev-c"\nclass = "v10"\nserial = "SERIAL-C"\nlock = "c"\nbaseline = "c"\naddress = "198.51.100.20:5555"   # keep\nwireless = ["203.0.113.4:7777", "198.51.100.21:5555"]\n')
        self.assertTrue(L.store_route("dev-c", "198.51.100.22:5555", p))
        _, devs = L.load_devices()
        self.assertEqual(L.wireless_routes(devs["dev-c"]), ["203.0.113.4:7777", "198.51.100.22:5555"])
        self.assertIn("# keep", open(p).read())
        self.assertFalse(L.store_route("dev-c", "198.51.100.22:5555", p))

    def test_unreachable_stored_route_is_a_soft_miss(self):
        calls = []
        L._ROUTE_MISS.clear()
        L.sh = lambda cmd, timeout, **kw: calls.append(cmd) or (0, "failed to connect to 203.0.113.50:5555")
        L.adb_live = lambda: {}
        dev = {"serial": "S9", "wireless": ["203.0.113.50:5555"]}
        self.assertIsNone(L.resolve(dev, connect=True))                  # no exception
        self.assertIsNone(L.resolve(dev, connect=True))
        self.assertEqual(len(calls), 1)                                  # second attempt skipped (miss cached)
        L._ROUTE_MISS.clear()

    def test_invalid_run_is_requeued_not_pass_or_fail(self):
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11", "--env", "FAKE_SO_SWAP=1")
        self.attach("dev-a")
        self.farmd()
        p = L.find_job(jid)["parts"]["v11"]
        self.assertEqual((p["state"], p["attempts"]), ("pending", 1))
        self.assertEqual(L.job_verdict(L.find_job(jid)), "pending")
        self.assertTrue(os.path.exists(self.d + f"/state/runs/{jid}-v11.invalid-1.json"))
        self.assertFalse(os.path.exists(self.d + f"/state/runs/{jid}-v11.json"))
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["verdict"], "pass")

    def test_ns_to_fail_reported(self):
        base = {"a": "Pass", "b": "NotSupported", "c": "NotSupported", "d": "Fail"}
        cur = {"a": "Pass", "b": "Fail", "c": "NotSupported", "d": "Fail"}
        self.assertEqual(L.ns_to_fail(base, cur), [["b", "Fail"]])
        self.assertEqual(L.compare(base, cur)[1], [])                    # new_failures alone would miss it
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11", "--env", "FAKE_NS2F=3")
        self.attach("dev-a")
        self.farmd()
        run = json.loads(self.farm("results", jid)[1])["runs"]["v11"]
        self.assertEqual(len(run["ns_to_fail"]), 3)
        self.assertIn("3 NotSupported->Fail", run["notes"])

    def test_identified_by_serial_on_any_transport(self):
        L._ROUTE_MISS.clear(); L._SERIALNO.clear()
        L.adb_live = lambda: {"203.0.113.77:5555": "device", "203.0.113.78:5555": "device"}
        L.sh = lambda cmd, timeout, **kw: (0, "OTHER\n" if "203.0.113.78:5555" in cmd else "S9\n")
        dev = {"serial": "S9", "wireless": ["203.0.113.99:5555"]}        # stored route is stale: the phone has another address now
        self.assertEqual(L.resolve(dev), "203.0.113.77:5555")
        dev2 = {"serial": "NOPE", "wireless": ["203.0.113.99:5555"]}     # nobody answers with that serial
        L._SERIALNO.clear()
        self.assertIsNone(L.resolve(dev2))
        L._SERIALNO.clear()

    def test_config_reloaded_without_restart(self):
        import importlib.machinery, importlib.util
        ld = importlib.machinery.SourceFileLoader("farmd_mod", HERE + "/farmd")
        m = importlib.util.module_from_spec(importlib.util.spec_from_loader("farmd_mod", ld))
        ld.exec_module(m)
        f = m.Farm(True, True, 1)
        self.assertEqual(L.wireless_routes(f.devs["dev-b"]), [])
        self.assertEqual(f.phone_env(f.devs["dev-a"])["QSAFE_HELD"], "cmf")
        p = self.d + "/devices.toml"
        txt = open(p).read()
        open(p, "w").write(txt.replace('baseline = "poco"\n', 'baseline = "poco"\nwireless = ["198.51.100.5:5555"]\n') + "\n")
        os.utime(p, (f.cfg_mtime + 5, f.cfg_mtime + 5))
        f.reload_devices()
        self.assertEqual(L.wireless_routes(f.devs["dev-b"]), ["198.51.100.5:5555"])

    def test_verdict_uses_main_baseline_too(self):
        # branch baseline is stale for 2 cases main passes; they fail in the run -> must be regressions, not silently "pass"
        self.attach("dev-a")
        rc, jid, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11", "--env", "FAKE_STALE_BRANCH=2")
        self.farmd()
        run = json.loads(self.farm("results", jid)[1])["runs"]["v11"]
        self.assertEqual(run["verdict"], "fail")
        self.assertEqual(len(run["new_failures"]), 2)
        self.assertIn("baseline branch ", run["notes"])
        self.assertIn(" + main ", run["notes"])
        self.assertIn("2 cases Pass only on main", run["notes"])

    def test_bless(self):
        repo = os.path.dirname(os.path.dirname(HERE))
        self.attach("dev-a", "dev-b")
        _, bad, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11,v13")      # not main's tip -> refused (unless HEAD == main)
        _, jid, _ = self.farm("submit", "main", "--suite", "daily", "--need", "v11,v13")
        _, smoke, _ = self.farm("submit", "main", "--suite", "smoke", "--need", "v11")
        for _ in range(3):
            self.farmd()
        co = self.d + "/co"
        os.makedirs(co + "/vk/deqp")
        _, devs = L.load_devices()
        for al in ("dev-a", "dev-b"):                                    # what farmd keeps of each daily run
            src = open(os.path.join(repo, "vk/deqp/ours-daily-sample-%s.tsv" % devs[al]["baseline"])).read()
            open(os.path.join(self.d, "state/logs", f"{jid}-{al}-daily.tsv"), "w").write(src.replace("\tPass\t", "\tFail\t", 1))
        rc, _, err = self.farm("bless", smoke, "--checkout", co)
        self.assertEqual(rc, 2)
        self.assertIn("not the full daily", err)
        rc, _, err = self.farm("bless", jid, "--checkout", co)
        self.assertEqual(rc, 0, err)
        for b in ("cmf", "poco"):
            p = f"{co}/vk/deqp/ours-daily-sample-{b}.tsv"
            if os.path.exists(p):
                self.assertEqual(len(open(p).read().splitlines()), 1381)
        j = L.find_job(jid)
        j["parts"]["v13"]["verdict"] = "fail"                             # a failing part -> refused
        L.write_json(L.job_path(jid), j)
        rc, _, err = self.farm("bless", jid, "--checkout", co)
        self.assertEqual(rc, 2)
        self.assertIn("verdict is", err)

    def mkjob(self, ref, commit, suite, parts, tsvs=None):
        j = L.new_job(ref, commit, False, suite, list(parts), "", "r")
        for cls, (alias, verdict) in parts.items():
            j["parts"][cls].update(state="done", verdict=verdict, device=alias)
            L.write_json(L.run_path(j["id"], cls), {"verdict": verdict, "device": alias, "counts": {"Pass": 2, "Fail": 1, "NotSupported": 1}})
            if tsvs:
                open(os.path.join(L.state_dir(), "logs", f"{j['id']}-{alias}-daily.tsv"), "w").write("".join(f"{c}\t{s}\t\n" for c, s in tsvs.items()))
        L.write_json(L.job_path(j["id"]), j)
        return j

    def test_cheap_commands(self):
        m = self.mkjob("main", "a" * 40, "daily", {"v11": ("dev-a", "pass")}, {"dEQP-VK.x.one": "Pass", "dEQP-VK.x.two": "Pass", "dEQP-VK.x.three": "Fail"})
        b = self.mkjob("feat/x", "b" * 40, "daily", {"v11": ("dev-a", "fail")}, {"dEQP-VK.x.one": "Pass", "dEQP-VK.x.two": "NotSupported", "dEQP-VK.x.three": "Pass"})
        rc, out, _ = self.farm("diff", m["id"], b["id"], "--class", "v11")
        self.assertEqual(rc, 0)
        self.assertIn("Pass->NotSupported", out)
        self.assertIn("Fail->Pass", out)
        self.assertNotIn("x.one", out)
        self.assertLess(len(out.splitlines()), 40)
        rc, out, _ = self.farm("digest")
        self.assertIn("queue: 2 done", out)
        self.assertIn("vs main aaaaaaa: 1 regressed (1 ->NS): x.two", out)
        self.assertLess(len(out.splitlines()), 41)
        rc, out, _ = self.farm("case", "x.two")
        rows = out.splitlines()
        self.assertEqual(len(rows), 2)
        self.assertTrue(rows[0].split()[1] == "main" and rows[1].endswith("NotSupported"))     # newest last
        self.assertIn("no stored daily", self.farm("case", "nope.nothing")[1])

    def test_cancel(self):
        self.attach("dev-a")
        _, j1, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        _, j2, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        out = self.farm("cancel", j1)[1]
        self.assertIn("v11 cancelled", out)
        self.assertEqual(L.job_verdict(L.find_job(j1)), "cancelled")
        self.farmd()
        self.assertEqual(L.find_job(j1)["parts"]["v11"]["state"], "cancelled")      # never run
        self.assertEqual(L.find_job(j2)["parts"]["v11"]["state"], "done")
        self.assertIn("v11 pass (untouched)", self.farm("cancel", j2)[1])           # finished parts are left alone
        # a running part needs --running; the flag stops it cleanly, the lock is released, the part ends cancelled
        _, j3, _ = self.farm("submit", "HEAD", "--suite", "cases", "--case", "dEQP-VK.a.b", "--need", "v11", "--env", "FAKE_SLOW=1")
        j = L.find_job(j3)
        j["parts"]["v11"]["state"] = "running"
        L.write_json(L.job_path(j3), j)
        self.assertIn("RUNNING (use --running)", self.farm("cancel", j3)[1])
        j["parts"]["v11"]["state"] = "pending"
        L.write_json(L.job_path(j3), j)
        os.makedirs(self.d + "/state/cancel", exist_ok=True)
        open(self.d + "/state/cancel/" + j3, "w").close()
        self.farmd()
        self.assertEqual(L.find_job(j3)["parts"]["v11"]["state"], "cancelled")
        lk = L.PhoneLock({"alias": "dev-a", "lock": "cmf"}, True)
        self.assertTrue(lk.acquire())
        lk.release()

    def test_cases_suite(self):
        self.attach("dev-a")
        rc, jid, err = self.farm("submit", "HEAD", "--suite", "cases", "--case", "dEQP-VK.a.b", "--case", "dEQP-VK.a.c", "--repeat", "3", "--need", "v11")
        self.assertEqual(rc, 0, err)
        self.assertNotEqual(self.farm("submit", "HEAD", "--suite", "cases")[0], 0)      # needs a case
        self.farmd()
        run = json.loads(self.farm("results", jid, "--json")[1])["runs"]["v11"]
        self.assertEqual(run["case_counts"]["dEQP-VK.a.b"], {"Pass": 3})
        self.assertEqual(run["verdict"], "pass")
        txt = self.farm("results", jid)[1]
        self.assertIn("3 Pass of 3 (100% pass)", txt)
        _, j2, _ = self.farm("submit", "HEAD", "--suite", "cases", "--case", "dEQP-VK.a.b", "--repeat", "2", "--need", "v11", "--env", "FAKE_CASE_NS=1")
        self.farmd()
        self.assertIn("2 NotSupported of 2 (0% pass)", self.farm("results", j2)[1])

    # ---- bench suite, auto triggers, priorities
    def test_bench_pure_trigger_and_idle_logic(self):
        self.assertTrue(BL.touches_driver(["README.md", "vk/src/mali_cmd.c"]))
        self.assertTrue(BL.touches_driver(["libkbase/src/ctx.c"]))
        self.assertFalse(BL.touches_driver(["tools/farm/farmd", "vk/tests/t_x.c", "vk/deqp/x.tsv", "bench/RESULTS.md"]))
        self.assertEqual(BL.plan_trigger(None, "b", [], ["v11"]), ("b", []))                       # first sight: record only, no storm
        self.assertEqual(BL.plan_trigger("a", "a", ["vk/src/x.c"], ["v11"]), ("a", []))
        self.assertEqual(BL.plan_trigger("a", "b", ["docs/x.md"], ["v11", "v13"]), ("b", []))     # moved but irrelevant
        self.assertEqual(BL.plan_trigger("a", "b", ["vk/src/x.c"], ["v13", "v11"]), ("b", ["v11", "v13"]))
        devs = {"cmf": {"class": "v11"}, "poco-x8": {"class": "v13"}, "poco-x4": {"class": "v10", "route": "shell"}, "optout": {"class": "v11", "bench": False}}
        st = {a: {"status": "up"} for a in devs}
        st["poco-x8"]["status"] = "absent"
        self.assertEqual(BL.available(devs, st, set()), ["cmf"])                                    # shell route, opt-out and absent excluded
        self.assertEqual(BL.available(devs, st, {"cmf"}), [])                                        # owner-disabled
        rows = []
        idle = {"cmf": 0}
        args = dict(devs=devs, dstate=st, rows=rows, jobs=[], tip="T", disabled=set())
        self.assertEqual(BL.idle_candidates(899, idle, **args), [])                                  # not idle long enough
        self.assertEqual(BL.idle_candidates(901, idle, **args), ["cmf"])
        self.assertEqual(BL.idle_candidates(901, idle, running={"cmf"}, **args), [])
        self.assertEqual(BL.idle_candidates(901, idle, **dict(args, dstate={"cmf": {"status": "cooling"}})), [])   # health gate not ok
        self.assertEqual(BL.idle_candidates(901, idle, **dict(args, rows=[{"phone": "cmf", "commit_full": "T"}])), [])  # already has a row for main
        self.assertEqual(len(BL.idle_candidates(901, idle, **dict(args, rows=[{"phone": "cmf", "commit_full": "OLD"}]))), 1)
        job = {"suite": "bench", "commit": "T", "auto": True, "parts": {"v11": {"state": "pending"}}}
        self.assertEqual(BL.idle_candidates(901, idle, **dict(args, jobs=[job])), [])                # already queued
        job["parts"]["v11"]["state"] = "cancelled"
        self.assertEqual(BL.idle_candidates(901, idle, **dict(args, jobs=[job])), ["cmf"])
        both = {"cmf": 100, "optout2": 0}
        devs2 = {"cmf": {"class": "v11"}, "b2": {"class": "v11"}}
        st2 = {"cmf": {"status": "up"}, "b2": {"status": "up"}}
        self.assertEqual(BL.idle_candidates(2000, {"cmf": 500, "b2": 100}, devs2, st2, [], [], "T", set()), ["b2", "cmf"])   # longest idle = oldest missing first

    def test_bench_priority_order(self):
        jobs = [{"id": "auto1", "auto": True, "seq": 1}, {"id": "ex2", "seq": 2}, {"id": "ex3", "seq": 3}, {"id": "auto0", "auto": True, "seq": 0}]
        self.assertEqual([j["id"] for j in BL.order_jobs(jobs)], ["ex2", "ex3", "auto0", "auto1"])   # explicit > auto, each FIFO
        self.assertTrue(BL.pin_ok({}, "cmf") and BL.pin_ok({"pin": "cmf"}, "cmf") and not BL.pin_ok({"pin": "cmf"}, "poco-x8"))

    def test_bench_digest_lines(self):
        def row(ph, c, drv, var, cpu, gpu, ref="main"):
            return {"phone": ph, "commit_full": c, "ref": ref, "date": "2026-11-17", "driver": drv, "variant": var, "offscreen": {"cpu_ms": cpu, "gpu_ms": gpu}}
        rows = [row("cmf", "a" * 40, "ours", "triangle-3k", 9.0, 0.0), row("cmf", "b" * 40, "ours", "triangle-3k", 1.91, 0.0), row("cmf", "b" * 40, "bridge", "triangle-3k", 3.2, 0.0),
                row("cmf", "b" * 40, "ours", "cube-720p", 0.4, 12.34), row("cmf", "b" * 40, "bridge", "cube-720p", 0.5, 11.0)]
        out = BL.digest_lines(rows)
        self.assertEqual(len(out), 1)
        self.assertIn("bbbbbbb", out[0])                                                              # newest commit only
        self.assertIn("tri CPU ours 1.91 / bridge 3.20 ms", out[0])
        self.assertIn("cube GPU ours 12.34 / bridge 11.00 ms", out[0])

    def nosweep(self):
        os.environ.pop("MALI_FARM_SWEEP_LIST", None)          # an earlier sweep test leaves it in os.environ: a sweep would loop in these passes
        self.env.pop("MALI_FARM_SWEEP_LIST", None)

    def test_bench_suite_idle_trigger_runs_once_and_digest(self):
        self.nosweep()
        self.attach("dev-a")
        self.env.update(MALI_FARM_AUTO_BENCH="1", MALI_FARM_BENCH_IDLE="0")
        self.farmd()
        jobs = [j for j in L.list_jobs() if j["suite"] == "bench"]
        self.assertEqual(len(jobs), 1)
        self.assertTrue(jobs[0].get("auto"))
        self.assertEqual(jobs[0]["pin"], "dev-a")
        self.assertEqual(jobs[0]["parts"]["v11"]["state"], "done")
        self.assertEqual(jobs[0]["parts"]["v11"]["verdict"], "pass")
        rows = BL.load_rows(self.d + "/state")
        self.assertEqual(len(rows), 4)
        self.assertTrue(all(r["phone"] == "dev-a" and r["ref"] == "main" and len(r["commit_full"]) == 40 for r in rows))
        self.assertIn("bench dev-a", self.farm("digest")[1])
        self.assertIn("tri CPU ours 1.90 / bridge 2.47 ms", self.farm("digest")[1])
        self.farmd()                                                                                   # a row for main exists: nothing more is queued
        self.assertEqual(len([j for j in L.list_jobs() if j["suite"] == "bench"]), 1)

    def test_bench_suite_explicit_submit(self):
        self.nosweep()
        self.attach("dev-a")
        rc, jid, err = self.farm("submit", "HEAD", "--suite", "bench", "--need", "v11")
        self.assertEqual(rc, 0, err)
        self.farmd()
        self.assertEqual(self.farm("wait", jid)[1], "pass")
        self.assertFalse(L.find_job(jid).get("auto"))
        rows = BL.load_rows(self.d + "/state")
        self.assertEqual({r["job"] for r in rows}, {jid})
        self.assertEqual(L.find_job(jid)["build"], "ok")

    def test_bench_main_commit_touching_driver_queues_every_available_phone(self):
        self.nosweep()
        self.attach("dev-a", "dev-b")
        root = subprocess.run(["git", "-C", HERE, "rev-list", "--max-parents=0", "main"], capture_output=True, text=True).stdout.split()[-1]
        os.makedirs(self.d + "/state/bench", exist_ok=True)
        L.write_json(self.d + "/state/bench/auto.json", {"last_main": root})                          # main moved a long way since: touches vk/src
        self.env.update(MALI_FARM_AUTO_BENCH="1", MALI_FARM_BENCH_IDLE="999999")
        self.health("poco", level=80, temp_c=30.0, thermal=0, powered=True)
        self.farmd()
        jobs = [j for j in L.list_jobs() if j["suite"] == "bench"]
        self.assertEqual(len(jobs), 1)
        self.assertEqual(sorted(jobs[0]["parts"]), ["v11", "v13"])
        self.assertTrue(jobs[0]["auto"] and "pin" not in jobs[0])
        self.assertEqual(jobs[0]["commit"], subprocess.run(["git", "-C", HERE, "rev-parse", "main"], capture_output=True, text=True).stdout.strip())
        self.assertEqual({p["state"] for p in jobs[0]["parts"].values()}, {"done"})
        self.farmd()                                                                                   # same main: no second job
        self.assertEqual(len([j for j in L.list_jobs() if j["suite"] == "bench"]), 1)

    def test_explicit_job_runs_before_queued_auto_bench(self):
        self.nosweep()
        self.attach("dev-a")
        auto = L.new_job("main", "0" * 40, False, "bench", ["v11"], "", "t", auto=True)               # queued first (older), but auto
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        self.farmd()                                                                                   # one phone, one job per pass
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "done")
        self.assertEqual(L.find_job(auto["id"])["parts"]["v11"]["state"], "pending")
        self.farmd()
        self.assertEqual(L.find_job(auto["id"])["parts"]["v11"]["state"], "done")

    def test_auto_bench_beats_sweep(self):
        self.sweep_env(["dEQP-VK.api.a.one", "dEQP-VK.api.a.two", "dEQP-VK.api.a.three"], 2)
        self.attach("dev-a")
        auto = L.new_job("main", "0" * 40, False, "bench", ["v11"], "", "t", auto=True)
        self.farmd()
        self.assertEqual(L.find_job(auto["id"])["parts"]["v11"]["state"], "done")
        self.assertEqual(S.load("v11")["pos"], 0)                                                      # the sweep did not run over it
        self.farmd()
        self.assertEqual(S.load("v11")["pos"], 3)                                                      # queue empty: sweep resumes

    # ---- idle sweep (fake phones; list = MALI_FARM_SWEEP_LIST, 3 cases per chunk)
    def sweep_env(self, cases, chunks, **extra):
        lst = self.d + "/mustpass.txt"
        open(lst, "w").write("\n".join(cases) + "\n")
        self.env.update(MALI_FARM_SWEEP_LIST=lst, MALI_FARM_SWEEP_CHUNK="3", MALI_FARM_SWEEP_MAX_CHUNKS=str(chunks), **extra)
        os.environ["MALI_FARM_SWEEP_LIST"] = lst

    def test_sweep_walk_resume_crash_skip(self):
        names = ["dEQP-VK.api.a.one", "dEQP-VK.api.a.CRASHX", "dEQP-VK.api.b.three", "dEQP-VK.api.b.HANGX", "dEQP-VK.pipe.c.NSX",
                 "dEQP-VK.pipe.c.FAILX", "dEQP-VK.pipe.c.many_things"]
        self.sweep_env(names, 1)
        self.attach("dev-a")
        self.farmd()
        self.assertEqual(S.load("v11")["pos"], 3)                              # one chunk, then the session ends
        self.farmd()                                                           # resumes where it stopped (no restart)
        st = S.load("v11")
        self.assertEqual(st["pos"], 6)
        self.assertEqual(st["chunks"], 2)
        self.env["MALI_FARM_SWEEP_MAX_CHUNKS"] = "5"
        self.farmd()                                                           # crash/hang/fail cases never stalled it: the pass completed
        passes = S.load("v11")["passes"]
        self.assertTrue(passes and passes[-1]["pos"] == 7)
        self.assertEqual(passes[-1]["counts"], {"P": 2, "C": 1, "T": 1, "N": 1, "F": 1, "K": 1})
        # per-chunk records carry the commit, compact status strings
        recs = [json.loads(l) for l in open(self.d + "/state/sweep/v11-chunks.jsonl")]
        self.assertTrue(all(r["commit"] and r["z"] for r in recs))
        self.assertEqual(S.load("v11")["pos"] < 7 and S.load("v11")["commit"] is not None, True)
        out = self.farm("sweep", "status")[1]
        self.assertIn("v11:", out)
        self.assertLess(len(out.splitlines()), 15)
        fl = self.farm("sweep", "failures", "--class", "v11", "--group", "api.b")[1]
        self.assertNotIn("api.a", fl)

    def test_sweep_failures_listing(self):
        self.sweep_env(["dEQP-VK.api.a.CRASHX", "dEQP-VK.api.a.ok", "dEQP-VK.api.b.FAILX"], 1)
        self.attach("dev-a")
        self.farmd()
        out = self.farm("sweep", "failures", "--class", "v11")[1]
        self.assertIn("Crash    dEQP-VK.api.a.CRASHX", out)
        self.assertIn("Fail     dEQP-VK.api.b.FAILX", out)
        self.assertNotIn("a.ok", out)

    def test_sweep_pauses_on_health_gate_and_never_runs_over_a_queued_job(self):
        self.sweep_env(["dEQP-VK.api.a.one", "dEQP-VK.api.a.two", "dEQP-VK.api.a.three"], 2)
        self.attach("dev-a")
        L.write_json(self.d + "/state/fake-health-cmf.json", {"temp_c": 45.0})              # over the 43 C stop: cooling
        self.farmd()
        self.assertEqual(S.load("v11")["pos"], 0)
        self.assertFalse(os.path.exists(self.d + "/state/sweep/v11-chunks.jsonl"))
        os.unlink(self.d + "/state/fake-health-cmf.json")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")        # a queued real job: it runs, the sweep does not start first
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "done")
        self.assertEqual(S.load("v11")["pos"], 0)
        self.farmd()                                                                       # queue empty -> now the phone sweeps
        self.assertEqual(S.load("v11")["pos"], 3)

    def test_sweep_yields_to_a_submitted_job_mid_chunk(self):
        self.sweep_env([f"dEQP-VK.api.a.c{i}" for i in range(9)], 50, FAKE_SWEEP_SLOW="20")
        self.attach("dev-a")
        p = subprocess.Popen([sys.executable, HERE + "/farmd", "--fake", "--poll", "0.3"], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            time.sleep(3)                                                                  # sweeping: a slow chunk is in flight
            _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
            t0 = time.time()
            self.assertEqual(self.farm("wait", jid, "--timeout", "30")[0], 0)
            self.assertLess(time.time() - t0, 20)                                          # did not wait for the 20 s chunk
            self.assertEqual(L.find_job(jid)["parts"]["v11"]["verdict"], "pass")
            self.assertEqual(S.load("v11")["pos"], 0)                                      # preempted chunk recorded nothing
        finally:
            p.terminate()
            p.wait(10)

    def test_util_and_prebuild_cache(self):
        self.attach("dev-a")
        _, j1, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        _, j2, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11")
        self.farmd()
        self.farmd()
        self.assertTrue(L.find_job(j1).get("so_sha256") == L.find_job(j2).get("so_sha256"))      # same commit: one build
        out = self.farm("util", "--since", "1d")[1]
        self.assertLess(len(out.splitlines()), 15)
        self.assertIn("dev-a", out)
        self.assertIn("busy", out)

    def test_smoke_log_and_only(self):
        self.attach("dev-a")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11", "--only", "triangle,buf")
        self.assertEqual(L.find_job(jid)["only"], "triangle,buf")
        self.assertNotEqual(self.farm("submit", "HEAD", "--suite", "daily", "--only", "x")[0], 0)
        self.farmd()
        rc, out, _ = self.farm("log", jid, "--class", "v11")
        self.assertEqual(rc, 0)
        self.assertIn("only=triangle,buf", out)
        self.assertEqual(len(self.farm("log", jid, "--tail", "5")[1].splitlines()), 6)
        self.assertIn("fake line 250 only", self.farm("log", jid, "--grep", "line 250 only", "--tail", "3")[1])

    def test_build_cache_covers_t_binaries(self):
        """a daily's cached .so must come with its t_* binaries: a smoke job of the same commit finds them (or rebuilds), never 0/0 tests"""
        import importlib.machinery, importlib.util, threading, shutil
        ld = importlib.machinery.SourceFileLoader("farmd_mod", HERE + "/farmd")
        spec = importlib.util.spec_from_loader("farmd_mod", ld)
        fd = importlib.util.module_from_spec(spec)
        ld.exec_module(fd)
        f = object.__new__(fd.Farm)
        f.fake, f.devs, f.build_locks, f.bl, f.jl = False, {}, {}, threading.Lock(), threading.Lock()
        f.repo = f.main_tree = self.d + "/repo"
        f.missing_deps = lambda suite: ""
        f.borrow_deqp = lambda tree: None
        os.makedirs(self.d + "/state/artifacts", exist_ok=True)
        os.makedirs(self.d + "/state/work", exist_ok=True)
        os.makedirs(self.d + "/state/logs", exist_ok=True)
        builds = []

        def fake_sh(cmd, timeout=0, env=None, **kw):             # stands in for vk/build.sh: writes the .so and t_* next to it
            tree = os.path.dirname(os.path.dirname(cmd[-1]))
            out = os.path.join(tree, "vk", "out", "a64-glibc")
            os.makedirs(out, exist_ok=True)
            open(out + "/libvulkan_mali.so", "w").write("so-built")
            for t in ("t_vk_draw", "t_vk_compute"):
                open(out + "/" + t, "w").write("bin")
                os.chmod(out + "/" + t, 0o755)
            builds.append(tree)
            return 0, ""
        orig_sh, fd.L.sh = fd.L.sh, fake_sh
        try:
            _, j1, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11")
            _, j2, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
            for j in (j1, j2):
                os.makedirs(f.tree(j) + "/vk", exist_ok=True)       # worktree already "created"
                open(f.tree(j) + "/vk/build.sh", "w").write("")
            sha, _ = f.build(L.find_job(j1))
            self.assertEqual(len(builds), 1)
            sha2, _ = f.build(L.find_job(j2))
            self.assertEqual(sha, sha2)
            self.assertEqual(len(builds), 1, "cache hit expected")
            out2 = f.tree(j2) + "/vk/out/a64-glibc"
            self.assertTrue(os.access(out2 + "/t_vk_draw", os.X_OK) and os.access(out2 + "/t_vk_compute", os.X_OK))
            self.assertEqual(L.find_job(j2).get("build_cached"), j1)
            # archive from before the fix (no t_* stored): the smoke job must rebuild, not run with 0 tests
            shutil.rmtree(f.tests_dir(sha))
            _, j3, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
            os.makedirs(f.tree(j3) + "/vk", exist_ok=True)
            open(f.tree(j3) + "/vk/build.sh", "w").write("")
            f.build(L.find_job(j3))
            self.assertEqual(len(builds), 2, "missing t_* archive must trigger a rebuild")
            self.assertTrue(os.access(f.tree(j3) + "/vk/out/a64-glibc/t_vk_draw", os.X_OK))
        finally:
            fd.L.sh = orig_sh

    def test_env_validation(self):
        self.assertEqual(self.farm("submit", "HEAD", "--env", "A=b;rm")[0] != 0, True)


class SweepBackoffUnit(unittest.TestCase):
    def test_backoff_1_to_30_min_and_alert_after_5(self):
        import importlib.machinery, importlib.util, types
        ld = importlib.machinery.SourceFileLoader("farmd_mod", os.path.join(os.path.dirname(os.path.abspath(__file__)), "farmd"))
        m = importlib.util.module_from_spec(importlib.util.spec_from_loader("farmd_mod", ld)); ld.exec_module(m)
        alerts = []
        m.alert = alerts.append; m.log = lambda *_: None
        f = types.SimpleNamespace(sweep_bad={}, cool={})
        waits = [m.Farm.sweep_backoff(f, "x", rc, False) for rc in (75, 125, 75, 125, 75, 75, 75, 75)]
        self.assertEqual(waits, [60, 120, 240, 480, 960, 1800, 1800, 1800])
        self.assertEqual(len(alerts), 1); self.assertIn("SWEEP-CHURN", alerts[0])
        self.assertEqual(m.Farm.sweep_backoff(f, "x", 0, False), 0)                 # healthy chunk resets
        self.assertEqual(m.Farm.sweep_backoff(f, "x", 0, True), 60)                 # rc 0 but session died fast, no cases


class StaleLockUnit(unittest.TestCase):
    def test_adb_server_holding_lock_is_recovered_after_10min(self):
        import farmlib as FL, tempfile, subprocess, fcntl
        d = tempfile.mkdtemp()
        path = d + "/qsafe-x.lock"
        dev = {"alias": "x", "lock": "x"}
        lk = FL.PhoneLock(dev); lk.path = path
        got = []
        FL.PhoneLock.on_stale = staticmethod(got.append)
        FL.PhoneLock._busy_since = {}
        # a child named "adb" that inherited the lock fd (what the leak looked like)
        fake = d + "/adb"
        os.symlink("/bin/sleep", fake)
        fd = open(path, "a"); fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        child = subprocess.Popen([fake, "30"], pass_fds=(fd.fileno(),))
        fd.close()
        try:
            self.assertEqual([c for _, c in FL.lock_holders(path)], ["adb"])
            t0 = time.time()
            self.assertFalse(lk.acquire(now=t0))                       # busy, clock starts
            self.assertFalse(lk.acquire(now=t0 + 300))                 # < 10 min: still busy, no alert
            self.assertEqual(got, [])
            self.assertTrue(lk.acquire(now=t0 + 700))                  # > 10 min, holder is adb only: recovered
            self.assertEqual(len(got), 1); self.assertIn("STALE-LOCK", got[0])
            lk.release()
        finally:
            child.kill(); child.wait()

    def test_python_children_do_not_inherit_the_lock(self):
        import farmlib as FL, tempfile, subprocess, fcntl
        d = tempfile.mkdtemp()
        lk = FL.PhoneLock({"alias": "y", "lock": "y"}); lk.path = d + "/l.lock"
        self.assertTrue(lk.acquire())
        r = subprocess.run(["ls", "/proc/self/fd"], capture_output=True, text=True)
        self.assertFalse(any(os.path.realpath(f"/proc/self/fd/{x}") == lk.path for x in []))
        p = subprocess.run(["sh", "-c", "ls -l /proc/self/fd"], capture_output=True, text=True).stdout
        self.assertNotIn("l.lock", p)
        lk.release()


class PublishUnit(unittest.TestCase):
    """bench publishing into a temp origin: own clone, no collision with ff-merging workers, results-only commits never trigger a bench"""
    def git(self, d, *a):
        r = subprocess.run(["git", "-C", d, "-c", "user.name=t", "-c", "user.email=t@t", *a], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.strip()

    def row(self, phone, drv, commit, variant="triangle-3k", bench="triangle", gpu=3.0, ver=None):
        return {"date": "2026-11-18", "bench": bench, "variant": variant, "phone": phone, "class": "v11", "driver": drv, "version": ver or (commit if drv == "ours" else "malideck p22"),
                "commit": commit[:9], "commit_full": commit, "job": "j", "ref": "main", "best_settings": "x",
                "presented": {"fps": 60, "low1": 50, "over50": 0, "cpu_ms": 1, "gpu_ms": gpu}, "offscreen": {"fps": 90, "cpu_ms": 1, "gpu_ms": gpu}}

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.origin = os.path.join(self.tmp, "origin.git")
        subprocess.run(["git", "init", "-q", "--bare", "-b", "main", self.origin], check=True)
        self.work = os.path.join(self.tmp, "work")
        subprocess.run(["git", "clone", "-q", self.origin, self.work], check=True, capture_output=True)
        os.makedirs(os.path.join(self.work, "bench"))
        open(os.path.join(self.work, "bench", "results.jsonl"), "w").close()
        open(os.path.join(self.work, "README"), "w").write("x")
        self.git(self.work, "add", "-A")
        self.git(self.work, "commit", "-qm", "seed")
        self.git(self.work, "push", "-q", "origin", "HEAD:main")
        self.state = os.path.join(self.tmp, "state")
        os.makedirs(os.path.join(self.state, "bench"))
        self.rep = [sys.executable, "-c", "import json;n=len(open('bench/results.jsonl').read().split());open('bench/RESULTS.md','w').write('table %d' % n)"]

    def put(self, *rows):
        with open(BL.results_path(self.state), "a") as f:
            f.write("".join(json.dumps(r) + "\n" for r in rows))

    def pub(self, **k):
        return BL.publish(self.state, self.origin, report_cmd=self.rep, log=lambda m: None, **k)

    def test_publish_commit_message_rows_and_idempotence(self):
        c1 = "a" * 40
        self.put(self.row("cmf", "ours", c1), self.row("cmf", "bridge", c1), self.row("cmf", "ours", c1, "cube-720p", "cube", gpu=0.2))    # last one is implausible
        r = self.pub()
        self.assertEqual((r["status"], r["n"]), ("pushed", 2), r)
        self.git(self.work, "pull", "-q", "origin", "main")
        self.assertEqual(self.git(self.work, "log", "-1", "--format=%s"), "bench: cmf rows for " + c1[:9])
        self.assertEqual(len(open(os.path.join(self.work, "bench", "results.jsonl")).read().splitlines()), 2)
        self.assertTrue(os.path.exists(os.path.join(self.work, "bench", "RESULTS.md")))
        self.assertEqual(self.pub()["status"], "nothing")                                            # idempotent: no empty commit
        c2 = "b" * 40                                                                                # a newer version keeps the older rows (one block per version)
        self.put(self.row("cmf", "ours", c2), self.row("poco-x4", "ours", c2))
        self.assertEqual(self.pub()["status"], "pushed")
        self.git(self.work, "pull", "-q", "origin", "main")
        keys = {(r["phone"], r["driver"], r["commit"]) for r in map(json.loads, open(os.path.join(self.work, "bench", "results.jsonl")))}
        self.assertEqual(len(keys), 4)
        self.assertIn(("cmf", "ours", c1[:9]), keys)
        self.assertEqual(self.git(self.work, "log", "-1", "--format=%s"), "bench: cmf,poco-x4 rows for " + c2[:9])

    def test_rejected_push_is_retried_and_other_checkouts_untouched(self):
        self.put(self.row("cmf", "ours", "c" * 40))
        # a worker ff-merges into origin right between our fetch and our push (pre-push hook of the farm's clone)
        clone = os.path.join(self.state, "bench", "publish-clone")
        subprocess.run(["git", "clone", "-q", self.origin, clone], check=True, capture_output=True)
        flag = os.path.join(self.tmp, "flag")
        hook = os.path.join(clone, ".git", "hooks", "pre-push")
        open(hook, "w").write(f"#!/bin/sh\n[ -e {flag} ] && exit 0\ntouch {flag}\ncd {self.work} && echo more >> README && git -c user.name=t -c user.email=t@t commit -qam worker && git push -q origin HEAD:main\nexit 0\n")
        os.chmod(hook, 0o755)
        before = open(os.path.join(self.work, "README")).read()
        r = self.pub()
        self.assertEqual(r["status"], "pushed", r)
        self.assertTrue(os.path.exists(flag))                                                        # the first push really was rejected
        subs = self.git(self.origin, "log", "--format=%s", "main").splitlines()
        self.assertEqual(subs[0], "bench: cmf rows for " + "c" * 9)
        self.assertIn("worker", subs)                                                                # the worker's commit survived (rebased onto, never overwritten)
        self.assertNotEqual(before, "")
        self.assertEqual(self.git(self.work, "status", "--porcelain"), "")                          # the workers' checkout was never touched

    def test_results_only_commit_does_not_trigger_a_bench(self):
        self.assertEqual(BL.plan_trigger("a", "b", ["bench/results.jsonl", "bench/RESULTS.md", "bench/best.json"], ["v11", "v13"]), ("b", []))
        self.assertFalse(BL.touches_driver(["bench/results.jsonl", "bench/RESULTS.md"]))

    def test_stale_best_rules(self):
        import datetime, importlib.machinery, importlib.util
        ld = importlib.machinery.SourceFileLoader("bench2mod", os.path.join(os.path.dirname(HERE), "bench2"))
        m = importlib.util.module_from_spec(importlib.util.spec_from_loader("bench2mod", ld))
        ld.exec_module(m)
        d = datetime.date(2026, 11, 20)
        fresh = {"cmf": {"ours": {"cube-720p": {"date": "2026-11-18", "sig": m.sweep_sig("cube-720p", "ours")}}}}
        self.assertIsNone(m.best_stale(fresh, "cmf", "ours", "cube-720p", d))
        self.assertIn("days old", m.best_stale(fresh, "cmf", "ours", "cube-720p", datetime.date(2026, 11, 26)))     # > 7 days
        self.assertIn("no stored best", m.best_stale(fresh, "cmf", "ours", "triangle-3k", d))
        fresh["cmf"]["ours"]["cube-720p"]["sig"] = "000000000000"                                      # a new knob/candidate changes the fingerprint
        self.assertIn("new knob", m.best_stale(fresh, "cmf", "ours", "cube-720p", d))
        old = m.OURS_ENV[:]
        m.OURS_ENV.append(["MALI_NEW_KNOB=1"])
        self.assertNotEqual(m.sweep_sig("cube-720p", "ours"), "x")
        sig_new = m.sweep_sig("cube-720p", "ours")
        m.OURS_ENV[:] = old
        self.assertNotEqual(sig_new, m.sweep_sig("cube-720p", "ours"))
        cfg = m.plan("cube-720p", "ours", "final", {"pres": {"env": ["MALI_RING=1"], "mode": "mailbox"}, "off": {"env": [], "off": True}})
        self.assertEqual(cfg[0]["env"], [])                                                            # cube never replays a non-base env for ours


class GateTemp(unittest.TestCase):
    """Poco X4: dumpsys battery 35.2 C, thermalservice 'Cached temperatures' BATTERY frozen at 33.1 (stale), HAL current BATTERY 35.2 / SKIN 37.6"""
    DUMP = ("  level: 97\n  temperature: 352\nThermal Status: 0\nCached temperatures:\n"
            "\tTemperature{mValue=33.1, mType=2, mName=BATTERY, mStatus=0}\n\tTemperature{mValue=38.975, mType=3, mName=SKIN, mStatus=0}\n"
            "Current temperatures from HAL:\n\tTemperature{mValue=35.2, mType=2, mName=BATTERY, mStatus=0}\n"
            "\tTemperature{mValue=%s, mType=3, mName=SKIN, mStatus=0}\n")

    def test_highest_of_battery_and_skin(self):
        self.assertEqual(gate.parse(self.DUMP % "37.6")["temp_c"], 37.6)
        self.assertEqual(gate.parse(self.DUMP % "43.4")["temp_c"], 43.4)       # skin hotter than the battery: it gates
        th = dict(gate.DEFAULTS)
        self.assertEqual(gate.next_state("ok", gate.parse(self.DUMP % "43.0"), th)[0], "cooling")
        self.assertEqual(gate.next_state("ok", gate.parse(self.DUMP % "42.9"), th)[0], "ok")        # worked right up to 43

    def test_stale_cache_is_ignored(self):
        t = "  temperature: 300\nCached temperatures:\n\tTemperature{mValue=47.7, mType=2, mName=BATTERY, mStatus=0}\n"
        self.assertEqual(gate.parse(t)["temp_c"], 30.0)
        self.assertEqual(gate.parse("  temperature: 470\nCurrent temperatures from HAL:\n\tTemperature{mValue=33.1, mType=2, mName=BATTERY, mStatus=0}\n")["temp_c"], 47.0)

    def test_die_sensors_not_used(self):
        t = "  temperature: 350\nCurrent temperatures from HAL:\n\tTemperature{mValue=70.0, mType=1, mName=GPU, mStatus=0}\n"
        self.assertEqual(gate.parse(t)["temp_c"], 35.0)

    def test_device_address(self):
        d = tempfile.mkdtemp()
        open(d + "/d.toml", "w").write('[[device]]\nalias = "poco-x4"\nserial = "SER1"\nlock = "poco-x4"\naddress = "192.0.2.5:5555"\n[[device]]\nalias = "cmf"\nserial = "SER2"\n')
        os.environ["MALI_FARM_DEVICES"] = d + "/d.toml"
        try:
            self.assertEqual(gate.device_address("poco-x4"), "192.0.2.5:5555")
            self.assertEqual(gate.device_address("SER1"), "192.0.2.5:5555")
            self.assertIsNone(gate.device_address("cmf"))
        finally:
            del os.environ["MALI_FARM_DEVICES"]


if __name__ == "__main__":
    unittest.main()
