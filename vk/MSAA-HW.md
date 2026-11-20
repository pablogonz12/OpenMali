# 4x MSAA on real hardware (Mali-G925, v13): evidence

Tested through the farm (hold, X8 only, battery temp stayed below 43 C). Tree: `integ/msaa-hw` (a scratch branch, not a PR) =
main + #187 (advertise 4x only behind `MALI_EXPERIMENTAL_MSAA=1`, merged) + #191 (`vkCmdClearAttachments` in 4x passes, open) +
#182 (`t_dxvk_replay`, open). a64-bionic build.

## Correctness (`MALI_EXPERIMENTAL_MSAA=1`)
| test | result |
|---|---|
| `t_vk_meta --only resolve` | pass |
| `t_vk_draw --only msaa_resolve4` | pass (area 8192, edge/quarter px 256) |
| `t_vk_draw --only msaa_sample_shading` | pass |
| `t_dxvk_replay` (48 pipelines, 4x every 5th) | ok, 300 frames |
| `t_dxvk_replay` without the env | FAIL: `vkEndCommandBuffer` returns -8, then the vk runtime asserts (rc 134): the test expects 4x to be usable; without the gate it must not be used. Gate-off behaviour is by design. |

No GPU fault was seen with the gate on. Kernel dmesg is not available to the farm; judged from test output only.

## Cost (1080p, wall ms/frame = record + submit + GPU + fence, 60 frames, 2 reps)
Workload: N overlapping triangles in one render pass, then (4x) an average resolve to 1x. Timing case: `t_vk_draw --only msaa_cost`
(`MALI_BENCH_DRAWS`, `MALI_BENCH_FRAMES`), in `integ/msaa-hw`. The floor of about 2 ms with few draws is submit/fence latency, not GPU time.

| draws | 1x median | 4x median | delta |
|---|---|---|---|
| 20 | 1.85 / 2.08 | 2.01 / 1.83 | within noise |
| 100 | 2.08 / 1.98 | 2.17 / 2.04 | about +0.1 ms |
| 300 | 2.65 / 2.64 | 3.47 / 3.49 | +0.83 ms (+32 %) |

4x MSAA plus resolve costs under 1 ms at 300 overlapping draws, so it fits an 8.3 ms (120 fps) budget for this kind of load. Real
shaders and bandwidth-heavy scenes (blending, depth) are not covered.
