---
name: mali-open
description: Work on mali-open, the from-scratch open Vulkan ICD for Arm Mali GPUs (libkbase + vk/ front end, Mesa compiler linked as a library). Use for ANY mali-open or Mali driver work - building, check.sh, the farm, dEQP triage, worker briefs, merging, phone tests. Load first; it holds the worker rules, lanes, merge gate and known pitfalls.
metadata:
  opencode/autoinvoke: false
---

# mali-open

Open Vulkan 1.3 ICD (`libvulkan_mali.so`, C11, MIT) on top of the vendor `mali_kbase` kernel driver. Layers: `libkbase/` (ioctls, BOs,
CSF queues), `vk/src/mali_*.c` (front end), `mali_backend.h` (backends `kbase` and `fake`), per-arch code compiled for v11 and v13.
Compiler = Mesa 26.3 as an unmodified library. PanVK is an oracle only: never copy its code. Goal: DXVK titles on Mali phones.
Read README.md, ROADMAP.md, vk/R4-GAP.md for status. Main checkout: `~/Documents/scripts/mali-open`; work in your own worktree.

## Devices (aliases only; serials live in ~/.config/mali-farm/devices.toml, never in the repo)
| alias | GPU | class | state |
|---|---|---|---|
| cmf | Mali-G615 | v11 | always attached; USB, wireless adb as fallback |
| poco-x8 | Mali-G925 | v13 | owner's daily phone; USB, wireless adb as fallback; tested ONLY while on a charger (on battery: untouched, jobs stay pending); owner switch: `farm disable poco-x8` |
| poco-x4 | Mali-G610 | v10 | always attached, wireless adb only; driver bring-up in progress |

## Rules for every worker
1. No build lock: `vk/build.sh` writes only to your worktree's `vk/out`.
2. Every local test runs under `timeout 120`. Never `adb kill-server`.
3. One agent per phone; the qsafe lock arbitrates. PHONE BUSY: do offline work, never retry in a loop.
4. Phone tests go through the farm, not hand-run adb: `tools/farm/farm submit <ref> --suite smoke|daily --need v11,v13`, then
   `farm wait <job> --timeout N` / `farm results <job>`. Only committed state is tested. Exit codes: 0 pass, 1 fail, 2 error, 3 timeout.
5. Loops run in chunks of at most 4 min, results appended to a file, resume from it. Two chunk timeouts in a row: stop and report.
6. Offline first; hardware only for the final check. Write status docs before the last hardware step.
7. 0 fps in the Steam menu is not a freeze: judge by load, memory, responsiveness.
8. Battery below 20 %: stop testing that phone until it is back at 50 %. Thermal limit (battery 43 C or thermal status SEVERE): stop and
   let it cool to 38 C. The farm and qsafe enforce both; never bypass them.
9. Final report at most 150 words. Record a lesson with `tools/lesson` when you learn something non-obvious.

## Commands (from vk/)
- `./build.sh` -> `out/native/{libvulkan_mali.so,mali_icd.json,t_*}`; `./check.sh [--no-build] [-v]` offline gate, prints OK or FAILED.
- Single tests: `tests/build-t_*.sh`, run as `MALI_BACKEND=fake MALI_FAKE_GPU=v11|v13 timeout 120 out/native/t_x --direct out/native/libvulkan_mali.so`.
- a64 cross build: see `vk/cross`; it must be warning-free.
- Features are advertised only when a test backs them (`t_vk_info`, `t_dxvk_caps`, `t_vk_feat`, `t_vk_draw`).
- `tools/skill-lint.sh` checks this skill; `tools/lesson <area> "<text>" <evidence>` appends to LESSONS.md.

## Lanes (one worker at a time in the hot files)
- Exclusive: `vk/src/mali_cmd.c`, `mali_cs_emit.c`, `mali_pipeline.c`. Ask the orchestrator before touching them if another worker holds one.
- Parallel lanes: meta/BC, descriptors, libkbase, tests, dEQP triage.

## Merge gate (all must hold)
1. Rebased on main. 2. `vk/check.sh` OK. 3. a64 build warning-free. 4. Farm `daily` on v11: no new failures.
5. A new test per feature. 6. v13 farm run if the Poco is attached, otherwise tag the commit `v13-pending`.
7. Local fast-forward only. Pushing is the orchestrator's job.

## Brief template
```
Goal: <one sentence>   Worktree/branch: <path> / <branch>   Lane: <area>
Paths: <files to read/edit>   Done when: <check + test + farm job>
Out of scope: <...>   Report: <=100 words + file:line, then the JSON line
```

## Report template (last line)
`{"branch":"","commit":"","check":"ok|fail","farm":"<job>|none","v13":"ok|pending|fail","new_tests":[],"open":""}`

## Pitfalls (max 20; promote only items hit twice or costing over 30 min)
1. An A/B result is worthless unless commit, binary sha256, env and method are recorded (the head flush 0x330 result could not be reproduced).
2. CMF benchmark timings are bimodal (about 1.6 vs 2.5 ms): use interleaved runs and medians, never single runs.
3. The roadmap listed inlineUniformBlock as open when it was already on main: check `t_vk_info`, `t_dxvk_caps` and the code before implementing.
4. Test binaries built from a stale tree gave a false failure: record the commit a binary was built from and rebuild after rebasing.
5. Scripts hardcoded to one phone: take the alias or class as a parameter.
6. A hung fake-backend test once blocked everyone before timeouts existed: always `timeout 120`.
7. "Builds warning-free" offline is not verification: only a test (fake or farm) backing the feature counts.
8. A fixed DXVK_MISSING in check.sh may only be lowered together with a passing test for the new feature.
