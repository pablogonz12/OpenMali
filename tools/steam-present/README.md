# steam-present: Steam Big Picture menu frame rate in the DroidDeck guest

`tools/steam-present/steam-present` starts (or reuses) the Steam Big Picture menu in the DroidDeck
guest, keeps it moving with a D-pad sweep, samples 30 s of real presents and prints:

* mean fps over the window,
* p50 / p95 / p99 frame interval in ms,
* the pacing class (display-paced / frame cap / CPU stalls / client or GPU bound),
* the panel mode it measured against, the layer name, and a coarse CPU total.

No phone numbers live in this repository. Rows go to stdout, or to `--json <file>`.

## Why SurfaceFlinger

The stack offers four counters. Only one is both trustworthy and invisible to what it measures:

| source | what it counts | verdict |
|---|---|---|
| SurfaceFlinger `--latency <layer>` | `desiredPresent` / `actualPresent` / `frameReady` timestamps of the compositor layer, i.e. pixels that reached the screen | **primary**: independent of the Vulkan ICD, no guest change, no overlay |
| gamescope `-T/--stats-path` | one `fps=<float>` line per 300 paints (`steamcompmgr.cpp`) | coarse cross-check only (~2.5 s resolution, no intervals); can be enabled with `BL_GAMESCOPE_ARGS=--stats-path <fifo>` |
| Steam overlay FPS counter | Steam's own frame counter, drawn over the UI | changes what it measures; not scriptable |
| MangoApp / MangoHud | gamescope stats with an overlay / a Vulkan layer writing a CSV | intrusive (extra layer and process) and off by default in benchmark sessions |

Driver-side `VK_KHR_present_id` / `VK_KHR_present_wait` and `vkCmdWriteTimestamp` are advertised by
mali-open, but they only report what an application chooses to arm; Steam's CEF/ANGLE client does not
arm them, so they cannot be the primary counter.

## Usage

```sh
tools/steam-present/steam-present cmf                      # installed (bridge) ICD, 30 s
tools/steam-present/steam-present cmf --driver ours        # mali-open a64-glibc-wl ICD
tools/steam-present/steam-present cmf --driver ours --uncapped
tools/steam-present/steam-present cmf --seconds 45 --json /tmp/menu.jsonl
tools/steam-present/steam-present cmf --dry-run            # print the plan, touch nothing
```

`poco-x8` is the owner's daily phone and is refused unless `--allow-owner-phone` is given after
coordinating with its lane. `poco-x4` has no guest route for this suite.

The tool takes the qsafe lock with `farmlib.PhoneLock` (non-blocking by default; `--wait N` blocks up
to N s, like qsafe), runs the farm health gate (battery 20/50 %, thermal 43/38 C, owner disable
switch), stops any running session, applies one-shot env and the `hudDeck=false` pref, starts Steam
with `droiddeckctl start steam --wait`, settles, warms up with input, samples, and restores the
previous env and pref afterwards (or leaves the session with `--keep`).

`--driver ours` pushes `vk/out/a64-glibc-wl/{libvulkan_mali.so,mali_icd.json}` to
`Download/steam-present/` and starts the session with `BL_VKBRIDGE=0 BL_VK_DRIVER=<manifest>
BL_OPEN_DRIVER=1 BL_DRM_STANDIN=/dev/mali0 gamescope_composite_force=false`; it fails the run unless
the session log says `vulkan driver: imported`. Build the guest ICD with
`MESA_BUILD=$PWD/third_party/build-vk-a64-glibc-wl MALI_TARGET=a64-glibc-wl vk/build.sh`.

## Pacing classes, and telling a 60 fps cap from a 120 fps render

`base = 1000 / panel_hz` (8.33 ms on a 120 Hz panel). Let `fast` be the share of intervals
`<= 1.3 * base`.

| measurement | class |
|---|---|
| p50 <= 1.25 * base and fast >= 90 % | display-paced at the panel rate (120 fps) |
| p50 ~ 2 * base (within 12 %), fast < 10 %, >= 90 % of intervals at 2 * base | **half-panel cadence**: every frame lands on every second vsync. This does not prove a 60 fps cap |
| p50 ~ 2 * base but < 90 % of intervals there | slow tail: a share of the frames take much longer than the median |
| 15 % <= fast <= 85 % | bimodal: CPU stalls / missed vsyncs (frames alternate cadence) |
| p50 > 1.25 * base (and not the cap case) | client or GPU bound |

Half-panel cadence is **ambiguous**: a 60 fps cap and a 120 fps producer whose alternate frames are
dropped before presentation put identical timestamps on the screen (0, 16.67, 33.33 ms), so the tool
reports the observed cadence and leaves the renderer-cap diagnosis open. Another measurement (for
example the guest-side produced-frame counter) has to establish it.

`--uncapped` changes several things at once (performance mode, swapchain image count, the webhelper
frame-rate flags), so a gain with it is not evidence of a cap: a GPU-bound session also gains from
performance mode. To attribute a gain to one limiter, change one limiter per run and compare.

A sample is rejected (exit 4) when SurfaceFlinger's 128-frame history ring lost frames between two
polls, or any poll failed: a gap would otherwise be counted as one very long frame.

`cpu_percent` is the summed `%CPU` of `steamwebhelper` / `gamescope` / `steam` from one `top -b -n 1`
sample. High CPU plus bimodal intervals points at CPU stalls; low CPU plus a uniform interval at a
vsync multiple points at a cap.

## Limiter table (lift these before believing a low number)

| layer | knob | effect / how to lift |
|---|---|---|
| panel | active display mode, `settings get system peak_refresh_rate` | the CMF exposes modes 60 and 120; a static screen can idle at 60. Check `dumpsys display` shows `fps=120` **before** measuring; set peak refresh rate to 120 for the run |
| gamescope | `gamescope_composite_force` | Lua treats `0` as truthy: `=0` enables compositing. Use `=false` for pass-through, or `BL_VKBRIDGE_WEBHELPER_ARGS` / `BL_GAMESCOPE_ARGS` for other convars |
| gamescope | `--fps-limit` / `gamescope_fps_limit`, vsync | unset the fps limit; `--adaptive-sync`/immediate where supported; the session may force vsync in nested mode |
| Steam/CEF | `--disable-frame-rate-limit --disable-gpu-vsync` | only injectable through `BL_VKBRIDGE_WEBHELPER_ARGS`, which the session applies **only when `BL_VKBRIDGE=1`**. With `--driver ours` there is no hook: measure the default first, then compare with the bridge |
| bridge | `VKBRIDGE_SWAPCHAIN_IMAGES=4..8`, `VKBRIDGE_PRESENT_EARLY=1` | more images remove `vkAcquireNextImage` stalls; early commit removes the GPU-done wait |
| DroidDeck | `MALIDECK_MODE=performance` | one-shot session env, lifts the app's power mode |
| overlay | `BL_MANGOAPP=0`, pref `session.hudDeck=false` | the overlay costs fps and adds a layer; the tool does both |

`--uncapped` sets `MALIDECK_MODE=performance`, `VKBRIDGE_SWAPCHAIN_IMAGES=8` and the CEF
`--disable-frame-rate-limit --disable-gpu-vsync` layout. It is a bridge profile; with `--driver ours`
only the first two apply, and the CEF switches cannot be injected without the bridge.

## Limits of the method

* SurfaceFlinger keeps the last 128 presents per layer. The tool polls every ~0.15 s plus adb
  latency, which is ~6x headroom at 120 fps; a stalled adb server longer than ~1 s can lose frames.
  The interval count is printed so a short window is visible.
* A static Steam menu legitimately presents nothing (looks like 0 fps). The D-pad sweep thread is
  what keeps it rendering; do not measure without it.
* The first `--latency-clear` plus a 5 s warmup drops pre-window rows. Rows with `actualPresent == 0`
  (not yet latched) are ignored.
* SurfaceFlinger sees the composed result, so with `gamescope_composite_force` on, a compositor
  cadence can mask or mimic the client rate; keep pass-through (`=false`) for client-side claims.
