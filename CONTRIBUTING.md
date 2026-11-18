# Contributing to mali-open

Thanks for helping. Everything below can be done on an ordinary x86-64 Linux machine: no Mali GPU or phone is needed for the offline gate that CI runs. Phone results (dEQP, hardware passes, benchmarks) come from the maintainer's own devices.

## 1. Build and run the offline checks

### Prerequisites

Packages (Debian/Ubuntu names, as in `.github/workflows/ci.yml`): `build-essential pkg-config git bison flex glslang-tools binutils python3-venv python3-dev libdrm-dev libelf-dev libudev-dev libzstd-dev libexpat1-dev zlib1g-dev libvulkan-dev vulkan-tools`, plus LLVM/clang/libclc/SPIRV-LLVM-Translator dev packages and `spirv-tools` (CI uses the LLVM 18 set). Other distros: the same libraries under their own names.

### One-time Mesa setup

The driver links Mesa's runtime and compiler as libraries. Mesa is pinned (`third_party/MESA-SOURCE.txt`: tag `mesa-26.3.0`, commit `0a6fa60abb737d26ac8a316a4aad7c9ddf64f9e4`) and is **not committed**: `third_party/` does not exist in a fresh clone, so create it first.

```
mkdir -p third_party && cd third_party
python3 -m venv venv
venv/bin/pip install meson==1.12.1 ninja==1.13.2 mako==1.4.3 pyyaml==6.0.3 packaging
git clone --depth 1 --branch mesa-26.3.0 https://gitlab.freedesktop.org/mesa/mesa.git mesa
git -C mesa rev-parse HEAD        # must print the commit above
export PATH=$PWD/venv/bin:$PATH
meson setup build-vk mesa -Dbuildtype=release -Dgallium-drivers= -Dvulkan-drivers=panfrost -Dtools= \
    -Dglx=disabled -Dplatforms= -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled \
    -Dopengl=false -Dllvm=enabled -Dvalgrind=disabled -Dlibunwind=disabled -Dpanfrost-rust=false \
    -Dmesa-clc=enabled -Db_ndebug=false
ninja -C build-vk src/panfrost/vulkan/libvulkan_panfrost.so
cd ..                             # back to the repository root: the commands below are relative to it
```

This is the same recipe as `vk/README.md` and CI. The PanVK target is built only because it produces the static libraries we link; no PanVK code ends up in our library (`vk/check.sh` verifies that). Do not commit anything under `third_party/`. The first build takes a while (about 1000 compile steps).

### Build and test

```
vk/build.sh                  # native x86-64 build -> vk/out/native/{libvulkan_mali.so,mali_icd.json,t_*}
vk/check.sh                  # the offline gate: builds, then runs every fake-backend case, prints OK or FAILED
vk/check.sh --no-build -v    # re-run without rebuilding, with each case's output
MALI_SAN=1 vk/check.sh       # fake-backend cases with ASan+UBSan driver objects (vk/out/native-san); skips the standalone vk/tests/build-t_*.sh tests (incl. the compiler test), so it does not replace the plain gate
libkbase/check.sh            # libkbase offline tests (not run by CI)
vk/cross/build-cross.sh all  # aarch64 glibc and bionic cross builds (need cross toolchains / the Android NDK; not run by CI)
```

`MALI_BACKEND=fake MALI_FAKE_GPU=v11|v13` selects the fake backend that `check.sh` uses: loader, caps, compile, descriptors and command-stream bytes versus goldens, for a Mali-G615 (v11) and a Mali-G925 (v13). Always wrap hand-run test binaries in `timeout 120`; a hung fake-backend test once blocked everyone.

Host tests worth knowing:

- `vk/tests/build-t_compile.sh [-v]`: the shader compiler test (seconds).
- `vk/check.sh --deqp-host`: also runs host dEQP-VK against the ASan fake-backend ICD (about 2 minutes, skipped if the host deqp build is absent).
- Command-stream goldens live in `vk/tests/golden/`. If your change legitimately alters them, run `vk/check.sh --bless`, review `git diff vk/tests/golden` and commit it with the change that caused it.
- `vk/check.sh --bless` also runs `vk/tests/compile-guard.sh --bless`, which records the current shader-compile source hash and `MALI_COMPILE_VERSION` in `vk/tests/compile-guard.txt`. If your change alters the shader-compile sources, bump `MALI_COMPILE_VERSION` in `vk/src/mali_compile.c` before you run `--bless` (otherwise stale shaders stay in users' caches), and commit the updated guard record with the goldens.

## 2. Gates a change must pass

CI (`.github/workflows/ci.yml`) runs, on every PR:

1. `tools/private-lint.sh`: fails if a tracked file holds a private identifier (adb serials, `/home/<user>` paths, private IPs, MACs, personal emails, key or token material). Do not put any of those in code, docs or commit messages.
2. `tools/skill-lint.sh`.
3. `python3 tools/progress.py --check`: `docs/progress.svg` must be current. Run `python3 tools/progress.py` and commit the result if you changed tests, dEQP results or `docs/progress.toml`.
4. `vk/check.sh` against the pinned Mesa.

Run 1 to 4 locally before you push. Rules of thumb that come from past breakage:

- A new feature needs a test that fails without it (fake backend, or a hardware-only test that skips until the feature is advertised). "Builds warning-free" is not verification.
- `DXVK_MISSING` and `ZINK_MISSING` in `vk/check.sh` may only be lowered together with a passing test for the newly advertised feature.
- Rebuild after rebasing: a binary built from a stale tree gives false results.
- Docs-only and tools-only changes still need lint and the progress check.

Maintainer-side gates (not available to outside contributors; the maintainer runs them on the phone farm before merging code that touches the driver): v13 smoke, v13 daily and v10 smoke on hardware, with no new daily failures beyond known flaky cases; the log must show the own driver ("mali-own"). If your PR changes driver behaviour, say so in the description and expect the maintainer to run these. Tools-only PRs need only the smoke.

## 3. Pull request flow

1. Fork and branch from fresh `origin/main`. Use a separate git worktree so your main checkout stays untouched:
   `git fetch origin && git worktree add -B <branch> ../mali-open-wt/<name> origin/main`
   then `cd ../mali-open-wt/<name>` and do all remaining steps there (not in the original checkout).
   The new worktree does not contain the ignored `third_party/` directory that `vk/build.sh` needs (Mesa build tree and venv): run the one-time Mesa setup from section 1 inside this worktree before the gates.
2. Keep the change small and in one area. Commit messages say what and why; no private paths or device serials.
3. Run the gates in section 2 locally.
4. `gh pr create --base main` (or open the PR on GitHub). Describe what changed, what test covers it, and what was not run (for example hardware).
5. CodeRabbit reviews the PR. Fix valid findings; for each one you reject, reply with a one-line reason. Recurring patterns are collected in the maintainers' lessons list, so check your change for the usual ones: when you add a case to one per-format switch, grep its sibling switches; close or terminate open blocks before concatenating partial logs; parse ISO timestamps with a real parser, not by slicing; re-read persisted state before a time-gated decision; clear dedupe markers when you cancel the item they guard.
6. Merge is rebase-only (`gh pr merge --rebase`), done by the maintainer once the gates pass. Keep your branch rebased on main; do not merge main into it.

## 4. Hot files and `patches/`

`vk/src/mali_cmd.c`, `vk/src/mali_cs_emit.c` and `vk/src/mali_pipeline.c` are **hot files**: they are touched by almost every draw-path change, so only one contributor at a time may edit them, and that contributor is assigned by the maintainer. Do not edit them in an ordinary PR.

If your feature needs a change in a hot file:

1. Make everything that does not need the hot file as a normal change: extension and feature tables, the non-hot sources, and a **test that skips until the feature is advertised**.
2. Put the hot-file part in `vk/patches/<name>.patch` (a plain `git diff` against current `origin/main`, applicable with `git apply` from the repo root) next to `vk/patches/<name>.NOTE.md`. (The maintainer's in-flight lanes keep the same pairs in a `patches/` directory next to their worktrees; older docs such as `vk/STEAM-GAPS.md` refer to that location. In a PR, use `vk/patches/`.) The note says: what the patch does, which files it touches, why it is correct, which test covers it, what must be run on hardware before merging, and the follow-up steps after applying (for example `vk/check.sh --bless`).
3. Say in the PR that the patch is waiting for the hot-file owner. The owner applies it, re-runs the gates and commits it.

Examples: `vk/patches/*.patch`, and `vk/STEAM-GAPS.md` for how pending patches are tracked. Patches go stale as the hot files move (the two files in `vk/patches/` no longer apply to current main), so regenerate against fresh `origin/main` before sending. Before sending a patch, test that it applies and that `vk/check.sh` is OK with it applied, then revert it so your PR stays patch-only.

## 5. Phones and the device farm

Not needed for offline work. If you do have a Mali phone: `tools/farm/README.md` describes the farm (`farm hold <dev> --max <min> -- <cmd>` and `farm wait` for phone access; no lock files or sleep loops). Never touch a session you did not start, and never `adb kill-server`. Hardware numbers belong in committed results (`bench/`, `vk/deqp/`), produced by the farm, not pasted by hand.

## 6. Licensing

The project is MIT (`LICENSE`; new files carry `SPDX-License-Identifier: MIT`). Contributions are accepted under the same licence. Do not copy code from Mesa or other projects into the tree: Mesa is linked as an unmodified library. See `LICENSE-NOTES.md` for the third-party inventory.
