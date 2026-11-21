---
name: mali-open
description: Work on mali-open, the from-scratch open Vulkan ICD for Arm Mali GPUs (libkbase + vk/ front end, Mesa compiler linked as a library). Use for ANY mali-open or Mali driver work - building, check.sh, the farm, dEQP triage, worker briefs, merging, phone tests. Load first; it holds the worker rules, lanes, merge gate and known pitfalls.
metadata:
  opencode/autoinvoke: false
---

# mali-open

Open Vulkan 1.3 ICD (`libvulkan_mali.so`, C11, MIT) on top of the vendor `mali_kbase` kernel driver. Layers: `libkbase/` (ioctls, BOs,
CSF queues), `vk/src/mali_*.c` (front end), `mali_backend.h` (backends `kbase` and `fake`), per-arch code compiled for v10, v11 and v13.
Compiler = Mesa 26.3 as an unmodified library (pin: `third_party/MESA-SOURCE.txt`). PanVK is an oracle only: never copy its code.
Goal, in order: the Steam menu in stock DroidDeck on our driver, then a game launched from it, then performance.
Status lives in README.md, ROADMAP.md and vk/R4-GAP.md. Main checkout: `~/Documents/Scripts/mali-open` (never edit it; use a worktree).

## Two kinds of work, two rule sets
- **Offline driver work** (code, fake backend, host tests, docs): this file is enough.
- **Anything that touches a phone, a deploy, a PR merge or the `integration` branch** is a mali-orchestrator lane: read
  `~/.config/opencode/mali-orchestrator/WORKER.md` first. It wins over this file. Its tools (`M/bin`: `ddc`, `smoke`, `deploy`,
  `lane-wt`, `publish`, `pr-gate`) are the only way to a phone; never hand-run `adb` or `droiddeckctl`.

## Devices (aliases only; serials are private and never enter the repo)
| alias | GPU | class | notes |
|---|---|---|---|
| x8 | Mali-G925 | v13 | owner's daily phone; a tool that answers REFUSED/BUSY means the owner is using it: do host work |
| cmf | Mali-G615 | v11 | test phone |
| x4 | Mali-G610 | v10 | test phone; driver bring-up, Steam does not start yet |

Older docs and the farm call them `poco-x8`, `cmf`, `poco-x4`.

## Rules for every worker
1. No build lock: `vk/build.sh` writes only to your worktree's `vk/out`. One private worktree per job; commit when something works.
2. Every local test runs under `timeout 120`. Never `adb kill-server`.
3. **The farm (`tools/farm`) is stopped** and stays stopped until the orchestrator says otherwise: it ran unsupervised and
   panicked a phone. Do not start `mali-farm.service`, do not `farm submit`. Read-only `farm digest|status|results` are fine.
4. The test that counts is the product path: stock DroidDeck, Steam menu, a game (`smoke <phone>` in a lane). `vkcube`, dEQP and
   offscreen benchmarks are diagnostics: each use names the product-path failure it explains.
5. Offline first; hardware only for the final check, and only in a lane. Loops run in chunks of at most 4 min, results appended to a
   file. Two chunk timeouts in a row: stop and report.
6. Label every finding PROVEN (evidence quoted: log line, file:line, number + reps), HYPOTHESIS (name the deciding test) or REFUTED.
   A host simulation or a source read is a hypothesis about the phone until one phone run confirms it.
7. Before investigating, search what is already known: `grep -l <topic> ~/.config/opencode/mali-orchestrator/notes/*` and the
   Refuted section of `mali-orchestrator/LESSONS.md`. Long findings go to that `notes/` directory, never to `/tmp`.
8. Battery below 20 % or battery 43 C / thermal SEVERE: the tools refuse; never bypass them.
9. Final report at most 150 words. Record a lesson with `tools/lesson` when you learn something non-obvious.

## Commands (from vk/)
- `./build.sh` -> `out/native/{libvulkan_mali.so,mali_icd.json,t_*}`; `./check.sh [--no-build] [-v]` offline gate, prints OK or FAILED.
- Single tests: `tests/build-t_*.sh`, run as `MALI_BACKEND=fake MALI_FAKE_GPU=v11|v13 timeout 120 out/native/t_x --direct out/native/libvulkan_mali.so`.
- a64 cross build: see `vk/cross`; it must be warning-free. The phone artefact is `tools/build-artefact.sh` (orchestrator only).
- Features are advertised only when a test backs them (`t_vk_info`, `t_dxvk_caps`, `t_vk_feat`, `t_vk_draw`). Never advertise a
  capability the hardware lacks (`geometryShader` on Valhall) without a design decision from the orchestrator.
- `tools/skill-lint.sh` checks this skill; `tools/lesson <area> "<text>" <evidence>` appends to LESSONS.md.
- Offline checks, goldens, rules, sanitizers, host dEQP: `.opencode/skills/mali-open/OFFLINE.md`.

## Hot files (one worker at a time)
- Exclusive: `vk/src/mali_cmd.c`, `mali_cs_emit.c`, `mali_pipeline.c`, `mali_wsi.c`. Ask the orchestrator before touching one.
- Parallel: meta/BC, descriptors, libkbase, tests, dEQP triage.

## Merge gate (all must hold)
1. Branch based on `main`, PR based on `main`; merge `origin/main` in when it conflicts, never rebase-and-force.
2. `vk/check.sh` OK and the a64 build warning-free. 3. A new test per feature or fix, failing without it.
4. `pr-gate <n>` GREEN (CI, CodeRabbit on the head sha, 0 open threads, every review-body finding answered).
5. For present, WSI, allocation or kbase memory: a phone run from an `integration` build that contains the PR head.
6. Search open PRs before opening one. Pushing is `publish <worktree>`; merging is the orchestrator's job.

## Brief template
```
Goal: <one observable outcome>   Rung: 1 menu | 2 game | 3 numbers   Worktree/branch: <path> / <branch>
Known: <proven facts with file:line; the notes file read>   Paths: <files to read/edit>
Done when: <the one command and the output that proves it>   Out of scope: <...>
Report: <=100 words + file:line, then the JSON line
```

## Report template (last line)
`{"outcome":"done|partial|blocked","branch":"","commit":"","check":"ok|fail","pr":"","new_tests":[],"evidence":"","next":""}`

## Pitfalls (max 20; promote only items hit twice or costing over 30 min)
1. An A/B result is worthless unless commit, binary sha256, env and method are recorded (the head flush 0x330 result could not be reproduced).
2. CMF benchmark timings are bimodal (about 1.6 vs 2.5 ms): use interleaved runs and medians, never single runs.
3. The roadmap listed inlineUniformBlock as open when it was already on main: check `t_vk_info`, `t_dxvk_caps` and the code before implementing.
4. Test binaries built from a stale tree gave a false failure: record the commit a binary was built from and rebuild after rebasing.
5. Scripts hardcoded to one phone: take the alias or class as a parameter.
6. A hung fake-backend test once blocked everyone before timeouts existed: always `timeout 120`.
7. "Builds warning-free" offline is not verification: only a test (fake or farm) backing the feature counts.
8. A fixed DXVK_MISSING in check.sh may only be lowered together with a passing test for the new feature.
9. Notes kept in `/tmp` were lost to every later worker when the work moved to another machine: findings go to the notes directory.
10. A status line that outlives its refutation misleads everyone downstream: when a hypothesis is refuted, fix the status doc in the same edit.
