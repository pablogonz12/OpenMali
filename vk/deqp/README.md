# dEQP-VK harness (R3 M3)

- **Build**: `./build.sh` → `out/a64-glibc/external/vulkancts/modules/vulkan/deqp-vk` (+ data dirs next to it). vulkan-cts-1.3.10.0, source fetched
  into `driver/third_party/cts-src` (`external/fetch_sources.py`), clang + the droidtest arm64 glibc sysroot (`a64-glibc.cmake`), headless platform, no
  X11/Wayland/EGL, ninja from `third_party/venv`. Needed workarounds (flags in the toolchain file, CTS source unmodified): `-include cstdint` (libstdc++ 15),
  `-Wno-missing-template-arg-list-after-template-kw` (clang 23). ~25 min on 4 cores, 75 MB binary.
- **Runner**: `scripts/quick/qdeqp <cmf|poco> <list> [--icd ours|panvk] [--chunk 500] [--case-timeout 60] [--limit N] [--keep] [--tag T]`.
  Starts a Steam session (focus check first, waits ≤10 min), pushes binary + data once (stamp) and, for `ours`, `libvulkan_mali.so` to
  `Download/qdeqp/`, runs chunks of `--deqp-caselist-file` in the guest (`droiddeckctl guest` allows ≤600 s per call, so a chunk is capped at 540 s),
  parses the QPA. `panvk` = `$BL_VKBRIDGE_FALLBACK_ICD` (DroidDeck's imported PanVK-kbase). Results append to `runs/qdeqp/<phone>-<icd>-<list>.tsv`
  (kill + rerun resumes); the case after the last finished one is retried alone (twice, with settle time); still dead = `Crash`. Writes `.csv`
  (per 4-level group: pass/fail/NS/crash/other, % of total and of supported) and `runs/quick/*-deqp.json`. Exit 0 only if all pass.
  Guest only; no bionic/direct flavour yet (not needed: slice speed ≈ 10 cases/s).
- **Lists** (`lists/`, `gen.py <mustpass dir>` regenerates them deterministically; counts in `counts.json`): `slice-m3.txt` 6800 (api.smoke 6, info 6113,
  device_init 224, object_management 457), `api-gate.txt` 210488 (M4–M13 gate groups incl. copy_and_blit.core, image_clearing.core),
  `pipeline-gate.txt` 54262 (monolithic vertex_input/input_assembly/depth/blend/… complete, stencil/sampler/multisample 1/10),
  `renderpass-gate.txt` 5544 (suballocation.{simple,formats,attachment} + small groups), `r3-list.txt` 270294 (union; the large files are
  regenerable, do not commit them), `r3-daily.txt` 27030 (every 10th). Full mustpass: api 299698, monolithic 273226, renderpass 15865, renderpass2 30246.
- **Baseline** (Poco, G925/v13, PanVK-kbase, `baseline-poco.csv`/`.tsv`): 6800 cases: 6278 pass, 385 NotSupported, 137 Crash, 0 Fail → 92.3 % of total,
  97.9 % of supported. All crashes in api.object_management.{alloc_callback_fail*, max_concurrent, multithreaded_per_thread_*}. **Open**: the same
  case passes in a manual single-case run, but the runner's retry-alone records Crash; cause not found (suspect state left by the crashed process or the
  extra flags `--deqp-surface-type=fbo --deqp-watchdog=enable`). Treat those 137 as "harness unresolved", not as a PanVK result, until fixed.
- **Ours** (`ours-smoke-poco.csv`, api.smoke 6 cases): 2 pass (create_sampler, create_shader), 4 Crash (draw cases), as expected this early.
- Not done: CMF baseline (`baseline-v11.txt`), two-run 5k determinism check, resume-after-kill test (resume proven by rerun), bionic variant.

## Host dEQP against the fake backend (`host-run.sh`)
Separate, slower gate (not in check.sh). Host x86-64 `deqp-vk` from `third_party/cts-src` in `third_party/build-vk-x86-host`
(`cmake -G Ninja` with `third_party/venv/bin` on PATH, clang, headless, `-DCMAKE_DISABLE_FIND_PACKAGE_Wayland=ON -DCMAKE_DISABLE_FIND_PACKAGE_X11=ON`,
CXX flags `-include cstdint -Wno-error -Wno-missing-template-arg-list-after-template-kw`; ~30 min at `nice -n 15 ninja -j3`).
`./host-run.sh` runs `lists/daily-sample.txt` (1381 cases) on fake GPUs v11 and v13 through the ASan+UBSan driver (`MALI_SAN=1 MALI_TARGET=native-san ./build.sh`),
in batches of 100 under a 300 s timeout (the case in flight at a crash/timeout is recorded and the run resumes after it), and compares only
Crash/Timeout/NotSupported against `host-baseline.tsv`, printing NEW ones by name (exit 1). `--bless` rewrites the baseline. ~2 min for both GPUs.

## Candidate case lists (non-gating)
`gen-caselists.py <mustpass vk-default dir>` regenerates `cases-small.txt` (1531: draw sample, dynamic-state, clipping, fragment-operations, depth; proposed for the gated daily) and `cases-deep.txt` (6884: sampled rasterization/compute/ssbo/ubo/texture/query-pool/memory/..., never a gate). Both are only for `farm submit --suite cases --case ...`; `lists/daily-sample.txt` and `SUITE_CASES` are untouched until every GPU class has a baseline.
