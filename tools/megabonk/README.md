# tools/megabonk: Megabonk (Steam 3405340) on OUR driver, one command

```
tools/megabonk/run.sh <alias> [--wait 90] [--secs 60] [--cull] [--farm] [--dry-run] [--out DIR]
tools/megabonk/test_megabonk.py          # host only, no phone: python3 -I tools/megabonk/test_megabonk.py
```

Run it through the farm: `--farm` takes `farm hold <alias> --max <min>` (capped at 20) and re-runs itself inside the hold; farmd
restores the phone on release. Without `--farm` use it inside your own `farm hold` (ANDROID_SERIAL comes from the hold). Always
try `--dry-run` first: it prints every command and runs nothing.

## Needs, once, by hand (nothing automates these)
- MediaDeck with Debugging tools on, and a driver bundle named `Mali-open*` imported (zip from
  `driver/vk/package/dist/mali-open-a64-glibc-wl-*.zip`).
- A driver build that reads `MALI_ADVERTISE_GS=1` (lane/gsshim, #156; not in main yet). `MALI_ADVERTISE_CULL=1` is only set with
  `--cull` and only matters once a driver reads it (nothing does today).
- Megabonk installed in the Steam library of the session.

## What it does
1. Pre-flight: owner switch (`<state>/disabled/<alias>`), battery < 38 C (`MB_TEMP_MAX`), `qpreflight` (foreground rule), Steam
   session up (`start steam --reuse`).
2. Select the driver: guest `/root/.malideck/debug=on`, `driver=ours`, pref `vkBridge=false`. Collects evidence (driver pref, debug,
   vkBridge, env list, `driver-setup.txt` tail, guest `vulkaninfo driverName`, DXVK log `Found device`) and **refuses before the
   launch** if it says bridge/panvk/conflict.
3. `droiddeckctl env set MALI_ADVERTISE_GS=1 MALI_SHADER_CACHE=<dir> DXVK_LOG_LEVEL=info DXVK_LOG_PATH=/tmp/dxvk-logs`, restart the session.
4. `launch 3405340 --wait`, sleep `--wait` s.
5. `droiddeckctl pacing --seconds 60` (SurfaceFlinger present-to-present histogram of the game layer, same as bench.sh/qgame). Prints
   avg fps, 1% low (1000 / p99 interval) and % frames <= 8.3 ms (the histogram has 1 ms bins: counted as <= 8 ms), with the
   `DRIVER-EVIDENCE` line. If the game is not focused or no frames arrive it prints the first DXVK/Vulkan error lines instead.
6. Always (EXIT trap): quit the game, `debug=off driver=bridge`, `qrestore <alias>` (stops the session, clears env, vkBridge=true).

## Evidence rule
`mblib.py evidence` classifies like MediaDeck's `driverinfo.py`: ours | bridge | panvk | unknown | conflict. The driver pref is only
the *selection*; `ours` needs proof (loaded ICD path or a driverName naming mali-open) and no vkBridge=true / Debug off. Anything
else prints `REFUSED ...`, exit 3, and **no fps**. Exit codes: 3 refused, 5 pre-flight (thermal/disabled), 6 game did not start, 4 no frames.

## Untested on a device
Written from the code and docs only (no phone was touched). The guest `/root/.malideck/*` files, `prefs get session` JSON shape and
`droiddeckctl logs latest` layout are taken from `megapath.md` and quick.py; fix the collect step on first contact if one differs.
