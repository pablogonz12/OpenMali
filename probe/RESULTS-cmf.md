# CMF Phone 1 (Mali-G615 MC2) kbase probe results

Raw: `runs/20261115-cmf-probe/probe-run3.jsonl` (final); run1 = first run (included secure heaps), run2 = intermediate.
Run inside the DroidDeck guest (session `steam`, pkg com.ludashi.benchmark). Kernel 6.1.162-android14, SELinux Enforcing, Nothing/TetrisEEA 16 (BP2A.250605.031.A3).

## Open / context
- /dev/mali0: mode 0666, uid 1000, gid 1003, rdev 10:105. open() O_RDWR succeeds from the app domain.
- SELinux context: `u:r:untrusted_app_27:s0:c232,c256,c512,c768`; uid 10232, groups 3002,3003,9997,20232,50232.
- Plain Android shell: glibc binary cannot exec on bionic ("No such file or directory", no ld-linux). Not run.

## Driver / GPU (type `open`, `version_check_csf`, `gpuprops`, `csf_glb_iface`)
- CSF flavour, uAPI 1.21 (JM variant not tried). DDK `K:r44p1-01eac0(GPL)`. GLB version 0x2050000.
- product_id 47267, gpu_id 0xb8a31030 = arch 11.8 rev 10, product_major 3; revision 1.3.
- shader_present 0x5 (2 cores), tiler 0x1, l2 0x1, stack 0x5. l2_features 0x7120306, mem_features 0x101.
- mmu_features 0x2830 (48-bit VA, 40-bit PA), as_present 0xff, tiler_features 0x809.
- max_threads 2048, max_registers 1024, max_workgroup 1024, gpu_freq_max 1047000 kHz, 1 coherency group.
- CSF: 8 groups, 64 streams (8/group), group0 suspend size 26432, prfcnt_size 0xe.
- Priorities 0,1,2 all granted (PRIORITY_CHECK).

## Memory (`mem_alloc`, 13 cases, all ok, all freed)
- MEM_ALLOC_EX works for: rw, cached_cpu, coherent_system, coherent_local, same_va, uncached_gpu, cpu_wr_gpu_rd, gpu_only, grow_on_gpf, gpu_ex (EXEC_VA, va 0x800000001000), and cached variants.
- Kernel adds SAME_VA (0x2000) to all non-EXEC allocs; result is a cookie (0x41000) mapped via mmap on the kbase fd.
- COHERENT_SYSTEM (0x400) is silently dropped => no IO coherency. COHERENT_LOCAL (0x800) kept.
- CPU mapping cost, 64 KiB, ns/byte (write / read):
  - uncached: 0.11-0.18 / 1.5-2.1
  - cached (BASE_MEM_CACHED_CPU): 0.02-0.04 / 0.14-0.27

## dma-buf (`dma_heap_alloc`, `udmabuf`, `dmabuf_import`)
- /dev/dma_heap: system, system-uncached, mtk_mm, mtk_mm-uncached, plus secure/firmware heaps (mtk_prot_*, svp, wfd, sapu).
- Safe heaps all allocate 4 MiB (O_RDONLY open). Direct mapping ns/byte w/r: system 0.015/0.126, mtk_mm 0.017/0.127, system-uncached 0.080/1.42, mtk_mm-uncached 0.084/1.46.
- /dev/udmabuf: EACCES. /dev/ion, /dev/kgsl-3d0 absent. /dev/ashmem present.
- kbase MEM_IMPORT (UMM) accepts every variant (rw, rw_shared, rw_shared_coherent_system, gpu_rw_only, gpu_rd_only, rw_shared_sync_on_map) from every safe heap => zero-copy import is viable.
  - flags_out gains NEED_MMAP|CACHED_CPU (0x500f); cookie 0x41000 then mmap on kbase fd gives GPU VA; MEM_QUERY flags 0x3c0100f, 1024 pages.
  - CPU read via the kbase mapping of an import SIGBUSes (signal 7): CPU access must use the dma-buf's own mmap.
- NOTE: run1 (not run3) allocated and imported from secure heaps before the allow-list was added; freed cleanly, nothing misbehaved. Run3 skips them.

## Export of native allocation
- No ioctl for exporting a kbase allocation as dma-buf in the uAPI headers used; not verified on r44p1.

## Fence / sync
- KCPU queue create/delete ok; FENCE_SIGNAL enqueue returns a sync_file fd (`anon_inode:sync_file`) that signals within 100 ms.
- /dev/sw_sync and debugfs sw_sync: ENOENT.

## Syscall floor (run3, ns; timer quantised ~77 ns)
- getppid: mean 232, p50 231, p99 308.
- KBASE GET_CONTEXT_ID: mean 462, p50 385, p99 615 (10000 calls, 0 failed). Run1: mean 477, p99 692.
- Laptop x86-64 getppid ~98 ns for comparison.
- Guest process: Seccomp 2 (2 filters), TracerPid 0. libproot.so is the parent but not ptracing; the extra syscall cost is seccomp-filter overhead, not tracing.

## Definitions
- No fixes needed to kbase_uapi_min.h for uAPI 1.21. Probe logic fixed: cookie same_va semantics, MEM_QUERY on mapped address, free-by-munmap, secure-heap skip.
- Not implemented: queue groups, submit, tiler heaps, TLSTREAM, PRFCNT, KCPU MAP_IMPORT/FENCE_WAIT, USER_BUFFER import.
