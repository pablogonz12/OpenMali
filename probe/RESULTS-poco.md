# Poco X8 Pro Max (Immortalis-G925 MC11, MT6991) kbase probe results

Raw: `runs/20261115-poco-probe/probe.jsonl` (probe.raw = droiddeckctl wrapper, t_submit.raw = t_submit refusal).
Binary: scripts/poco/out/probe-poco-aarch64 = driver/probe/probe.c + one change (also skip heap names containing "tee";
the stock list would have allocated from `mtk_tee_page-uncached`). Source: scripts/poco/probe-poco.c. Run in the guest, session `steam`,
pkg com.ludashi.benchmark, USB-powered, 30 C.

## Open / context
- /dev/mali0 mode 0666 uid 1000 gid 1003, rdev 10:288 (CMF: 10:105). open() ok from `untrusted_app_27` (uid 10541).
- /dev/dri exists (empty: no card0/renderD128). /dev/kgsl-3d0, /dev/ion absent, /dev/ashmem present, /dev/udmabuf EACCES (same as CMF).

## Driver / GPU
- CSF, **uAPI 1.30** (CMF 1.21). DDK `K:r49p1-03bet0(GPL)` (CMF r44p1). GLB iface version 0x30a0000 (CMF 0x2050000).
- product_id 55344 (0xd830), gpu_id 0xd8300015 (CMF 0xb8a31030, product 47267). Decoded gpu_id: arch 13.8 rev 3 (CMF 11.8 rev 10), product_major 0, version 0.1 status 5.
- shader_present 0xee0057 (11 cores, non-contiguous), tiler 0x1, l2 0x1, stack 0xe7. l2 features 0x8130306 (4 L2 slices, 512 KiB, 64 B line). mem_features 0x301, mmu 0x2830 (48-bit VA), as_present 0xff, tiler_features 0x809.
- max_threads 2048, max_registers 65536 (CMF 1024), workgroup 1024, gpu_freq_max 1612000 kHz (CMF 1047000), available memory 0x2bce95000 (~11.2 GiB; CMF ~7.2).
- Coherency 0x301 (bit 0 ACE-lite + bit 8/9 set); CMF 0x101. 1 coherency group.
- CSF: 8 groups, 64 streams (8/group), group0 suspend size 27712 (CMF 26432), prfcnt_size 0x3c (CMF 0xe). Priorities 0,1,2 all granted.
- Texture features 0xc7fe001e, 0 for words 1-3: identical bitmask to CMF; per-format bits (BC/ASTC/ETC2) are not decoded by the probe, so HW support is unconfirmed; the bridge docs say BC is decoded on upload.

## Memory (13 cases, all ok, all freed)
- Same cases succeed as on CMF (rw, cached, same_va, uncached_gpu, cpu_wr_gpu_rd, gpu_only, grow_on_gpf, gpu_ex va 0x800000001000).
- **Difference: COHERENT_SYSTEM (0x400) is KEPT** (rw_coherent_system flags_out 0x240f, nothing dropped); CMF dropped it. COHERENT_LOCAL kept on both. So this GPU/kernel appears to offer IO coherency (needs a real GPU-visible test before relying on it).
- CPU mapping ns/byte (64 KiB, write/read): uncached 0.05/2.36; cached (CPU_CACHED) 0.012/0.065; coherent_system uncached-ish 0.064/2.61; cached+coherent_system 0.011/0.065. CMF: uncached 0.11-0.18/1.5-2.1, cached 0.02-0.04/0.14-0.27 (Poco CPU ~2-3x faster on cached).

## dma-buf
- /dev/dma_heap: system, system-uncached, mtk_mm, mtk_mm-uncached, mtk_slc (EACCES), plus mtk_prot_page-uncached, mtk_prot_region(-aligned), mtk_sapu_page-uncached, mtk_svp_page-uncached, mtk_tee_page-uncached, mtk_tui_region(-aligned). All secure/TEE/TUI ones skipped (not allocated).
- 4 MiB map ns/byte w/r: system 0.009/0.060, mtk_mm 0.009/0.060, system-uncached 0.048/1.59, mtk_mm-uncached 0.051/2.39.
- MEM_IMPORT (UMM) ok for every variant (rw, rw_shared, rw_shared_coherent_system, gpu_rw_only, gpu_rd_only, rw_shared_sync_on_map) from all 4 safe heaps. Same flags_out as CMF (0x500f; cookie 0x41000); CPU read via the kbase mapping SIGBUSes (signal 7). No export ioctl tested.

## Fence / sync
- KCPU queue + FENCE_SIGNAL gives a sync_file fd that signals within 100 ms. /dev/sw_sync and debugfs sw_sync: ENOENT (as CMF).

## Syscall floor (ns, timer quantum ~77 ns)
- getppid mean 184, p50 231, p99 308. KBASE GET_CONTEXT_ID mean 267, p50 308, p99 308, 0 failures (CMF 462/385/615).

## t_submit (libkbase)
- Refused at open: rc -95, "kbase CSF uAPI 1.30 is not supported by libkbase (known: 1.21)...". Expected; not modified. Porting needs the 1.30 layouts (group create is 112 bytes from 1.25).
