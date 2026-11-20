# MEASURE.md — the measurement contract

Every benchmark number in `docs/BENCH.md`, in a lane report, in a commit message or in a chat
summary is governed by this file. If a number does not satisfy this contract it is not a result:
it is a claim, and claims go to `docs/rejected.jsonl` or stay in prose, never on a chart.

## One metric: displayed fps

`fps` means **frames per second a human could see on the panel**. The only acceptable sources:

| source | field | notes |
|---|---|---|
| menubench (`MediaDeck/results/menubench.jsonl`) | `screen.fps` | SurfaceFlinger present timestamps for the panel |
| SurfaceFlinger / perfetto present trace | present timestamps | count frames presented, divide by window |
| a HUD the user reads on screen | the number on screen | quote the screenshot/labelled capture, not a log line |
| driver-mode `vkcube` present counter | the fps the cube shows on the panel | `bench/driver.jsonl` only; quote the lane note (`/tmp/opencode/*.md`), never a render loop rate |

Everything else lives in its own field and is **never plotted, never compared, never used for
"best"**:

* `render.avg_fps` (menubench) → `render_fps`
* `presented.fps` (bench2) → `presented_fps`
* any offscreen producer rate → `producer_fps`

A 120 Hz panel cannot display 421 fps and a 144 Hz panel cannot display 850 fps. The old store
did exactly that by reading `render.avg_fps` / `presented.fps` as `fps`; it was reset on
2026-11-20 (`docs/bench-archive/2026-11-20-invalid/`).

## Lanes: how to run a valid point

Each measurement is **3 reps × 30 s** of the *same* scenario string, on the same device and
build, same profile:

1. **Same scenario string.** Pass one literal string (e.g. `lr8x@150ms+shelf/3`) to every rep;
   a different string is a different cell and must not be pooled with the others.
2. **Diagnostics env cleared.** Unset `VKBRIDGE_FRAME_TRACE`, `VKBRIDGE_STATS` and
   `MALIDECK_KEEP_DIAG` for measurement runs. They change pacing and inflate the render rate.
   Record the env you did use in the source string.
3. **Brightness-verified.** The run counts only if `brightness_mean > 20` **and** the screenshot
   shows a populated UI (menu, cards, HUD text). A black frame reads ≈ `0.19`; a run with a
   black screen is stored as `status: black`, never as fps 0 "measured".
4. **Name the displayed-fps source** in `source` (`screen.fps`, SurfaceFlinger, HUD). A number
   with no named source is `unverified`.
5. Record `reps`, `panel_hz`, `profile`, `driver_sha`/`version` and the workload on every row.
   A row missing `reps` or `panel_hz` cannot be plotted.

### Workload names: game lanes and WSI smoke lanes

Two kinds of workload share the store, and they are never compared with each other:

* **game / app lanes** — `cards`, `triangles`, `steam-menu`, `megabonk`. A real workload whose
  displayed fps a player would feel. These compete for "best" inside their own lane only.
* **WSI smoke lanes** — `vkcube-wayland` and `vkcube-xcb`. `vkcube` on the same device and build,
  run to prove a swapchain + present path works and at what pace the cube is paced:
  `vkcube-wayland` is the Wayland path (`present -> VK_SUCCESS`, exit 0), `vkcube-xcb` is the
  X11/dri3 path (`vkCreateXcbSurfaceKHR` + putimage/shm present). A smoke lane is a
  **go/no-go on a WSI path, not a game fps**: never plot it against `cards`/`triangles`, never
  quote it as "the driver's performance", and never compare a wayland point with an xcb point
  (different presentation paths, different pacing). Run the exact command
  (`vkcube --c 20 --wsi wayland` / `--wsi xcb`) in `source`, with `--c` and the diagnostics env
  cleared; a smoke lane has no screenshot brightness gate (see below), so record
  `brightness_mean: null` and say so in `note` rather than inventing one.

### Profiles are not comparable
`profile=default` is the comparable lane. `uncapped` runs are *skipped* by the importer: they
are a different configuration, they are never compared with `default`, and they can never win a
"best". Other variants (`cefv0`, `cefv1`, …) are stored with their profile but are not drawn as
default lines and do not compete for best.

### Best

`best` = **median of at least 3 reps** with the same device, stack, version, profile, panel and
run day. No best exists for a group with 1 or 2 reps, however good the single number looks.

## Rejection rules (importer-enforced, never clamped)

| condition | reason in `docs/rejected.jsonl` |
|---|---|
| `ok != true` (session not ready, no frames, selfcheck failed) | `not_ok` |
| `profile == "uncapped"` | `uncapped_profile` |
| no displayed-fps source (only render/presented/offscreen) | `presented_fps_not_displayed`, `render_avg_not_displayed`, `no_displayed_fps` |
| `fps > panel_hz + 2` | `above_panel_ceiling` |
| panel missing or device unknown | `no_panel_hz`, `unknown_device` |
| workload not a known lane (`bench/driver.jsonl`) | `unknown_workload` |
| a figure typed into a doc/FINDINGS/owner note | `doc_claim_not_measurable` |

`fps > panel_hz + 2` is instrument error: the sample is rejected and logged with the observed
value. It is never clamped to the ceiling, because a clamped number hides the bug.

## Schema (`docs/bench.jsonl`, one JSON object per line)

```
{ts, device, stack, version, driver_sha, workload, metric:"displayed_fps", profile, fps,
 panel_hz, reps, brightness_mean, status: visible|black|crash|not_run|unverified, source,
 render_fps, presented_fps, off_cadence_pct, run, note}
```

`off_cadence_pct` is the share of presents that missed a refresh slot (the `pacing` harness
reports it next to the fps). It is the signal that a rate is *even* rather than an average that
hides stutter: 0.8 % on 118 fps is clean pacing, 40 % would not be. Informational like
`render_fps` — it never replaces `fps`.

`run` names one measurement session (`r1`, `r2`, …) when the same device/build/lane is measured
twice on the same day. Two sessions are two groups: `reps` counts the reps inside one run and
`best` is per run, so a newer run that supersedes an older one shows as its own row instead of
being pooled into a 6-rep median. The older run stays in the store (history is never deleted);
its `note` says what superseded it.

`render_fps` / `presented_fps` are informational only. Charts read `fps` only, draw the panel
ceiling (X8/CMF 120 Hz, X4 144 Hz) as a dashed line, and label every point with its `reps`.

## Regenerating

```
make bench        # import → docs/bench.jsonl + docs/rejected.jsonl, charts → docs/BENCH.md
make bench-check  # CI gate: fails if a committed generated file is stale
```

A lane that changes a number must cite this file and the exact scenario string in its report.
