# Frame profiler: is this frame GPU-bound or CPU-bound?

`mali_fprof.c` prints one line per frame with the CPU cost of the submit, an **estimate** of the GPU busy time of
that frame and the present interval, so a session can say "GPU-bound" or "CPU-bound" with numbers instead of a
guess. Instrumentation only: with `MALI_FRAME_PROF` unset the code is a branch on a `false` flag (no ioctl, no clock
read, no allocation), and `tests/t_frameprof.c --off` fails if anything is printed or written.

## Why an estimate: what kbase gives us

kbase has no GPU busy counter and no per-job start/end timestamps. What it has is
`KBASE_IOCTL_GET_CPU_GPU_TIMEINFO` (nr 50), wrapped by `libkbase/kb_timeinfo()`:

| field | meaning on this kbase (uAPI 1.21 / 1.30, our `kbase_uapi.h`) |
|---|---|
| `in.request_flags` | `MONOTONIC` (bit 0) and `TIMESTAMP` (bit 1); both requested in one call. No cycle-count flag exists in this uapi. |
| `out.sec`, `out.nsec` | `CLOCK_MONOTONIC_RAW` of the CPU at the ioctl |
| `out.timestamp` | GPU timestamp counter, ticks, period `1/kb_timestamp_hz` (`timestampPeriod` in Vulkan terms) |
| `out.cycle_counter` | zero: it is only filled when a cycle-count flag is requested, and this uapi has none |

Both are **global to the GPU, not per context**; there is no queue/job association. The counter is a *clock*: it runs
with the GPU clock whether the GPU is executing or idle, so a difference of two samples is elapsed GPU time, never
"busy time". `kb_timestamp_hz()` calibrates it against `CLOCK_MONOTONIC_RAW` (the fake backend reports `ts_hz = 0`
and its counter is `CLOCK_MONOTONIC_RAW` in ns, which the profiler reads as ns).

## What is measured

Printed per frame (`MALI_FRAME_PROF=1` -> stderr, `MALI_FRAME_PROF=<path>` -> appended to that file):

```
FRAMEPROF f=17 submit_us=142.3 gpu_ms=6.42 est=sat depth=1 present_ms=16.71 frame_ms=16.90 bound=gpu
```

* `submit_us` — CPU time inside one submission (`mali_cmd_submit`: CS build + kick). It does *not* include the wait
  for an older in-flight frame (`hw_retire`), so it is the CPU's own work, not the frame's CPU wall time.
* `frame_ms` — wall time between two frame kicks (submit-to-submit): the rate the app actually runs at.
* `present_ms` — interval between two present-shaped submits. The driver sees no present entrypoint; it counts a
  submit with **no command buffers, at least one wait and at least one signal**, which is what `vk_common`'s
  `QueuePresentKHR` submits through `driver_submit`. A fence-only submit has no waits and is not counted.
* `depth` — how many earlier frames the GPU had *not* retired at the kick (read from the CS retire counter at the
  kick, so it is a lag-free observation).
* `gpu_ms`, `est` — the estimate below.
* `bound` — `gpu` when `depth > 0`, else `cpu`.

## The estimate

Two cases, chosen by `depth` at the kick:

| `est` | when | formula |
|---|---|---|
| `idle` | `depth == 0`: the GPU had retired everything, so this frame started immediately | `retire_ts - kick_ts` |
| `sat` | `depth > 0`: older frames were queued, so the GPU was saturated and consecutive retires tile the GPU timeline | `retire_ts - previous_retire_ts` |

Both are clamped to `[0, frame_ms]`. `retire_ts` is the GPU timestamp at the moment the **CPU observes** the frame's
retire counter move: at the next frame's `job_begin`, or at `hw_wait` when the driver has to block.

## Accuracy, honestly

* It is a **sampled estimate**, not hardware busy time. No tiler/compute/vertex split is visible and no segment
  boundary inside a frame exists anywhere in the data: a frame is one submission and one retire.
* The counter advances while the GPU is idle, so every span is an **upper bound** on the work inside it.
* The retire timestamp is the CPU's observation, so with the GPU idle at the kick (`est=idle`) it can be up to one
  frame late: `est=idle` values are an upper bound (and get clamped to `frame_ms`). `est=sat` values are unbiased in
  the mean, because the same observation lag applies to both retires that are subtracted.
* `bound=gpu` (queued work at the kick) is the reliable part: it only needs the retire counter, read at the kick.
* A vsync/mailbox wait on the compositor looks like idle GPU time, so a present-paced app can read `bound=cpu`
  while it is really display-limited. Cross-check with `present_ms` against the refresh period.
* If a frame is split over several submissions (indirect draws force a drain, see `mali_cmd_submit`), it shows as
  several frames; `frame_ms` stays correct, but per-frame `gpu_ms` is then per submission.
* On the **fake backend** the retire counter only advances when something waits on it, so host runs show
  `est=sat` / `bound=gpu` almost always: those numbers prove the plumbing, not a machine's bottleneck.

What would make this a real busy time: per-job start/end timestamps from kbase (none in this uAPI), or firmware
counters via the tiler heap / GPU register reads (not exposed un-rooted). Until then, the pair
(`bound`, `gpu_ms` vs `frame_ms`) is the strongest statement the driver can honestly make.

## Reading it

```
tools/frameprof <log>                 # medians/p99 of frame_ms, gpu_ms, submit_us, present_ms + bound split
MALI_FRAME_PROF=/tmp/fp.log <app>     # on the phone: a path the app can write (or =1 for stderr -> logcat)
tools/frameprof /tmp/fp.log
```

On the phone, `MALI_FRAME_PROF=1` lines go to the process's stderr (logcat `I/`/`E/` per the runner); a path under
the app's own writable directory is easier to pull. `MALI_PROF=1` still prints the older per-stage averages.

Test: `vk/tests/t_frameprof.c` (fake backend, both GPU generations): line count, field presence, `0 <= gpu_ms <=
frame_ms`, monotonic `f=`, one summary, a measured present interval, and `--off` = no output at all.
