# mali-farm: automatic hardware tests on whichever phone is attached

    tools/farm/farm submit <ref> --suite smoke|daily|bench --need v10,v11,v13 [--env "K=V ..."]
    tools/farm/farm status | wait <job> --timeout N | results <job>
    tools/farm/farmd [--once] [--fake]     # the daemon (systemd user unit: mali-farm.service)

State (queue, runs, artifacts, alerts.log) is in `~/.local/state/mali-farm/`, never in the repo.
`farmd` checks the commit out detached, builds a64-glibc under `timeout 300`, archives the `.so` by sha256 and runs the suite once
per needed device class on an attached, idle phone while holding the qsafe lock (`/tmp/qsafe-<lock>.lock`), then `qrestore`.
A class with no attached phone stays pending and is picked up when the phone reappears. A missing phone that has work waiting gets
`adb reconnect` / `reconnect offline` / `connect <wireless>` (never `kill-server`); after 10 min it is marked down (one line in alerts.log).
One JSON per run: `runs/<job>-<class>.json` (commit, dirty, .so sha256, env, suite, device, GPU, kbase, battery temp, counts, new failures,
newly passing, verdict pass|fail|error, wall time). `farm wait` exits 0 pass, 1 fail, 2 error, 3 timeout.

Suites: smoke = vk/tests/valrun.sh (20 loops, honours --env); daily = qdeqp on lists/daily-sample.txt diffed per case against
`vk/deqp/ours-daily-sample-<baseline>.tsv` (from the commit, else main; --env goes to deqp-vk); bench = stub (error).
Only committed state is tested (`dirty` records uncommitted edits at submit time). Needs the untracked `third_party/` and dEQP build of the main checkout.

## Config (private, in ~/.config/mali-farm/; nothing machine-specific in the repo)

`farm.toml`: `quick_root` (has scripts/quick/qrestore and tools/) and `main_checkout` (holds the untracked `third_party/` Mesa build,
`vk/deqp/out` dEQP build and `vk/deqp/lists/daily-sample.txt`; farmd fails the job with a message naming what is missing).
`service.env` (for the systemd unit): `MALI_FARM_DIR=/path/to/checkout/tools/farm`.

Transport: per alias farmd resolves the live adb id: USB serial, else the mDNS entry `adb-<serial>-*._adb-tls-connect._tcp`,
else `adb connect <wireless>`; it passes it to valrun/qdeqp/qrestore as `ADB_SERIAL_<LOCK>` (quick.py and qsafe resolve the same way).

## devices.toml

    [[device]]
    alias = "phone-a"            # used everywhere in the farm
    class = "v11"
    serial = "ADB_SERIAL"
    lock = "cmf"                 # qsafe lock name (valrun.sh knows cmf|poco)
    baseline = "cmf"             # vk/deqp/ours-daily-sample-<baseline>.tsv
    wireless = ["HOST:5555"]     # optional extra routes (farmd adds ip:5555 itself)

Tests without hardware: `python3 tools/farm/test_farm.py` (fake mode: `farmd --fake`, attached = aliases in `<state>/fake-attached`).

## Transport policy, wireless adb, health gates

Per device `transport`: `"usb+wireless"` (default: USB first, then the `adb-<serial>-*._adb-tls-connect._tcp` mDNS entry, then every
`wireless`/`address` route `ip:5555`, then `adb connect`), `"wireless"` (ignores USB) or `"usb"` (USB only, never connect).
Route setup, once and idempotent: when a `usb+wireless` phone is on USB and idle, farmd runs `adb tcpip 5555` (only if not already set),
reads the wlan0 IP, `adb connect`s it and stores `<ip>:5555` in the device's `wireless` list in devices.toml (`farmlib.store_route`).
It never touches `adb_wifi_enabled`; pairing Wireless Debugging itself is the owner's job. `adb tcpip` is lost on reboot until USB returns.

Charger-only phones: `require_charging = true` in devices.toml. Jobs run only while `dumpsys battery` shows AC/USB/Wireless/Dock powered.
On battery the farm does nothing on that phone (no pushes/tests/wake-ups, jobs stay pending, `farm status` shows `on battery: paused`).
Checked before each job, between dEQP chunks and every ~10 s during a run; charger removed mid-job = clean stop, part back to
pending (attempts not counted), lock released.

Owner kill switch: `farm disable <device> [--adb-off]`, `farm enable <device>`; or just `touch <state>/disabled/<alias|lock>`
(`~/.local/state/mali-farm/disabled/`). A disabled device is never touched by farm or qsafe (jobs stay pending, `farm status` shows
`disabled by owner`; a running job is stopped). `--adb-off` additionally, if reachable: `settings put global adb_wifi_enabled 0`,
`adb usb` (close the tcp listener) and `adb disconnect` of its routes.

Health gates (farmd before each job and between dEQP chunks; quick.py/qsafe for agents; shared state `gate-<lock>.json`, shared module
`gate.py` == `scripts/quick/mgate.py`): battery < 20 % => `charging` until >= 50 %; battery temp >= 43 C or Android thermal status >=
SEVERE => `cooling` until <= 38 C and status <= LIGHT. Per-device override in devices.toml: `batt_stop batt_resume temp_stop temp_resume
thermal_stop thermal_resume`. A gated device keeps its jobs pending, writes one alerts.log line per state change, `farm status` shows
state/battery/temp, run JSON has battery_pct_/temp_c_ start+end. qdeqp stops with rc 75 and resumes from its TSV.

## Result integrity
- The driver `.so` is verified by sha256 ON THE PHONE against the job's `so_sha256` before every dEQP chunk (qdeqp, `QDEQP_SO_SHA256`),
  after valrun's push and after each run; a mismatch makes the run `invalid` (kept as `runs/<job>-<class>.invalid-N.json`, alert line, re-queued).
- Each dEQP run uses its own on-phone ICD paths (`QDEQP_RUN`), so concurrent runs cannot swap each other's driver.
- Device tests (qdeqp, qdrv, qdirect, valrun) refuse (exit 73, PHONE BUSY) unless they hold `/tmp/qsafe-<lock>.lock` or run under the holder
  (`QSAFE_HELD=<lock>`); `QSAFE_LOCK_OVERRIDE=1` for emergencies only.
- `ns_to_fail` lists baseline-NotSupported cases that now Fail; `known-flaky.txt` cases are ignored by the verdict but reported.
- Phones are identified by `getprop ro.serialno` on any transport; devices.toml is re-read on change; the stored `:5555` route follows the phone's IP.

## Baselines and bless
- A daily's verdict counts a case as regressed if it is Pass in the branch's own committed baseline OR in main's baseline; the notes name both commits.
- `farm bless <job> [--checkout DIR]` copies the job's per-class result tsv over `vk/deqp/ours-daily-sample-<baseline>.tsv` in DIR. Refused unless the job
  ran main's current commit on a clean tree, verdict pass, `so_sha256` confirmed on every part, and it is the full daily (1381 cases).

## Shell route (`route = "shell"` in devices.toml)
For a phone with no in-app (guest) route, e.g. the Poco X4 GT (class v10): `farm submit <ref> --need v10` runs `tools/farm/shellroute.py` under the phone lock.
- smoke: bionic ICD + `t_vk_draw/compute/meta --direct` from `vk/out/a64-bionic`, in a per-run dir `/data/local/tmp/farm/<job>-<alias>`.
- daily: glibc deqp-vk staged once in `/data/local/tmp/g` (see the phone's DEVICE doc), our glibc ICD at the per-run path `g/icd.<job>-<alias>.so`.
- the sha256 of the job's .so is checked on the phone before every chunk/test and once more by farmd afterwards; a mismatch is `invalid` and re-queued.
- same lock guard (exit 73), health gate before every chunk (exit 75 = paused, resumes from its TSV) and owner kill switch as the other routes.
