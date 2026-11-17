# Poco X4 GT (MT6895, Mali-G610 MC6) on stock HyperOS 2.0.3 / Android 14: final stock-ROM results, 2026-11-17
Archived before the planned bootloader unlock and LineageOS flash (wipes the phone). Other files: `DEVICE-poco-x4.md` (probe facts, ladder, farm commands), `deqp/ours-daily-sample-poco-x4.tsv` (per-case results).

## Driver (feat/v10, arch 10)
- Ladder through our ICD: `t_vk_feat --spv-dir`, `t_vk_draw`, `t_vk_compute`, `t_vk_meta`, `t_vk_info`, `t_dxvk_caps` all pass (shell user, /dev/mali0).
- Shell-route dEQP: `api.smoke` 6/6; daily sample (1381 cases, ~30 s): 302 Pass / 107 Fail / 971 NotSupported / 1 ResourceError. No v10-only failures; 20 stencil/depth cases pass here that fail on the CMF (feat/ds).

## MaliDeck p20 bootstrap on a clean phone (the p17-p19 pin bug)
- p19 on this phone: `RUNTIME_INSTALL_FAILED`, `Cannot download proot` (no pack pins).
- p20 (versionCode 20, same key): `start steam` installs the runtime (ubuntu-r1-rc5, 291,553,163 B, via the gh-proxy retry after the direct URL) and the packs; `runtime.installed: true`, version ubuntu-r1-rc5.
- Pack evidence: no "no pack and none is installed" warning and no "install failed"; the session log names the pinned pack dir `files/addons/proot/6d3844bb...e78a96/libproot.so (add-on pack)` and DirectAudio relay starts from the pack. Download success is not logged by the app (it only logs failures).

## Guest tests: NOT run
- The Steam session starts (`session pid`, proot from the pack) but exits within 0.1 s (`session ended: 0`, `VkBridge server exited with status 137`), `droiddeckctl state` goes back to IDLE, `guest` answers `NO_ACTIVE_SESSION`. Log icd line shows `freedreno_icd.json`, so the p20 runtime picks the Adreno ICD on this Mali phone. `start desktop` goes to INSTALLING_RUNTIME (needs the desktop pack). So sandbox `/dev/mali0`, valrun and guest dEQP were not measured.

## Install findings
- Host `adb install -r` of p20 works after the owner's Install-via-USB change; `input keyevent` is still denied (rc 255) at that moment.
- Before that, shell-side `pm install -r` from /data/local/tmp worked without a tap (recipe in quick/README.md).
