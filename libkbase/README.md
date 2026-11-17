# libkbase (slice 1)
Standalone C11 library (MIT, libc only, no Mesa) for Arm `mali_kbase` CSF at `/dev/mali0`, un-rooted.
Verified on CMF Phone 1 (Mali-G615 MC2, arch 11.8, uAPI 1.21, DDK r44p1). Other uAPI versions are refused.

    include/kbase.h       public API (A context, B memory, C queue, D sync)
    include/kbase_uapi.h  our own kbase/dma-buf UAPI subset (sources cited in the header)
    include/kbase_cs.h    CS instruction encoders (layouts from Mesa genxml v10, study only)
    src/ctx.c bo.c queue.c sync.c
    tests/t_submit.c      JSON-lines test (E1-E4, F)
    ./build.sh [native|aarch64|all]   -> out/t_submit-*  (-std=c11 -Wall -Wextra -Werror -pedantic)

Run on the phone (see driver/probe/README.md for the session recipe):
    adb push out/t_submit-aarch64 /storage/emulated/0/Download/t_submit
    guest: cp ... /tmp/x && chmod +x /tmp/x && /tmp/x [--quick] [--event-thread]

Notes learned (uAPI 1.21):
- Group create is the 40-byte layout (nr 58); 112 bytes only from 1.25.
- MEM_ALLOC_EX returns a cookie; mmap on the kbase fd yields the GPU VA. Free SAME_VA by munmap only.
- Imported dma-bufs: never touch the kbase mapping (SIGBUS); CPU access via the dma-buf's own mmap.
- GPU is not IO-coherent: cached BOs need `dc civac` (kb_bo_cache_flush_inv) both ways.
- KCPU enqueue takes exactly one command per ioctl. CQS wait uses GT with val = target-1.
- Doorbell-only kick works while the group is on a slot; after idle it can be lost. Use the ioctl kick.
- A fatal group error is terminal: terminate the group and create a new one.
- Fatal notifications arrive on read() without CSF_EVENT_THREAD.
- Tiler heaps need MEM_JIT_INIT on the context first (else ENOMEM); kb_open does MEM_EXEC_INIT + MEM_JIT_INIT like PanVK. See PANVK-SEQUENCE.md.

## kbase_fb + t_clear (milestone R1 stretch, verified on CMF Phone 1)
    include/kbase_fb.h src/fb.c   v11 FBD (128 B) + RGB render-target descriptor (64 B) packers, annotated
                                  with Mesa genxml v11 layouts (study only; own code)
    tests/t_clear.c               fragment-only clear of an imported dma-heap buffer, JSON lines
    ./build.sh also builds out/t_clear-*   (opts: --slot N --finish --noclean --tiled --nowait --noflush --n N)
Format: R8G8B8A8, linear block format (2), row stride = width*4, surface stride = stride*height, identity
swizzle (0x688). Clear word r|g<<8|b<<16|a<<24 replicated in the 4 RT clear words (memory bytes R,G,B,A).
Stream: FLUSH_CACHE2 -> REQ_RESOURCE(fragment) -> SET_STATE_IMM32(endpoint SB slot 3) -> RUN_FRAGMENT
-> WAIT(1<<3) -> REQ_RESOURCE(0) -> SYNC_ADD64 (CQS) -> KCPU wait+fence. Staging regs: r40:41 = FBD|1|(rt-1)<<2,
r42 bbox min, r43 bbox max (inclusive).
Hardware findings: geometry-less frame needs NO tiler context/heap (Tiler ptr 0) and NO FINISH_FRAGMENT.
REQ_RESOURCE (op 34) is mandatory: without it CS_EXTRACT stalls at RUN_FRAGMENT with no fault.
Descriptors written uncached by CPU were read stale by the GPU on rapid reuse (loop clear 2 got clear 1's
colour); a synchronous FLUSH_CACHE2 (L2/LSC clean+invalidate, other invalidate) at stream start fixes it.
Results: runs/20261115-cmf-libkbase/clear-h4.jsonl (h1-h3 = the failed bring-up iterations).
Explicit sync: KCPU fence imported as dma-buf WRITE fence; poll(POLLIN) not ready before kick, ready after.

## Triangle bring-up status (t_triangle, hardware runs r7..r17)
PanVK draw trace + pandecode diff: see PANVK-SEQUENCE.md steps 10-12.  Fixed: RUN_IDVS fragment-SRT select, 64-bit register values (scissor, FAU
count) via two MOVE32, SET_STATE endpoint SB re-select before RUN_FRAGMENT, FBD sample locations/tile size/provoking vertex.
Status: the framebuffer clear now lands (r17), the triangle still covers 0 px; next suspects: blend descriptor (w0 bit 11, w3 conversion),
DCD0, varying size.  Tracer: tools/ioctl_trace.c dumps rings, CS chunks and per-kick BO snapshots (.mem); tools/vkdraw.c = identical PanVK draw.

## Poco X8 Pro Max (Mali-G925 MC11, kbase CSF uAPI 1.30, r49p1, arch 13.8) - UNVERIFIED port
Accepted since the 1.30 caps row (src/ctx.c). Layouts come from Arm's public headers (see kbase_uapi.h), nothing has run on the phone yet.
- 1.30 deltas handled: QUEUE_GROUP_CREATE 112 B (nr 58, new ioctl size), TILER_HEAP_INIT 24 B (nr 48); both fall back to the
  1.21 sizes on ENOTTY. The 1.21 path is unchanged. CS_FAULT notifications (1.25) stay off (cs_fault_report_enable = 0).
- Descriptor packers (fb.c, draw.c) are still Mesa genxml v11; on arch 13 they warn once on stderr. TODO(v13): compare v13.xml.
- Run (same recipe as the CMF, driver/probe/README.md): `./build.sh aarch64`, push out/t_submit-aarch64 and out/t_clear-aarch64 to
  Download, in the guest copy to /tmp, chmod +x, run `/tmp/t_submit --quick`, then `/tmp/t_clear`. Try t_submit first: it needs no descriptors.
- Expect to debug: group-create/heap sizes (ioctl error text names the failing call), then the v13 FBD (t_clear), and the dma-heap choice
  (skip any `tee`/secure heap). COHERENT_SYSTEM is kept on this GPU, so the "not IO-coherent" cache-flush rule may be pessimistic.
- First runs 2026-11-16: t_clear PASS; t_triangle (--only 1 and 2), t_uniform, t_depth, t_texture all FAIL the same way: group faults at the
  first draw instruction (cs_extract 0x148 = opcode 0x06 RUN_IDVS, which does not exist on v13; RUN_IDVS2 = 12) with the v11 register map.
  Fault text not captured (notification consumed in the wait path). Details/table: runs/20261116-poco-libkbase/V13-DIFFS.md.
- v13 port run 2026-11-16 (t_triangle_v13, 12 hw runs): RUN_IDVS2 runs without fault, clear lands, flat triangle still 0 px (needs 8192). Fault decode fixed (kb_fence_wait swallowed the notification; the v13 test has its own wait that prints it; 1.30 event layout is fine: TRANSLATION_FAULT decoded when FBD ZS/CRC bit set without extension).
- v13 ladder 2026-11-16: flat + varying triangle and uniform scene 1 PASS on the Poco (t_triangle_v13 --loop 1000: 1000/1000). Varying needs tiler flags bit 18 (default) and the FS compiled against the VS layout. Two draws per frame and depth still fail, texture not ported: see V13-NEXT2.md.
  PanVK reference captured on the Poco (vkdraw 8192 px, runs/20261116-poco-libkbase/panvk-trace/): effective tile 12 in tiler ctx AND FBD, hmask 0, FBD ptr tag 3 + CRC ext, FS FAU words 4,5 = RT conversion; most IDVS regs already identical. Findings and open list: V13-NEXT.md "Poco hardware results".
- Round 2 (7 more hw runs, all negative, 0 px, polylist_hi16 0, state_w0 0): options --zsext (320 B FBD with CRC ext), --cs-tctx (CS-built tiler ctx), --sb-prologue (SET_STATE/NEXT_SB_ENTRY/LOGIC_OP32), --heap-panvk, --panvk-desc, --all-panvk; the stream now matches PanVK draw instructions 4..27. The tiler is initialised (tctx_after shows heap head written) but nothing is binned, so the suspect is VS/position/shader side. See V13-NEXT2.md; raw in runs/20261116-poco-libkbase/v13-runs2/.

## Triangle hardware run 20261116 (CMF Phone 1, Mali-G615, uAPI 1.21 path)
t_clear still passes (0 mismatches). `t_triangle --only 1`: 8192/8192 px (0 % error), flat colour 0xff996633 exact, tiler_state_w0=0x1, polylist_hi16=0x4, submit->signal 2.4 ms.
`--revert 1` (v6 swizzle 0x688 attribute format): 0 px, w0=0x0, hi16=0x0, clear colour only -> the attribute pixel format was the root cause.
Interpolated triangle (`--only 2`): 8192 px, max channel error 0/255 (tol 4), submit->signal ~31 ms (flat in same run ~5 ms; first-run 2.4 ms).
Raw: runs/20261116-cmf-libkbase/triangle-*.jsonl. 4 hardware invocations, no regression on the 1.21 path.

## Rung 2 (uniforms + depth) on CMF, 2026-11-16
PASS: t_uniform (FAU, 1 and 2 draws) and t_depth (D32F, D24X8, clear 0.5) on first hardware run; see R2-NEXT.md section 4.

## Part B: zero-copy pairing trial (CMF, 2026-11-16)
VKBRIDGE_HOST_WAYLAND=/run/droiddeck/wayland-0 (guest XDG_RUNTIME_DIR=/run/droiddeck; compositor add_socket "wayland-0", compositor.c:3911), marker=1, MANAGE_EXTERNAL_STORAGE allow. Env reached session. Compositor log (partB-wayland.log): steam, steamwebhelper, explorer.exe and Megabonk.exe all "bound banner_ahb_v1 version 3 (takes offers)" = host connections work. But NO "paired with offered gralloc buffer" / "presenting ... without a copy"; perf zero-copy 0 throughout; frames = "layer copy" (gamescope dma-bufs copied into the AHB layer, as in marker-ON before). Megabonk 60 s: 23.3 fps, 1% low 11.6, >25 ms 1306, >50 ms 61 (OFF 19.5, ON-layer ~28). Flicker not screencapped. Pairing failures are silent in the patch (zero matches on st_dev/st_ino => return, no log), so offers vs buffer inode cannot be compared from logs; client VKB_INFO lines ("Wayland: ... takes offered") were not found in any collected log. Next: enable bridge client log level or add logging in host_offer/patch pairing to see whether offers are sent and their inode vs gamescope's wl_buffer fd.

## Rung 3 (textured draw) on CMF, 2026-11-16
t_texture PASS on first hardware run: 65536/65536 px exact, 64/64 cell centres, 2D linear RGBA8 texture, stride 64, sampler nearest/clamp/normalized. No encoder changes needed. See R3-NEXT.md section 5. Note: t_triangle without `--only` fails triangle_varying after triangle_flat (0 px); `--only 2` passes.

Note 2026-11-16: full `t_triangle` (flat then varying in one process) passed 5/5 on the CMF, as did the other four tests; the reported
varying failure did not reproduce. See TRIANGLE-NEXT.md.

## Loop harness (2026-11-16)
`t_triangle|t_uniform|t_depth|t_texture --loop N [--loop-reset-ctx] [--loop-max-fail K]`: repeats the body, prints one `loop_summary` line (pass, fail, first_fail_iter, us min/mean/max); failing iterations are replayed with dumps. `scripts/quick/qdrv` honours `QDRV_TIMEOUT` (s). Results and the intermittent t_triangle failure (~0.4 %/iter, gone with `--arena-adv`): see TRIANGLE-NEXT.md.

## Offline check (no GPU, no phone): `./check.sh` (~0.4 s warm, ~2.5 s cold; prints `OK` or only unexplained findings)
    src/backend_fake.c   defines open/ioctl/mmap/munmap (link-time interposition, only in out-fake/*): real libkbase sources run on heap
                         memory with fake VAs (0x5f00000000+, never reused); CS_QUEUE_KICK dumps the ring + all BOs to $KB_FAKE_DUMP, fences are signalled
    tools/decode_stream.py  own CS interpreter (MOVE/CALL/LOAD_MULTIPLE regs) + rules + field diff vs the PanVK golden; `show` prints a stream
    golden/allow-v11.txt  every known/justified difference vs runs/20261115-cmf-libkbase/panvk-trace/draw-flat.jsonl (VA/pointer qwords auto-ignored)
    ./check.sh --mutate idvs38|pixfmt688   re-injects an old bug in the dump only (self-test, must FAIL);  ./check.sh --v13  t_triangle_v13, rules only
Catches: wrong/missing descriptor words, RUN_IDVS flag bits, FAU count (MOVE48 truncation), RUN before WAIT(FLUSH_CACHE2), r44/r45 != 0 at RUN_FRAGMENT,
no SB re-select after REQ_RESOURCE, ring/descriptor alignment (SPD 32, others 64), pointers into freed BOs.
Cannot: anything the GPU/firmware does (timing, cache races such as INTERMITTENT.md, MMU faults), shader binaries, v13 register semantics (no golden yet),
test readback results. Golden diff only for t_triangle; other tests get the static rules. Findings in allow-v11.txt are "works on HW", not "proven equal".

## Intermittent empty draw (status 2026-11-16)
Fix (head FLUSH_CACHE2 wait, rolling arena, dsb st) gives 0 failures in ~16000 direct-shell iterations, but the old stream (`--arena-rewind --no-flush-wait`) also gave 0/3000 there,
so the fix is unproven; needs the same bisect under guest/GPU load. Details: INTERMITTENT.md; data: runs/20261116-cmf-libkbase/intermittent-*.jsonl.
Guest update: reproduced under Steam menu (old 7/3000). WAIT alone 3/1500, rolling arena alone 3/1500, both (default) 0/7500: both are needed. See INTERMITTENT.md.
- v13 round 3 (10 hw runs): PanVK traced doing two-draw / depth / texture (runs/20261116-poco-libkbase/panvk-trace2/); CS, ZSD, DCD0/1, tiler flags identical to ours, HSR bits (`--var 256`) and no-ZS (`--var 128`) change nothing, FBD w12 bits 31:28 (`--var 16`) fault the queue. Two-draw and depth remain open, texture not ported, loops not run: see V13-NEXT2.md.
- v13 round 4 (12 hw runs): **all four v13 tests pass on the Poco** (t_triangle_v13, t_uniform_v13 incl. two draws, t_depth_v13 D32F/D24X8/clear value, t_texture_v13 65,536 px), each 1000/1000 with `--loop 1000` and `--loop 1000 --loop-reset-ctx`; `qdirect poco all` 5/5. The draw-2 and depth failures were one harness bug: the 72 B VS FAU (17 words) sat in a 64 B block, so the FS colour overwrote the VS immediate at word 16 (draws with an even red-float word vanished). Fixed in `tests/rung2_v13.h` (`R2_FAU_BYTES`). Details: V13-NEXT2.md.

## R2: compiled shaders, animation, presentation (2026-11-16)
See [R2-DONE.md](R2-DONE.md): Mesa-compiled rotating triangle (t_compiled) and its on-screen present (t_present_tri, ~117 fps, 0 CPU copies/frame).
