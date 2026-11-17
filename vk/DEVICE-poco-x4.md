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

## uAPI differences found (all handled in libkbase, caps row 1.18)
- **No MEM_ALLOC_EX (nr 59)**: ENOTTY. The legacy MEM_ALLOC (nr 5) works; kernel adds SAME_VA (0x2000) and honours COHERENT_LOCAL, drops COHERENT_SYSTEM.
- **QUEUE_GROUP_CREATE is nr 42, 32 bytes** (upstream CREATE_1_6), nr 58 gives ENOTTY. Found with an ioctl size scan.
- DMA_BUF export/import sync_file ioctls: ENOTTY. KCPU fence signal -> sync_file works. dma-buf import (UMM) works for `system`, `system-uncached`, `mtk_mm-uncached`.
- No /dev/sw_sync, /dev/udmabuf, /dev/ion. `/dev/dri/card0` exists, no renderD128.

## Ladder (libkbase tests, hand-built, v10 packers)
| rung | result |
|---|---|
| t_submit (open, props, BO alloc, queue/group create, NOP/MOVE/STORE/SYNC, 100 submits, fault decode, teardown) | **pass**, after the two uAPI fixes above. Kick latency p50 146 us. |
| t_clear (256x256 RGBA clear, FBD + RUN_FRAGMENT) | **fails**: fence returns EIO, queue faults at CS offset 0x40 of 0x80; no notification was read within 300 ms |
| t_triangle | fails: queue faults at CS offset 0x128 of 0x240 (same class) |
| t_uniform / t_depth / t_texture | not run (depend on the fragment path) |

Next: capture the fault (non-fatal CS_FAULT/exception code via the event queue with a longer wait or `--event-thread`), then diff the v10 FBD/SB handling against the v11 hand-built one (V10-BRINGUP.md items 1-2).
