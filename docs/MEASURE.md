# Measuring the driver: the uncapped protocol

Every performance number this repo publishes is a **render_fps** number from a workload that
has no vsync, no frame limiter and no panel pacing. A capped run measures the screen (or the
compositor's pacing), not the chip: once the producer is faster than the panel, a capped
number sits at `panel_hz` and cannot say how much headroom is left. So a capped sample is
not a slower sample, it is a different quantity and never counts as a result.

Two tests carry the two halves of the load, plus one app-shaped workload:

| workload | binary | what it stresses | default shape |
|---|---|---|---|
| `gpu-heavy` | `vk/tests/t_bench_gpu.c` | fill and ALU: fullscreen triangle, 12-octave value-noise fragment shader (48 `sin`/pixel) | 1920x1080, 1 sample, offscreen |
| `cpu-heavy` | `vk/tests/t_bench_cpu.c` | submission/validation/queue overhead: 50k tiny draws per frame, one dynamic UBO offset each, 64x64 target | 50k draws/frame |
| `app` | `bench/mdbench` (`--wsi headless`) | the app-shaped cube scene at a fixed size, still headless | 1920x1080, cube:8/64 |

Both C tests render into an **offscreen image**, never a surface or swapchain: there is no
presentation path to cap them, and they print `vsync=none` / `present_path=none` so a run
that accidentally grew one is visible. They are normal Vulkan loader clients (`dlopen`
`libvulkan.so.1`), so the ICD is whichever one `VK_DRIVER_FILES` / `VK_ICD_FILENAMES`
selects — the same guest ICD the lanes deploy. No environ or device selection is baked in.

## Protocol

1. Clear the diagnostic env first (`VKBRIDGE_STATS`, `VKBRIDGE_FRAME_TRACE`, `MALIDECK_KEEP_DIAG`);
   it cost 15.5 fps on a device run.
2. Use the lane's ICD and confirm `driverName` in `vulkaninfo --summary`; a vkBridge session
   silently measures the bridge instead of our driver.
3. Run `tools/bench/suite.sh --seconds 15 --reps 3 --panel-hz <hz> --out bench/suite.jsonl`.
   Every run has a fixed `--seconds` window and a frame counter; warmup frames are excluded.
4. Report the median of the reps per workload, keeping the rep count and `panel_hz` next to it.
   Do not clamp a `render_fps` above `panel_hz`; that is the expected uncapped result. (The
   README rule "fps > panel_hz + 2 is an instrument error" applies to `displayed_fps` only.)
5. Reject a run whose JSON has `valid:false` or a `present_path` other than `none`; fix the
   cause, never edit the number.
6. Watch temperature: the suite records `temp_before_c`, `temp_after_c`, `cpu_idle_pct` and
   `gpu_busy_pct` (Mali sysfs when readable, otherwise `null`). A throttled second half is
   not the chip's steady state.

## The two metrics

* **`render_fps`** = frames the workload completed inside its measured window / wall seconds.
  It is producer-side, independent of any panel, and is the only performance metric here.
* **`displayed_fps`** = what the display actually showed, from SurfaceFlinger present
  timestamps or an on-screen HUD. It exists only when the workload has a presentation path,
  is a **separate field** in the JSON, and is never substituted for `render_fps` (or the
  reverse). The two tests above have no presentation path, so their records omit it.

Headroom is reported as

    headroom = render_fps / panel_hz

so `1.0` means exactly panel-rate with nothing to spare, `3.0` means the chip could feed a
360 Hz panel at that workload, and `< 1.0` is the only case where the panel is not the limit.
The suite emits `headroom` only when it was given `--panel-hz` (or `MALI_PANEL_HZ`); without a
panel rate the field is `null`, never guessed.

The CPU test also prints `cpu_ms_per_frame` (record + submit, excludes the fence wait),
`record_ms`, `submit_ms` and `bound` (`cpu` or `gpu`, from frame time vs CPU time). Read
`cpu_ms_per_frame` when judging draw-call overhead; `render_fps` alone can be GPU-limited on a
fast host even at 50k draws, and `bound` says which side won.

## JSON line (`schema: mali-bench/1`)

One line per run. Field order is fixed; absent device data is `null`:

```json
{"schema":"mali-bench/1","ts":"2026-11-20T15:00:00+01:00","host":"cmf","workload":"gpu",
 "workload_class":"gpu","rep":1,"metric":"render_fps","render_fps":412.3,"frames":6184,
 "window_s":15.0,"cpu_idle_pct":8.4,"gpu_busy_pct":96.0,"temp_before_c":38.0,
 "temp_after_c":47.0,"panel_hz":120.0,"headroom":3.436,"resolution":"1920x1080",
 "samples":1,"draws_per_frame":null,"cpu_ms_per_frame":0.09,"bound":null,
 "present_path":"none","icd":"/usr/local/share/vulkan/icd.d/mali_icd.json",
 "driver":"mali-own","valid":true,"reason":null,"cmd":"...t_bench_gpu --seconds 15 ..."}
```

`displayed_fps` is added **only** when `present_path` is not `none`. `gpu_busy_pct` comes from
Mali sysfs (`/sys/class/misc/mali0/device/utilisation`, or a devfreq `gpu_busy_percent`, or
`MALI_UTIL`); if none is readable it is `null` and the summary prints `n/a` — it is never
invented. Temperature is the first `thermal_zone` whose type names the GPU/SoC, or
`MALI_TEMP_ZONE`.

## Building and where the numbers hold

`vk/build.sh` builds both tests for every target (`out/<target>/t_bench_gpu`, `t_bench_cpu`);
the `.spv` shaders are committed under `vk/shader/`. On a host without a Mali device they run
against the host ICD (RADV/lavapipe) to prove the workload, the schema and the harness; those
numbers describe the host GPU, not a phone.

Only a device run can confirm the real thing: GPU busy counters and DVFS/throttling from kbase,
the `/sys/class/misc/mali0` layout that actually exists on that ROM, per-draw cost through the
guest ICD (our real `mali_cmd`/`mali_cs_emit` path), and whether the two workloads really split
the chip's limits instead of both hitting the same wall. Treat host numbers as a smoke test,
never as a phone result.
