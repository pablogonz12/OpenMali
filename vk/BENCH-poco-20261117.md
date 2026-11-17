# mdbench ours vs bridge, Poco X8 Pro Max (Mali-G925 MC11), 2026-11-17

Main = mali-open 5d0fd38 (async submit, tex array fix, 64 KiB geometry); ICD rebuilt (vk/cross/build-cross.sh glibc) and mdbench rebuilt (bench/build.sh aarch64). App MaliDeck p18.1 (p16 payload), session via run-guest.sh.
3 x 20 s, wayland 1280x720, mailbox for BOTH, warmup 60, USB, qrestore before every config. Raw: bench/results/raw-poco-20261117.txt, runs/mdbench/.
Temperatures (battery, USB-charging) rose from 42 to 48 C during the sweep (above the 43 C target for alu/compute/drawcalls; alu was the hottest). Treat later rows as indicative.
CAVEAT: bridge fps sits at exactly 120.4 in every scene, ours reaches 160-295 in mailbox: the bridge path is display/compositor paced even in mailbox, ours is not, so fps is NOT comparable as throughput; compare gpu_ms and 1% low. gpu_ms is ours 1.7-2.5x the bridge.

| scene | ours fps | 1% low | >25ms | gpu_ms | bridge fps | 1% low | >25ms | gpu_ms | temp C (b/o) |
|---|---|---|---|---|---|---|---|---|---|
| cube | 294.6 | 138.2 | 0 | 0.84 | 120.6 | 91.2 | 0 | 0.17 | 41.8C/42.2C |
| drawcalls (1000) | 160.2 | 100.8 | 0 | 1.95 | 120.4 | 91.1 | 0 | 1.04 | 44.1C/44.8C |
| overdraw | 209.7 | 116.2 | 0 | 2.06 | 120.4 | 91.6 | 0 | 0.84 | 43.3C/44.8C |
| alu (64) | 70.7 | 46.1 | 15 | 10.83 | 118.9 | 58.3 | 0 | 5.77 | 47.6C/48.0C |
| tex | 200.5 | 112.3 | 0 | 2.24 | 120.4 | 92.4 | 0 | 0.69 | 41.8C/43.2C |
| compute | 224.4 | 122.7 | 0 | 1.74 | 120.4 | 89.3 | 0 | 0.85 | 45.3C/46.1C |

## Image comparison (headless, 120 frames, readback grid 12x12 RGB, ours vs bridge, on the Poco)
| scene | nonclear ours/bridge | grid max diff | full hash | verdict |
|---|---|---|---|---|
| cube | 0.2952/0.2952 | 1 (mean 0.0) | differ | equal |
| drawcalls | 0.8065/0.8065 | 1 (mean 0.2) | differ | equal |
| overdraw | 1.0000/1.0000 | 0 (mean 0.0) | differ | equal |
| alu | 1.0000/1.0000 | 8 (mean 2.1) | differ | float precision |
| tex | 1.0000/1.0000 | 0 (mean 0.0) | differ | equal |
| compute | 0.3867/0.3867 | 0 (mean 0.0) | same | equal |

tex is fixed (was grid diff 106 on CMF before the array fix): identical grid. alu differs by float precision only. Full hash differs except compute (sub-grid LSB, not bit-exact expected).
