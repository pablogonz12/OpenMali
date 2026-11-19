# Geometry shaders: implementation status (2026-11-19)

Short answer: **there is no geometry stage anywhere in this driver.** A GS pipeline is refused; it never draws.
`MALI_ADVERTISE_GS=1` (`src/mali_gs_shim.h`) only makes the *device feature* true so DXVK 2.x/3.x will
create a device — it does not make a geometry shader run, and it is not conformance.

## What a GS draw does today, step by step

| step | behaviour | where |
|---|---|---|
| `vkCreateShaderModule` with GS SPIR-V | succeeds; the words are stored, nothing is compiled (module-less stage compile happens at pipeline create) | `mali_pipeline.c` `vkCreateShaderModule` |
| `vkCreateGraphicsPipelines` with `VK_SHADER_STAGE_GEOMETRY_BIT` | `VK_ERROR_FEATURE_NOT_PRESENT`, no pipeline handle, one line on stderr with the shim on. This is true whether or not `geometryShader` is advertised | `mali_pipeline.c:131-134` (`create_one`, "tessellation / geometry: not supported") |
| `mali_compile` with a geometry stage | the stage enum has VERTEX, FRAGMENT and COMPUTE only; a GS is never handed to the compiler | `mali_compile.h` (`enum mali_stage`) |
| draw path | `vkCmdDraw*` consumes only the VS and FS programs of a pipeline. Nothing reads a GS, `ViewportIndex`, `Layer`, stream-out, or GS invocations | `mali_cmd.c`, `mali_cs_emit.c` |
| limits | `maxGeometryShaderInvocations/InputComponents/OutputComponents/OutputVertices/TotalOutputComponents` are 0 by default and the Vulkan minimums (32/64/64/256/1024) only with `MALI_ADVERTISE_GS=1` | `mali_physical_device.c` `mali_get_properties` |
| dEQP | the `dEQP-VK.geometry.*` tests are not in any list we run; they cannot pass. `tests/t_gs_shim.c` is the only GS test: both env states, feature/limits agreement, the rejected pipeline and a NULL handle | `tests/t_gs_shim.c`, `check.sh` |

## Why the hardware does not help

Valhall (G615, G725) and Bifrost have no geometry stage. PanVK does not offer `geometryShader` either.
The only ways to get one are (a) emulation on the CPU (read back vertices, run the GS, feed an indirect
draw — a frame-time and synchronisation problem for a driver whose frame record is already the
bottleneck), or (b) recognising a *provably trivial* GS (identity, no emission beyond one vertex per
input) at pipeline creation and dropping the stage. Neither exists; (b) is the only one worth doing and
it would still have to refuse everything else.

## What this means for DXVK / Megabonk

DXVK requires the `geometryShader` feature bit at D3D11 feature level 11_0 even though a game may never
create a GS pipeline. With `MALI_ADVERTISE_GS=1`:

- the device is created (`tests/t_gs_shim.c --expect on`, `t_dxvk_caps` reports one fewer missing feature);
- a D3D11 pipeline that really contains a GS fails `vkCreateGraphicsPipelines` with
  `VK_ERROR_FEATURE_NOT_PRESENT`, which DXVK reports as a pipeline-compile error — the affected draws do
  not render, and nothing renders *wrong*;
- Megabonk (Unity 2023.2 Built-in RP, Megabonk's shader set per `docs/`) emits no GS stage; the failure
  mode above is therefore expected to be unreachable there, but that is an expectation, not evidence.

## Rules

- Keep `MALI_ADVERTISE_GS` off by default: with the bit on, `t_dxvk_caps` and `t_vk_info` no longer
  describe a conformant device, and any CTS run including geometry shaders must be excluded.
- Never enabled implicitly by another feature env.
- When the identity-GS case (b) is implemented, `t_gs_shim` gains a positive draw case and this file
  loses the "no GS runs" line — not before.
