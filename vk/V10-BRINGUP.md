# v10 (Mali-G610 MC6, Poco X4 GT, alias poco-x4): offline bring-up status

Status: compiles (native, a64-glibc, a64-bionic objects, -Werror) and passes every fake-backend case in `check.sh` with
`MALI_FAKE_GPU=v10`. NOT run on hardware: the phone was not attached. Nothing here is hardware-proven.

## What v10 is wired as
- `arch_major == 10` -> `env->arch`/`mali_compile` arch 10; it takes the **v11 code path** (RUN_IDVS, IDVS SR, hmask 0xfe, R11
  scratch registers, tile log2 as v11) plus the differences below. Format tables: `pan_pipe_format_v10`, `pan_blendable_formats_v10`.
- Mesa model for the compiler: G610 `0xA8670000` (measured: product_id 0xA867); G310 is `0xAC040000` in Mesa. G710/G510 have no model
  entry in this Mesa, so they would compile against the G610 model (unverified, exact product ids unknown until a probe).
- `kb_v10` (libkbase global, set in `mali_hw_init`) switches the two libkbase packers that differ.

## v10.xml vs v11.xml, per area (third_party/mesa/src/panfrost/genxml)
| Area | v10 | v11 (what our code assumes) | Handled |
|---|---|---|---|
| Scoreboard select | no SET_STATE (27) / SET_STATE_IMM32 (28); `SET_SB_ENTRY` op 23 (3:0 endpoint, 7:4 other), 8 entries (CS_MAX_SB_COUNT 8) | SET_STATE_IMM32, 16 entries | yes: `kb_cs_set_state_imm32` emits SET_SB_ENTRY(ep, LS slot) when `kb_v10`; SB_SEL_OTHER/DEFERRED become NOPs (only diagnostic `MALI_CV*` knobs). Slots used (0, 3) are < 8. |
| ZSD word 4 bit 21 | "Shader read only Z/S" | "Separated dependency tracking" (we set it) | yes: cleared on v10 (`kb_draw_zsd_off`) |
| RUN_IDVS | no "Tiler Flags2 Enable" (bit 55), no Defer Mode (52); IDVS SR has no reg 61 (TILER_FLAGS2) | has both | not needed: we never set bit 55/52; the harmless `M32(r61, 0)` is a plain register write |
| RUN_TILING op 5 | exists (v10 only) | gone | n/a (we use RUN_IDVS, which both have) |
| New v11 CS ops | no LOGIC_OP32, NEXT_SB_ENTRY, SHARED_SB_INC/DEC, no WAIT mode bit | present | n/a: the v11 path does not use them (only v13 does) |
| FBD | params 16 words + 16 padding (VRS image fields at words 16/17 absent); RT mask 8 bits (v11: 11); no conservative raster / cull-zero-area / no-shader-ZS-read bits; no "Early ZS Intersect" | VRS words, wider RT mask | partly: we write zeros in the new fields, size (128 B) is equal. Word 11/12 bits used are the same in both files (diffed). |
| Surface/texture descriptors | 64-bit pointers, 32-bit row stride | 48-bit pointer + Size hi, 27-bit stride + hi bits | yes by value range (hi fields are zero for our sizes); would break only for >4 GiB surfaces / strides >128 MiB |
| MSAA | has `Multiple` (value 2: N samples, 1 surface, unresolved) | removed | unhandled: only matters when MSAA lands (feat/msaa); check there |
| Image/format | 190 reserved vs R10X6G10X6B10X6A10X6 | added | n/a (format table is the v10 one) |
| BO coherency | unknown | COHERENT_LOCAL default on (SSBO atomics across cores) | assumed the same: `MALI_BO_COHERENT_LOCAL=0/1` overrides; CHECK on hardware (atomicAdd count) |
| kbase uAPI | unknown (MediaTek, Android 12-13 kernel 5.10) | 1.21 on CMF | unknown: the fake reports 1.21; probe prints the real one |

## Unhandled / unverified on hardware
1. MSAA `Multiple` value (above). 2. Whether the MediaTek kbase accepts our CSF queue setup (v10 firmware iterators: 8 SBs).
3. COHERENT_LOCAL default. 4. Settled by the probe (vk/DEVICE-poco-x4.md): gpu_id 0xA8670000, shader_present 0x130013 (MC6, non-contiguous, core_id_range 21), uAPI 1.18 (r32p1).
5. `libkbase/tests/*` hand-built references are v11/v13 only; no v10 reference capture exists. 6. G710/G510 model.
