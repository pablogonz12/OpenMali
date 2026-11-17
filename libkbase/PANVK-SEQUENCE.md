# What PanVK (Mesa kbase backend, DDK r44p1, uAPI 1.21, Mali-G615) does for a first draw
Source: `driver/tools/ioctl_trace.c` (LD_PRELOAD) on `driver/tools/vkdraw.c` (256x256 triangle, vertex+fragment, copy to buffer).
Raw: `runs/20261115-cmf-libkbase/panvk-trace/vkdraw-trace{1,2,3}.jsonl` (trace3 also has ring buffers, CALLed CS chunks and descriptors).
Result on the phone: 8192 px covered = analytic area (128*128/2), so the trace is of a working draw.

1. open /dev/mali0 twice (first fd = device probe: handshake, props, glb iface; second fd = the real context, same calls).
2. Context: VERSION_CHECK_CSF(1.21) -> SET_FLAGS(0) -> mmap 4 KiB PROT_NONE @off 0x3000 (tracking page) -> GET_GPUPROPS (size query,
   then 723 B) -> **MEM_EXEC_INIT{0x1000 pages}** (nr 38) -> **MEM_JIT_INIT{va 0x2000000, max_alloc 255, trim 0, group 0, phys 0x2000000}**
   (nr 14, REQUIRED for tiler heaps) -> CS_GET_GLB_IFACE x2 (count query, then fill) -> mmap 4 KiB PROT_READ @0x2f000 (glb page).
3. Small BOs via MEM_ALLOC_EX (nr 59, 64 B, out = cookie, mmap @0x41000 gives the SAME_VA address) + MEM_SYNC (nr 15) for CPU-written ones.
   Flags seen: 0x380f (CPU-cached RW, GPU RW, coherent-local, SAME_VA) for descriptors/rings, 0x280f for uncached, 0x1817 for
   code, 0x82000f for the CSF event/sync page; sizes 4 KiB..64 KiB, one 10 MiB (0x280f), one 512 KiB.
4. Three queue groups (CS_QUEUE_GROUP_CREATE_1_6, nr 42, **32 bytes**: tiler_mask 1, fragment_mask ~0, compute_mask ~0, cs_min 1,
   prio 0, tiler_max 1, fragment_max 64, compute_max 64; out group_handle 0,1,2). The group exists BEFORE the heap.
   Per group: MEM_ALLOC_EX 64 KiB ring (0x280f) -> CS_QUEUE_REGISTER{ring va, size 0x10000, prio 0} -> CS_QUEUE_BIND{va, csi 0}
   (out mmap_handle 0x30000) -> mmap 12 KiB @0x30000 (USER_IO: input/output pages).
5. After queue 3's mmap: two MEM_ALLOC_EX 4 KiB + 0x10000-byte BOs, then **CS_TILER_HEAP_INIT** (nr 48, 16-byte layout):
   in = chunk 0x100000, initial 10, max 400 (0x190), target_in_flight 0xffff, group_id 0, pad 0;
   out gpu_heap_va 0x6000000000, first_chunk_va 0x6000002000. A second heap is created on a worker thread right away
   (out 0x6000000040 / 0x6000a02000: heap contexts are 64 B slots, chunks follow each other). No HEAP buffer-descriptor BO involved.
6. Then command-buffer memory (4 KiB x2, 512 KiB, 10 MiB, 264 KiB), one MEM_ALLOC_EX exec BO (flags 0x1817) for the shaders.
7. Submit: no KCPU/ioctl for the draw. Each queue ring (64 KiB) contains a prologue (SET_STATE/…, "1c" ops) and a CALL (op 0x20) of a CS
   chunk (0x300..0x3f0 bytes) built in the command buffer; then CS_QUEUE_KICK{ring va} x3, one per queue. Wait = poll CQS/sync page.
8. Tiler context (RUN_IDVS r40:41 and FBD words 14:15) is a 0x5fff36e340 block that is ZERO at kick time: the vertex CS fills it itself
   (LOAD_MULTIPLE from the context state + STORE_MULTIPLE 16 regs, then HEAP_SET etc.). FBD pointer carries tag 3 (ours: 1).
   Fragment: r40:41 = FBD|3, r42 = 0, r43 = 0x00ff00ff, r58:59 = tiler ctx va, then RUN_FRAGMENT (op 7), FINISH_FRAGMENT.
9. Teardown order not traced (process exits).

## Draw stream (decoded with pandecode from runs/20261115-cmf-libkbase/panvk-trace/draw-{flat,var}.{jsonl,mem}; tracer snapshots BOs per kick)
10. Vertex queue chunk (776 B): MOVE48 r24 / r40 = tiler ctx; build the 16-reg tiler context in registers (w2 = 0x400fe: hmask 0xfe | bit 18
    first provoking vertex; w3 = 0x00ff00ff; heap = [ctx_state+48]) and STORE_MULTIPLE it; NEXT_SB_ENTRY/SET_STATE sb masks;
    HEAP_OPERATION vt_start; IDVS staging regs; RUN_IDVS flags 0x42_00000000 = malloc enable (bit 33) + FRAGMENT SRT SELECT (bit 38);
    FINISH_TILING; SYNC_ADD64 (progress); FLUSH_CACHE2 clean L2/LSC. HEAP_SET sits in the ring prologue (d92 = heap ctx 0x6000000000).
    No REQ_RESOURCE in the chunk (prologue has one per queue: vertex 0x0c = tiler|idvs, fragment 0x02, compute 0x01).
11. IDVS regs: r0 varying SRT, r4 fragment SRT, r8:9 / r12:13 varying / fragment FAU as 64-bit (count in bits 63:56: 4 / 5), r16/r18/r20 SPDs,
    r42 = 0, r43 = 0x00ff00ff (scissor min/max), r44 = 0, r45 = 1.0, r48 varying size 0x10, r50 blend|1, r52 ZSD, r56 tiler flags 0x470008,
    r57 DCD0 0x0481000b (CCW, no early ZS update), r58 DCD1 0x1ffff.
12. Fragment queue chunk: SYNC_WAIT64 on the vertex queue counter, RUN_FRAGMENT with r40 = FBD|3 (CRC extension), r42/r43 bbox, r58:59 tiler ctx.
    FBD: w4:5 sample-location table (PanVK 4 KiB BO, pattern 0 = 0x8000,0x8000), w11 = 0x04005480 (32x32 tile, alloc 4096, first provoking vertex),
    w12 = 0xf0210000, w14:15 tiler ctx.  Blend: w0 = 0xa00 (enable | round to FB precision), w3 = conversion 0x000ed000.

### libkbase fixes found by the diff (hardware run log r7..r17, runs/20261115-cmf-libkbase/triangle-r*.jsonl)
- RUN_IDVS must set Fragment SRT select (bit 38).  r43/FAU registers: kb_cs_move48 drops bits 63:48, so the scissor max (r43) and the FAU
  counts were lost; they need two MOVE32.
- IDVS r44/r45 (depth clamps) alias FRAGMENT TEM_POINTER; zero them before RUN_FRAGMENT.
- Same-queue draw+fragment: re-issue SET_STATE_IMM32(endpoint SB) after REQ_RESOURCE(fragment). Without it RUN_FRAGMENT writes NOTHING (not
  even the clear); with it the clear lands (r17: outside pixels 0x80402010).
- FBD: sample-locations pointer, 32x32 tile size with hmask 0xfe, first-provoking vertex in FBD w11 bit 14 and tiler ctx w2 bit 18.
- STILL OPEN (r17): clear is written, triangle covers 0 px. First remaining differences vs PanVK: blend w0 bit 11 / w3 (ours 0x000bb688
  = attribute pixfmt, PanVK 0x000ed000), DCD0 (ours 0x0480002b), r48 (ours 0x20 vs 0x10).
