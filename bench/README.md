# bench2: triangle + cube, ours vs the Mali bridge, inside the DroidDeck guest

Only two benchmarks, both run INSIDE the guest (Linux arm64 / glibc a64 build under proot, started like a game via
`reaper SteamLaunch AppId=769`, Wayland WSI through gamescope). Never native Android binaries.

    tools/bench2 <alias|class> [--driver ours|bridge|both] [--bench triangle|cube|all] [--sweep] [--reps 5] [--secs 8] [--wait N]
    tools/bench2 --report        # regenerate RESULTS.md from results.jsonl

Flow per phone: `--sweep` (one short run per settings combination, writes `best.json` + `sweeps.jsonl`), then the final run (default,
5 interleaved reps of the stored best presented + offscreen config per driver) which appends rows to `results.jsonl` and rewrites
`RESULTS.md`. Both steps are resumable: calls stop after `--budget` s (default 230, rule: chunks of at most 4 min) with
`CHUNK DONE`; run the same command again (finished runs are cached in `raw/`, untracked, keyed by phone + commit + .so sha).
Phone access: qsafe lock (`/tmp/qsafe-<lock>.lock`, busy = pending, nothing is retried in a loop; `--wait N` = one blocking flock like
qsafe), owner disable switch, battery 20/50 % and 43/38 C gate (`tools/farm/gate.py`), focus rule (launcher/settings only), farm adb routing.
poco-x4 has no guest route (shell route only): it stays pending until it has one. Nothing is installed on a phone.

## Workloads (`mdbench/`, one binary, a vendored copy of the mission repo's mdbench + `--draw-idx`)
* **triangle** (CPU-heavy): `--scene drawcalls --n 3000 --draw-idx 3`: 3000 draws per frame, one triangle each, a dynamic-UBO descriptor
  offset per draw. Headline: CPU ms/frame (record + submit, mean), also fps and GPU ms.
* **cube** (GPU-heavy): `--scene cube --layers 8 --iters 24`: a rotating textured cube plus 8 full-screen overdraw layers plus a
  24-iteration ALU fragment shader, depth on. `cube-720p` = fixed 1280x720 window, `cube-native` = compositor size. Headline: GPU ms/frame
  (timestamps), fps, 1 % low, frames over 50 ms.
* Each runs presented (wayland, 120 Hz cap) and offscreen (headless 1280x720, no cap). Compare drivers on CPU ms and GPU ms, not on capped fps.

## Settings swept (rows record the winner)
Present mode mailbox/fifo/immediate x swapchain images 2/3/4, frames in flight 1/2/3. Ours additionally env sets (`MALI_RING`+`MALI_GEOM_SIZE`,
`MALI_HF=0 MALI_NOTF=1` (unvalidated), `MALI_NO_MERGE=1`). The bridge exposes no knobs in the guest beyond the above.
Ranking: triangle presented = fps then CPU ms, offscreen = CPU ms; cube presented = 1 % low then fps, offscreen = GPU ms.

## Known limits (open driver items; the harness records them, it does not hide them)
* Frames over ~3000 draws used to fail with `QueueSubmit = VK_ERROR_DEVICE_LOST`: the 32 MiB descriptor arena (~4.6 KB/draw) ran out. Fixed on `perf/draw-path` by retiring in-flight frames and spilling into overflow BOs (test: `t_vk_draw --only many_draws`, 3k-50k draws; phone re-check pending).
* Host (fake v11, 3000 dyn-UBO draws, median of 7): 1.43 us/draw before, 0.72 after descriptor sharing between consecutive draws.
  `tools/bench2 <phone> --variant triangle-20k` runs the large one once the driver can (the bridge handles it).
* **OPEN DRIVER BUG: `VK_ERROR_DEVICE_LOST` at QueueSubmit above ~3000 draws/frame (ours, CMF).** Repro (needs the phone lock, so qsafe):
  `tools/bench2 cmf --driver ours --variant triangle-20k --reps 1 --secs 8` (guest command: `mdbench --scene drawcalls --n 20000 --draw-idx 3`).
  Bisect: n=3000 ok presented+offscreen; 4000 fails offscreen; 6000 fails offscreen; 8000+ fails both. Bigger `MALI_RING`/`MALI_GEOM_SIZE` do not help.
* CMF timings are bimodal: medians of interleaved runs only. The bridge's presented fps is display-paced (about 120): compare CPU/GPU ms.
* poco-x8 runs MaliDeck p19 (sessions sometimes die early; p21 fixes it but is not installed). A run that fails is retried only via the cache/rerun.

## poco-x4 (v10, MediaDeck p22 guest) caveat, 2026-11-17
The `ours` cube rows on poco-x4 (GPU 0.09-0.17 ms, 650-3800 fps vs bridge ~7.3 ms) are NOT valid speed: the sweep winner env `MALI_RING=67108864+MALI_GEOM_SIZE=67108864` makes the cube draw (almost) nothing on v10. The default env of the same sweep gives cube-720p ~104 fps / 7.9 ms GPU (bench/sweeps.jsonl, poco-x4 ours cube-720p), on par with the bridge. Triangle-3k rows are plausible (ours cpu ~4.6 ms vs bridge 0.46 ms). The sweep should reject winners whose GPU ms is far below the default's.
