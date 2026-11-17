# Poco X4 GT (xaga) - Mali-G610 MC6, arch 10 - probe and ladder results

Measured with `probe/probe.c` (bionic build, run as the adb shell user) and the `libkbase/tests` ladder. Wireless adb only.
The probe was run with the secure dma-heaps (`*prot*`, `*svp*`, `*wfd*`, `*region*`, `*tee*`) skipped.

## Device
- Dimensity 8100 (MT6895), Android 14 / HyperOS OS2.0 V816 (vendor build is an Android 12 base), kernel 5.10.209-android12.
- Vendor GLES: `ARM, Mali-G610 MC6, OpenGL ES 3.2 v1.r32p1-01eac0`. Vulkan HAL name `mali`.
- `/dev/mali0` crw-rw-rw- system:graphics, label `gpu_device`. open() works from the shell user (`u:r:shell:s0`).
  Not yet checked from an untrusted-app sandbox (no test app installed). SELinux denials: none visible to the shell user (dmesg/logcat not readable for avc).

## GPU / kbase
- CSF, **uAPI 1.18**, DDK `K:r32p1-01eac0(GPL)`. GLB iface version 0x1010000 (CMF 0x2050000, Poco X8 0x30a0000).
- product_id 43111 (0xA867), gpu_id **0xA8670000**: arch 10.8, rev 6.
- shader_present **0x130013** (6 cores, non-contiguous, highest bit 20 -> core_id_range 21), tiler 0x1, l2 0x1, stack 0x33.
- l2 features raw 0x7130306 4 L2 slices, 512 KiB, 64 B line; mmu 0x2830 (48-bit VA); as_present 0xff; coherency 0x301 (1 group).
- max_threads 2048, max_registers 1024, workgroup 1024, freq max 950000 kHz, available memory ~7.5 GiB (0x1d1e67000).
- CSF: 8 groups, 64 streams (8/group), group0 suspend size 26112, prfcnt size 0x1b. Priorities 0, 1, 2 granted.
- Texture features word0 0xc1fe039e (same mask family as CMF/X8).
- CSF firmware file `/vendor/firmware/mali_csffw.bin` exists; the version string is not readable without root (sysfs `core_mask`, `gpuinfo` are permission-denied for shell).

## Version differences (kbase r32p1 / uAPI 1.18) - recorded in the libkbase caps table (`{1,18}` row in `libkbase/src/ctx.c`) or keyed on arch 10
- **No MEM_ALLOC_EX (nr 59)**: ENOTTY. Legacy MEM_ALLOC (nr 5) works; `caps.mem_alloc_ex = false`. Kernel adds SAME_VA (0x2000), honours COHERENT_LOCAL, drops COHERENT_SYSTEM.
- **QUEUE_GROUP_CREATE is nr 42, 32 bytes** (upstream CREATE_1_6): `caps.group_create_1_6 = true`. nr 58 gives ENOTTY (found with an ioctl size scan).
- DMA_BUF export/import sync_file ioctls: ENOTTY (kernel 5.10), so `clear_poll` cannot pass here. KCPU fence signal -> sync_file works. dma-buf import (UMM) works for `system`, `system-uncached`, `mtk_mm-uncached`.
- Tiler heap init, CS queue register/bind/kick, KCPU fence, CQS wait all work with the 1.21 layouts (no ENOTTY, hardware results correct).
- **Arch 10 (not uAPI)**: no SET_STATE_IMM32; scoreboards are selected with SET_SB_ENTRY. `kb_v10` is now set by `kb_open` from the GPU arch, so every test and the driver emit it. ZSD word 4 bit 21 cleared.
- No /dev/sw_sync, /dev/udmabuf, /dev/ion. `/dev/dri/card0` exists, no renderD128.

## Root cause of the first fault
t_clear/t_triangle faulted with `CS_INVALID_INSTRUCTION` (0x49) at CS offset 0x40 (sideband = ring VA + 0x40): instruction 8 was `SET_STATE_IMM32` (op 28), which does not exist on v10. The vk driver set `kb_v10`, the hand-built tests did not. The fatal event was consumed by the fence wait, hence the empty drain; `kb_last_fatal()` now keeps it.

## Ladder (Poco X4 GT, bionic builds, `/data/local/tmp`)
| rung | result |
|---|---|
| t_submit | pass |
| t_clear (red/grey/asym, 256^2 and 1280x720, 200-clear loop) | pass; only `clear_poll` fails (no dma-buf sync_file ioctl, kernel) |
| t_triangle (flat, varying; hmask 0xfe; 8192/8192 px) | pass |
| t_uniform (scale/offset/colour, two draws) | pass |
| t_depth (D32, D24, clear value) | pass |
| t_texture (checker) | pass |
| ICD `t_vk_draw --loop 20`, `t_vk_compute --loop 5`, `t_vk_meta` (`--direct`, bionic ICD) | pass, device "Mali-G610 MC6 (mali-own)" |
| `t_vk_info`, `t_dxvk_caps` | pass (6/47 DXVK features, 11/13 extensions missing, same harness output as other phones) |
| dEQP daily sample (1381 cases) | **302 Pass / 107 Fail / 971 NotSupported / 1 ResourceError**, via the shell-user route (`vk/deqp/shell-deqp.py`: glibc deqp-vk + sysroot ld.so from /data/local/tmp/g, our ICD behind a `libvulkan.so.1` shim; NOT the MaliDeck guest). Baseline `vk/deqp/ours-daily-sample-poco-x4.tsv`. Against the CMF's pre-feat/ds 282/127/971/1 the only differences are 20 cases Fail on CMF, Pass here (stencil/depth formats fixed by feat/ds, in this run's tree); no case passes on the CMF and fails here. api.smoke 6/6. ~30 s for the whole sample |
| app-sandbox access to /dev/mali0 | MaliDeck p19 installed by the owner but blocked at first run: the storage-permission dialog needs a tap (`pm grant`/`appops` are ignored by this ROM), so the guest never starts. Not tested. Needs DroidDeck (or any app that opens /dev/mali0) installed over adb; device node is 0666 with label gpu_device, so untrusted_app access is expected to work as on the other phones |
