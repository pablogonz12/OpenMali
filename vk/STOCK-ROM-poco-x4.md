# Poco X4 GT (MT6895, Mali-G610 MC6) on stock HyperOS 2.0.3 / Android 14: final stock-ROM results, 2026-11-17
Archived before the planned bootloader unlock and LineageOS flash (wipes the phone). Other files: `DEVICE-poco-x4.md` (probe facts, ladder, farm commands), `deqp/ours-daily-sample-poco-x4.tsv` (per-case results).

## Driver (feat/v10, arch 10)
- Ladder through our ICD: `t_vk_feat --spv-dir`, `t_vk_draw`, `t_vk_compute`, `t_vk_meta`, `t_vk_info`, `t_dxvk_caps` all pass (shell user, /dev/mali0).
- Shell-route dEQP: `api.smoke` 6/6; daily sample (1381 cases, ~30 s): 302 Pass / 107 Fail / 971 NotSupported / 1 ResourceError. No v10-only failures; 20 stencil/depth cases pass here that fail on the CMF (feat/ds).

## MediaDeck p20 bootstrap on a clean phone (the p17-p19 pin bug)
- p19 on this phone: `RUNTIME_INSTALL_FAILED`, `Cannot download proot` (no pack pins).
- p20 (versionCode 20, same key): `start steam` installs the runtime (ubuntu-r1-rc5, 291,553,163 B, via the gh-proxy retry after the direct URL) and the packs; `runtime.installed: true`, version ubuntu-r1-rc5.
- Pack evidence: no "no pack and none is installed" warning and no "install failed"; the session log names the pinned pack dir `files/addons/proot/6d3844bb...e78a96/libproot.so (add-on pack)` and DirectAudio relay starts from the pack. Download success is not logged by the app (it only logs failures).

## Guest tests: NOT run
- The Steam session starts (`session pid`, proot from the pack) but exits within 0.1 s (`session ended: 0`, `VkBridge server exited with status 137`), `droiddeckctl state` goes back to IDLE, `guest` answers `NO_ACTIVE_SESSION`. Log icd line shows `freedreno_icd.json`, so the p20 runtime picks the Adreno ICD on this Mali phone. `start desktop` goes to INSTALLING_RUNTIME (needs the desktop pack). So sandbox `/dev/mali0`, valrun and guest dEQP were not measured.

## Install findings
- Host `adb install -r` of p20 works after the owner's Install-via-USB change; `input keyevent` is still denied (rc 255) at that moment.
- Before that, shell-side `pm install -r` from /data/local/tmp worked without a tap (recipe in quick/README.md).

## Guest session (p21) — root cause found, session stays up
- Cause: a system-wide stack-sampling trace (Traceur, `traced_perf` running) makes Android send Bionic's profiling signal (RT 36, `libc: sh: received profiling signal with si_value: 1` for every new process, even adb-shell `sh`/`cut`). glibc guest processes have no handler and die: session.log shows `droiddeck-session: line 301: ... Real-time signal 2  gamescope` (proot `-v 3` changed timing enough to see it). proot's status 0 is real: vpid 1 died, proot exits 0.
- Fix (p21): guest starts under `/bin/sh -c "trap '' 36; exec ..."`. Result: session stays up (STARTING_STEAM running for 50 s), `vkbridge PASS ... Mali-G610 MC6, Vulkan 1.1.177`, zero RT-signal deaths. Not a kernel/proot limitation.
- Sandbox `/dev/mali0`: `crw-rw-rw- 1000 1003 10,113`, openable from the guest (exit 0).
- NOT run: valrun smoke and guest dEQP (the guest route uses the bridge; the feat/v10 glibc ICD still has to be pushed). Shell route stays 302/107/971/1.
- Leftover: none (override consumed, env cleared, prefs removed, app force-stopped, screen off).

## Guest tests on main 0645400 (MediaDeck p21, 2026-11-17)
Glibc ICD built from main (`MESA_BUILD=third_party/build-vk-a64-glibc MALI_TARGET=a64-glibc vk/build.sh`), qsafe lock held, app force-stopped and screen off afterwards, max battery temp 38.3 °C.
- Sandbox `/dev/mali0`: `crw-rw-rw- 1000 1003 10,113`, opens from the guest.
- valrun smoke (guest, `PHONE=poco-x4`): t_vk_compute pass, t_vk_meta pass, **t_vk_draw FAIL**: `t_vk_draw.c:487 r == VK_SUCCESS, graphics pipeline = -1000012000` (VK_ERROR_INVALID_SHADER_NV). Draw pipeline creation on v10 at main needs a look. (valrun.sh pushes with the raw serial, which fails on this wireless phone; run from a copy that uses `quick.py serial`.)
- Guest dEQP daily sample (`qdeqp poco-x4 daily-sample.txt --icd ours`, 1381 cases, 106 s): **301 Pass / 121 Fail / 958 NotSupported / 1 ResourceError, 0 Crash**.
- Shell route baseline (feat/ds tip): 302 / 107 / 971 / 1. Differences guest vs that baseline: 14 NotSupported->Fail (`pipeline.monolithic.blend.dual_source.*`; main now advertises dual-source blending, the old baseline did not), 1 Pass->Fail (`image_clearing.core.clear_color_attachment.single_layer.a2r10g10b10_unorm_pack32`), 1 Fail->NotSupported. So the guest-vs-shell gap is the driver version (main vs older tip), not the route.
- Shell route re-run with the same main ICD stopped at 521/1381: on those cases 12 Crash in the shell harness (guest: 6 Pass, 6 NotSupported). Not a full comparison.
- Why the guest died before (p19/p20): see above; while a system stack-sampling trace (Traceur, `traced_perf`) is active Android sends RT signal 36 to every new process, fixed in p21 by ignoring it in the guest.
- Source of that trace: Traceur (`com.android.traceur`) "stack samples", started from the phone UI (its MainActivity was opened ~15:25); none of our scripts start it (they only run bounded `perfetto` traces). `traced_perf` stays resident but idle, signals stopped after the trace ended. CMF: no `traced_perf`. X8: not reachable (adb offline) at the time.
