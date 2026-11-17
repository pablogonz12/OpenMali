# R2 done: Mesa-compiled shaders, animated, on screen (CMF Phone 1, Mali-G615, v11, 2026-11-16)

## What
- `compiler-spike/rot.vert` (rotation + offset from a push constant) + `var.frag`, compiled offline by Mesa: `compiler-spike/gen-compiled.sh`
  -> `libkbase/tests/shaders_compiled_v11.h` (reproducible, byte-identical on regeneration). VS FAU: push block at byte 32 = (cos, sin, ox, oy).
- `tests/compiled.h`: shared render engine (`cmp_setup/cmp_render/cmp_verify/cmp_xf_frame`), N frames in flight, 256x256.
- `tests/t_compiled.c`: ref_identity, compiled_identity, compiled_vs_reference, compiled_rotated, animation.
- `tests/t_present_tri.c`: same engine into a ring of dma-heap buffers, presented over Wayland (guest).

## Results
- Reference = t_triangle's shaders (the repo has no hand-assembled blobs). compiled_identity == reference bit-exactly (FNV 0xbcf93685, 0 bad).
  compiled_rotated: area error 0.012 %, max channel error 1.
- Animation 600 frames, ring 4 depth 1, all verified: submit->fence mean 613 us, p99 856 us. Ring 6 depth 3: 600/600 verified, < 0.72 ms/frame.
- Present (ring of 4, 10 s, no CPU wait, fence attached to the dma-buf): ~116-117 fps committed and presented (panel 120 Hz), 0 discarded,
  0 errors, 0 "no free buffer". Frames 0/60/... CPU-verified in run 1 (all in_bad = out_bad = 0).
  Screenshot: `runs/20261116-r2-present/run1/shot2.png` (rotating gradient triangle, HUD ~119 fps).
- Copies per frame: 0 CPU. GPU render writes the dma-buf directly. Then: gamescope samples it in its composite (GPU pass; our buffer is
  256x256, not output size, so no scanout pass-through), then the DroidDeck compositor does 1 GPU blit into the screen swapchain
  ("dma-buf: copied into the screen swapchain", app log `copy 1086, zero-copy 0` per 10 s). Total 2 GPU passes, 0 CPU copies.
  Zero-copy needs banner_ahb_v1 v3 + AHB-backed buffers (not done).

## Visibility fix (see PRESENTATION-path.md "Not shown")
gamescope assigns a client's appId from a `reaper SteamLaunch AppId=N` ancestor. `tools/fake-reaper.c` (`reaper SteamLaunch AppId=769 -- prog`)
is enough to get the toplevel focused and shown (appId 769 = the Steam UI's, so it lands on top of Big Picture).

## Run
    driver/compiler-spike/gen-compiled.sh        # regenerate shaders
    driver/libkbase/check-compiled.sh            # offline stream check (no GPU)
    driver/libkbase/build-compiled.sh all        # out-static/t_compiled (bionic) + out/*-aarch64 (guest, t_present_tri)
    scripts/quick/qdirect cmf t_compiled [--only 4 --ring 6 --depth 3]
    scripts/quick/qpresent <outdir> 769 --seconds 10 [--verify]   # CMF only; leaves session up: finish with scripts/quick/qrestore cmf
(`droidtest/tools/cc-aarch64-glibc tools/fake-reaper.c -> out/reaper-aarch64` is needed by qpresent.)
Hardware runs used: 4 of 14 (2 t_compiled, 2 present; one extra qpresent call aborted on the focus guard without touching the phone).
