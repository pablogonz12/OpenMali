# Phone lab (experimental)

These tools are a draft mirror of a particular lab's tooling. Review and adapt
the scripts and their dependencies before use; host syntax checks are not device
validation. They can change phone settings and files. Never run the old farm
service as part of this tooling.

## Setup

Copy `bin/` to `$MALI_LAB_ROOT/bin/`, where `MALI_LAB_ROOT` defaults to
`$HOME/.local/share/mali-phone-lab`. Copy `phones.conf.example` there as
`phones.conf`, fill in local values and keep that file private and outside the
repository. Columns are alias, space-separated device serials, DroidDeck package,
display refresh rate, and optional golden drift threshold, separated by `|`.

Install `adb`, `droiddeckctl`, Python 3, `jq` and the utilities used by the
individual scripts. Set `ADB`, `MALI_DROIDDECKCTL`, `MALI_REPO`,
`MALI_DEPLOY_DIR`, `MALI_LANES_DIR`, `MALI_DRIVER_LOOP` and
`MALI_MENUBENCH` as appropriate. `MALI_GITHUB_REPO` overrides the repository
for `pr-gate`. `FARM_HOSTS` lists other hosts to check over SSH: configure it
explicitly if other machines can run the farm. `M` overrides the lab root.

## Safety and usage

- Use the stock DroidDeck app and the driver under test, not a bridge. Do not
  install, build or patch the app as part of a driver test.
- Access phones through `bin/ddc <alias> ...`; a `REFUSED` or busy result is a
  stop signal, not a challenge to bypass. Never kill the shared adb server.
- Before a change, run `bin/phone-snap <alias> <label>` and record any lasting
  change with `bin/ledger <alias> add <what> <original> <undo>` before applying
  it. Finish with `bin/phone-restore <alias> --apply`; check restoration.
- Use `bin/smoke <alias>` for the product path. Diagnostic hand steps must
  explain a named product-path failure. Read screenshots, rather than relying
  on brightness or FPS; black and static frames have no meaningful FPS.
- Keep Steam offline, run only one Steam session on the LAN, and do not edit
  files in the guest Steam directory. Do not stream from another device.
- Respect battery and thermal refusals. Deploy only a stashed build using
  `bin/deploy <alias> <sha>` for the plan and `--apply` to execute; verify the
  expected commit with a clean smoke run.
- `bundle` and `import` are unverified on hardware. For `bundle`, create
  `$M/android-cache/<alias>.json` with `tar` (path to the cached Android
  half), `gpu` (alphanumeric identifier), and `sha256` (a map from each
  bundled Android filename to its independently verified SHA-256). Import
  records a next-session guest SHA-256 check; perform it before
  trusting the imported library. `IMPORT_HUNG` sends BACK once and requires
  manual inspection of the captured image; it never retries.
- `smoke`, `soak`, `bundle`, `import`, and direct session-changing `ddc`
  commands use `flock` per phone. Older already-running scripts do not hold
  these locks; do not use the lock as proof no legacy lane is active.
- Interpret `LIVE` as a changing live game, `SHELF_OK` as a live painted menu,
  and `REFUSED` as no change. `BLACK`, `STATIC`, `START_FAILED`, `WRONG_BUILD`
  and `INTERRUPTED` are not successful product-path results. Inspect captured
  images and logs, and stop if restoration failed.
- `soak <phone> --minutes 0 --pacing-scroll 5` runs three 30-second pacing
  reps while sending right every five seconds; read `pre-pacing.jpg` before
  interpreting results. Percentiles are estimated from integer histogram bins.

Golden images are manually approved references, never automatically promoted
from a run. `PICTURE_DRIFT` signals a difference, not a pass or failure.
Do not commit device identifiers, network addresses, logs or screenshots.

## Supervised queue

After setup, optionally install `mali-mq.service` in `~/.config/systemd/user/`
and enable it with `systemctl --user enable --now mali-mq.service`. Edit its
`ExecStart` if the lab root is not the default. `bin/mq submit <phone> --
<smoke|soak> [args...]` queues a job; `bin/mq status` shows queues and
`bin/mq wait <id> [--timeout seconds]` reports its result. Jobs run
sequentially per phone and in parallel across phones.

`$M/queue/STOP` prevents new starts without interrupting running jobs.
`<phone>.blocked` requires operator investigation of the result and phone
before clearing. An `.inflight` file after a crash represents an uncertain
outcome: reconcile it manually; the service will not replay it. Job results
are appended to `$M/queue/results.jsonl`. Never submit imports or deployments
to this queue.
