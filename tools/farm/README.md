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

## Guest on a shell-route phone (`guest = true`, `foreground_launch = true`)
`route = "shell"` keeps smoke and dEQP daily on the fast adb-shell route. `guest = true` additionally lets the in-app guest (MediaDeck) serve what only it can: `bench` (auto triggers and `--suite bench`) and `menu`. `foreground_launch = true` (our own test phone, never the lent one): a screen-on phone with only the launcher in front is not "the owner"; before a menu run farmd starts MediaDeck itself (`monkey -p <pkg> -c LAUNCHER`). Any other foreground app still pauses the job. Heat keys per device: `temp_stop`/`temp_resume` (farm gate) and `temp_start` (bench2 starts no burst above it).

## Smoke fallback on a locked guest-route phone (`smoke_fallback = "shell"` in devices.toml)
The guest smoke (valrun.sh -> qdrv -> `t_vk_*` in the MediaDeck app's Linux guest, glibc build) needs a READY app session; with the screen off or the keyguard
showing it stays SUSPENDED. For a phone with `smoke_fallback = "shell"` (the Poco X8, v13) farmd reads `dumpsys power` (read-only) and, when the phone is not Awake
or the keyguard shows, runs the smoke through the shell route instead (also when the guest route ran but produced no result: valrun rc 74 / "session not READY").
- Same binaries, bionic build of the job's tree (built on demand when the job was built earlier) under the adb shell user, this job's driver, `.so` sha256 checked on the phone before every test and after the run.
- Nothing on the phone's screen is touched (no brightness/screen commands, no input); only `adb shell` runs and the `/data/local/tmp/farm/<job>-<alias>` dir.
- The kept log (`farm log <job>`) starts with `SMOKE-ROUTE route=guest|shell|shell-fallback serial=<transport> so_sha256=<the .so that ran> reason=...`; the part's notes say `route=shell-fallback` and list what is skipped (present/swapchain, gamescope: need the session's display; no smoke test uses them today).
- **dEQP (`daily`, `cases`, qdeqp.py) has no such route**: it also runs in the guest session (`start_steam`, `droiddeckctl guest`), so it cannot produce results on a locked guest-route phone (a job that finds the phone locked ends `invalid`/pending, never with results).
- A guest-route smoke without a result of THIS run is an `error` (valrun rc 74), never another phone's old result. farmd re-installs its valrun.sh into a job tree whose copy lacks that fix.

## Cheap commands (one short command instead of ad-hoc scripts; each prints <= ~40 lines of plain text)
- `farm cancel <job>... [--running]`: pending parts become `cancelled` (never run); a running part needs `--running`, which stops it within one poll and releases the lock. Finished parts are untouched.
- `farm submit <ref> --suite cases --case <name> [--case ...] --repeat N --need <class>`: only those cases, N times (one dEQP run per repetition, per-run .so path, sha256 on the phone, lock, health gate).
  `farm results <job>` prints pass/fail/NS counts per case (`--json` for the raw run).
- `farm digest`: devices, queue (counts + the running part), and per ref/class the latest smoke/daily with counts and verdict, plus the cases main's latest daily passes that this ref fails or turns NotSupported (5 names + count).
- `farm diff <jobA> <jobB> [--class v11]`: case-level status changes grouped `A->B` (3 names per group).
- `farm case <substring>`: status of matching cases in every stored daily log (date, ref, short sha, device, status), newest last.

## Utilisation, gap removal and the idle sweep
- `farm util [--since today|6h|2d|ISO]`: per phone from the farm's own records (runs, jobs, alerts GATE lines, sweep chunks): busy % (and how much of it was the driver build under the lock), sweep %, and where the idle time went: no work queued, queue gap (build / poll / other class / lock held by a worker; not separable from the records), gated (cooling, on_battery ...).
- Gaps removed: the next queued job's driver is built by a separate prebuild thread while the phone runs the current job; builds are cached per commit (a second job of the same commit reuses the archived .so, no rebuild); a finished run wakes the scheduler at once and the poll interval is 4 s (`farmd --poll`); classes schedule independently (a part starts as soon as ITS phone is free).
- Heat: shell-route runs (`route = "shell"`, no display needed) set minimum brightness and turn the screen off, original values are kept on the phone (`/data/local/tmp/farm/dim.state`) and restored at the end of the run (or by `qrestore` if the run was killed). Every chunk logs battery/gpu/cpu temperature to `<state>/logs/temps-<lock>.log`.
- The shell route also pauses (gate "busy") while the phone's load average is above `SHELLROUTE_MAX_LOAD` (default 8): another app on the GPU makes dEQP crash.

### Idle sweep
When a phone is up, passes its health gates and its class has no queued part, farmd walks the full dEQP-VK mustpass list (`third_party/cts-src/.../mustpass/main/vk-default/*.txt` of the main checkout; 2,720,531 cases) on it with main's current build, in chunks (1500 cases, 300 s cap), same lock, gates, per-run .so and sha256 check as real jobs. Between and inside chunks any queued real job of that class preempts it (the running chunk is stopped within one poll, finished cases are kept, the rest is redone later). Crashes, hangs and cases without a result never stall it: they are recorded (Crash / Timeout), a case that yields no result twice is skipped, and the stress cases the daily sample also drops are marked Skipped.
- State `<state>/sweep/`: `<class>.json` (position, totals, per-group totals, earlier passes), `<class>-chunks.jsonl` (one line per chunk: start index, commit, .so sha, counts, temperatures, zlib+base64 status string per case), `<class>-fails.tsv` (non-Pass/NS cases of the current pass). A pass restarts from index 0 when it has finished or main moved by more than 25 commits since it began (the commit is recorded per chunk).
- `farm sweep status` (per class: % covered, Pass/Fail/NS/Crash/Timeout/Skipped totals, ETA, top failing groups), `farm sweep failures --class v11 [--group api.image] ` (names, capped at 40), `farm sweep off|on`. A device with `sweep = false` in devices.toml is never swept. Owner rules (battery, 43/38 C, charger-only, kill switch) apply unchanged: a gated phone is not swept. Extra sweep-only rules (`sweeplib.power_blocked`, `farmd sweep_blocked`): the sweep runs only on a charger (devices.toml `sweep_on_battery = true` opts a phone in, then only at >= `sweep_batt_min` %, default 80) and only at battery temperature <= `sweep_temp_max` (default 36 C); a guest-route phone is also left alone while its owner is in another app or its session cannot be READY (locked screen), so a chunk never holds the lock waiting for a focus.

## Bench suite and auto bench
- `farm submit <ref> --suite bench --need v11[,v13]`: builds the **a64-glibc-wl** (Wayland WSI) variant of the ref (`MESA_BUILD=third_party/build-vk-a64-glibc-wl`; `vk/cross/build-cross.sh glibc-wl` sets that tree up; the non-WSI `a64-glibc` cannot present), builds `bench/mdbench`, then runs `tools/bench2 <alias> --out <work>` (triangle CPU-heavy, cube GPU-heavy; ours vs the bridge, best settings from `bench/best.json`, interleaved medians). The build and harness happen BEFORE the phone lock; farmd holds the qsafe lock only while measuring (bench2 chunks of <= 230 s, up to 6 per part; "owner using the phone" / gate / budget left = part back to pending, resumes from the work dir's raw cache).
- Rows (one per phone/driver/variant) go to `<state>/bench/results.jsonl` with `ref`, `commit_full`, `job`, `phone`, ours `so_sha256`; `bench/RESULTS.md` is still generated by hand-run `tools/bench2 --report`, the farm does not touch the checkout. The driver .so on the phone is sha256-verified after the run (mismatch = `invalid`, re-queued).
- `farm digest` prints per phone the newest tested commit: triangle CPU ms and cube GPU ms, ours vs bridge (offscreen = uncapped).
- Auto triggers (`benchlib.py`, jobs marked `auto`, state `<state>/bench/auto.json`; `<state>/bench/off` disables them; `bench = false` in devices.toml opts a phone out; shell-route phones never get bench):
  1. main gets a commit that touches `vk/src/` or `libkbase/` -> one auto bench job for every class with an available phone (not absent/missing/down/disabled). First sight of main only records it (no job storm).
  2. a phone idle > 15 min (a preemptible sweep chunk counts as idle; clock restarts when it is gated, absent or runs a real job; `MALI_FARM_BENCH_IDLE` seconds to override) with health gate ok and no row for main's tip gets a bench job pinned to it; longest idle first.
- Priorities: explicit jobs > auto bench > idle sweep. Auto jobs queue behind every explicit job, and the sweep never starts while any part is pending. Errors are not retried automatically (the job exists); resubmit by hand.
- Steam menu benchmark (suite `menu`, `farm submit --suite menu`): runs MediaDeck's `scripts/quick/menubench <alias> --profile both --reps 3 --lockwait 30` (path: `menubench = "/path/to/MediaDeck/scripts/quick/menubench"` in farm.toml, or `MALI_FARM_MENUBENCH`). menubench takes the qsafe lock itself, so farmd does NOT hold it for this suite; exit 75 = rerun (up to 8 calls), 125 busy / 126 gated = part stays pending, 77 = version changed, rerun with `--new`. Rows come from MediaDeck's `results/menubench.jsonl` (entries since the job was created) -> one row per phone, MediaDeck versionCode and profile in `<state>/bench/menu.jsonl` (avg/median fps, 1 % low, p99 ms, panel Hz, reps ok/failed, valid): valid = at least one rep passed its self-checks (medians of those reps); an invalid row never replaces a valid one. The menu runs on the Mali bridge: numbers change with the MediaDeck build, not with mali-open commits.
  - Auto trigger: a phone idle > 15 min (gate ok) whose installed MediaDeck versionCode differs from the last one queued for it (recorded in `bench/menu-auto.json` when queued, so at most once per version; an error is not retried: resubmit by hand). Auto jobs sit below explicit jobs, level with the auto bench, above the sweep. Kill switch: `touch <state>/bench/menu-off`; per phone `menu = false` in devices.toml.
  - Owner in use: never started (and not queued) while `dumpsys power` shows mWakefulness=Awake with a resumed activity that is not MediaDeck (home/lock screen included; unreadable = in use). Stays pending, retried after 15 min.
  - Published with the bench rows: `bench/menu.jsonl` + a "Steam menu" section of `bench/RESULTS.md` (one block per MediaDeck version, newest first), one `menu` line per phone in `farm digest`.

## Flaky reruns
A daily (and a `cases` job) reruns just the cases that Fail but passed in the baseline (main's last daily for that GPU version), in the same job, up to 3 more times.
A case that passes in any rerun is flaky (`flaky: [{case, fails, runs}]` in the run JSON; not in `new_failures`) and goes to `<state>/flaky.tsv`
(case, GPU version, fails, runs, first/last seen). Digest/status show `daily pass 342/80/958 (+0 new, 2 flaky)`; digest also prints `new Fails against main: N`.
`farm flaky [--class v11]` prints the list; `farm flaky --seed [--days 3]` adds cases that flipped Pass/Fail on the same commit or driver binary, or on main
(two or more changes in time order), across stored dailies. Test hooks: `FAKE_FLAKY=N FAKE_FLAKY_PASS_ON=K`, `FAKE_FLAKY_CASES=1`.


## Queue priority and supersede

Every job has a `prio` (lower runs first, FIFO inside a level; explicit before auto at equal level). Defaults by suite: `smoke`/`daily` (merge
gates) 0, `bench`/`menu` 10, `cases` (exploratory, PIPE_DBG runs) 20; the idle sweep only runs when nothing is pending. `farm submit --prio N`
sets it, `farm prio <job> <n>` changes it later. A new `smoke`/`daily` job cancels the *pending* parts of older gate jobs of the same
suite, repo, env and `--only` whose commit is an ancestor of the new one (same branch line), for the classes the new job covers; running and
finished parts are never touched. The line goes to stderr and `<state>/logs/farm-queue.log`. `--keep-old` on the new job opts out.
`farm digest` prints `queue by class` (pending/running per GPU class) and every pending job with its position per class (`v11#3`).

## Driver debug output of a cases job

`farm submit ... --suite cases --env "MALI_PIPE_DBG=1 MALI_META_DBG=1"` (any `MALI_*_DBG` in the env, qdeqp route v10/v11): qdeqp.py keeps the dEQP
stdout lines `Test case ...` and the driver's stderr lines (`mali_cmd: unsupported ...`, `mali_meta_vk: plan error`) of every chunk; farmd copies
them to `<state>/logs/<run id>.dbg` (e.g. `20261118-050057-89a8bfd2-poco-x4-r0.dbg`). The QPA never carries them, the notes TSV only the CTS message.
