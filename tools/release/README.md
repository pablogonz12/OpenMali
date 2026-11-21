# Release builds for phones

Two scripts, one rule: a phone build comes from a clean worktree of `origin/main` (or another merged
ref), never from a working tree with uncommitted edits and never with an unmerged branch bundled in.

```sh
tools/release/build-release.sh                 # clean worktree of origin/main -> zip + manifest
tools/release/build-release.sh --ref <commit>  # a merged ref (tag, commit), never a lane branch
tools/release/verify-guest.sh <serial>         # check the running guest against the newest manifest
```

## build-release.sh

1. `git fetch origin main`, resolve `origin/main` (or `--ref`) and add a **detached clean worktree**.
   The untracked `third_party/` tree of the main checkout is linked into it (Mesa is not vendored, and
   the symlink is ignored by git; nothing is committed from the release worktree).
2. Build the `a64-glibc-wsi` variant against `third_party/build-vk-a64-glibc-wsi` (Wayland + X11
   platforms). The zip therefore has `libxcb.so.1` in `NEEDED` and advertises all four surface
   extensions: `VK_KHR_wayland_surface`, `VK_KHR_xcb_surface`, `VK_KHR_xlib_surface`,
   `VK_EXT_headless_surface`.
3. Run the full host gate (`vk/check.sh`). A failing gate stops the release; no zip is produced.
4. Package with `vk/package/build-zip.sh` and verify the zip as a phone would receive it: checksums,
   aarch64 ELF, the three loader exports, no `libelf`, the four surface names, `libxcb.so.1` in
   `NEEDED`.
5. Build the aarch64 probes `t_vk_release` (instance/device extensions, driverInfo, device name) and
   `t_dxvk_caps`, and write `manifest.json`.
6. Print the zip path and its sha256.

Output (`--out`, default `tools/release/out/<date>-g<commit>/`, ignored by git):

```
mali-open-a64-glibc-wsi-<YYYYmmdd>-g<commit>.zip   deterministic name (date + commit)
manifest.json                                      commit, date, feature flags, checks, missing list
probe/t_vk_release, probe/t_dxvk_caps              aarch64, pushed by verify-guest.sh
check.log, build.log, dxvk-v11.log, dxvk-v13.log
```

Environment: `MESA_SRC` (default `<main checkout>/third_party/mesa`), `MESA_BUILD` (host gate tree,
passed to `vk/check.sh`; unset means "let `vk/build.sh` pick"), `MALI_A64_MESA_BUILD` (default
`third_party/build-vk-a64-glibc-wsi`). If the a64 tree is missing, run `vk/cross/build-cross.sh
glibc-wsi` in the main checkout once.

## manifest.json

| Field | Meaning |
|---|---|
| `commit`, `commit_short`, `ref`, `date`, `built_at` | the exact source the zip was built from |
| `zip`, `zip_sha256` | artifact and its hash (also printed by the script) |
| `library`, `library_sha256` | `libvulkan_panfrost.so` inside the zip; what the guest must run |
| `driver_info`, `driver_info_sha256` | the exact `driverInfo` string and its sha256 |
| `surfaces` | the four surface extensions verified in the binary |
| `feature_flags` | platforms and the `MALI_*` switches that were set at build time |
| `checks` | `vk/check.sh`, packaging checks, both `t_dxvk_caps` runs |
| `dxvk_missing.v11` / `.v13` | the `t_dxvk_caps` missing list per GPU generation |
| `dxvk_present.v11` / `.v13` | the advertised DXVK features/extensions |
| `needed` | the zip library's `NEEDED` list |

## verify-guest.sh

```sh
tools/release/verify-guest.sh <serial> [--manifest FILE] [--ctl PATH]
```

Finds the `libvulkan_mali`/`libvulkan_panfrost` that a session process maps inside the guest (falls
back to installed copies), pushes `probe/t_vk_release` and `probe/t_dxvk_caps`, and compares:

* the library sha256 with `library_sha256`;
* the reported `driverInfo` and its sha256;
* the four advertised surface extensions;
* the `t_dxvk_caps` feature list (present + missing) with the manifest's v11/v13 lists.

Exit 0 = the guest runs exactly the manifest build, 1 = mismatch, 2 = setup problem (no adb, no
session, missing probes). It is read-only and needs `droiddeckctl`
(`~/Projects/mali-gaming/tools/droiddeckctl`, override `DROIDDECKCTL`) plus a running session.

## Rules

* Every phone build goes through `build-release.sh`; do not hand-zip `vk/out/*`.
* Never bundle an unmerged branch, lane or local patch into a phone build. Only `origin/main` or a
  merged ref is packaged; the manifest records the commit so a phone can be traced back to it.
* Keep `third_party/`, `vk/deqp/` and the release output out of commits. The release worktree is
  removed on exit (`--keep` keeps it for debugging).
