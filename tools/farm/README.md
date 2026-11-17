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
`vk/deqp/ours-daily-sample-<baseline>.tsv` (from the commit, else main; --env ignored); bench = stub (error).
Only committed state is tested (`dirty` records uncommitted edits at submit time). Needs the untracked `third_party/` and dEQP build of the main checkout.

## ~/.config/mali-farm/devices.toml (private; aliases only in the code)

    quick_root = "~/path/to/quick-tools-root"   # has scripts/quick/qrestore and tools/
    [[device]]
    alias = "phone-a"            # used everywhere in the farm
    class = "v11"
    serial = "ADB_SERIAL"
    lock = "cmf"                 # qsafe lock name (valrun.sh knows cmf|poco)
    baseline = "cmf"             # vk/deqp/ours-daily-sample-<baseline>.tsv
    wireless = "HOST:5555"       # optional, used by recovery

Tests without hardware: `python3 tools/farm/test_farm.py` (fake mode: `farmd --fake`, attached = aliases in `<state>/fake-attached`).
