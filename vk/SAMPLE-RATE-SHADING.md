# sampleRateShading: advertised unconditionally (decision, 2026-11-19)

`sampleRateShading = true` is advertised unconditionally since #145 (`mali_physical_device.c`); the
`MALI_EXPERIMENTAL_MSAA=1` gate no longer covers the *feature bit*, only the 4x sample count and the 4x
pipeline path. The reason is that the feature is a no-op when `rasterizationSamples == 1`: with one
sample, `minSampleShading` cannot select a per-sample fragment shader, and the driver's per-sample DCD bit
(`mali_cs_emit.c`, "Evaluate per-sample") is only set when the pipeline actually has samples > 1. The
1-sample path is therefore unaffected by advertising, and it is what `t_vk_feat --only samplerate`
exercises on both fake GPUs (skips pixels: the fake backend renders nothing).

What *has* been verified on hardware: 4x MSAA with per-sample shading (`t_vk_draw` mode 2,
`msaa_sample_shading`) passed on the X8 (v13, G715) and the CMF (v11, G615) with
`MALI_EXPERIMENTAL_MSAA=1`, plus `t_cmd`/`t_meta` 4x draws and `t_vk_feat samplerate`; see
`docs/` MSAA evidence and PR #187/#215.

What stays unverified / limited: sample counts 2 and 8 are not advertised (1|4 only), so a game asking
for "2x MSAA" still fails pipeline creation; `vkCmdClearAttachments` inside a 4x pass is unsupported
(#191 pending); resolve/LOAD/depth combinations are limited; and nothing here makes the 4x *pipeline*
path default-on — it still needs `MALI_EXPERIMENTAL_MSAA=1`, so DXVK gets the feature bit but would fail
if it created a 4-sample pipeline. Megabonk's Unity quality levels default to no MSAA in the low preset;
if a user picks 2x/4x, that is a sample-count problem, not a sampleRateShading one.
