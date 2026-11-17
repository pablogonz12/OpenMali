# R3-NEXT: textured draw (written 2026-11-16, OFFLINE: nothing here has run on hardware)

Test: `out/t_texture-aarch64` (`./build.sh`; shader `../compiler-spike/tex.frag` via `gen-shaders.sh` -> `tests/shaders_v11.h` `fs_tex_*`).
Paths are relative to `driver/third_party/mesa/src/panfrost` (`v11.xml` = `genxml/v11.xml`).
The existing shaders in `shaders_v11.h` are byte-identical to before; only `fs_tex_*` was added.

## 0. Test and interpretation
"64x64 quad covering 256x256 target" is read as: one full-target quad (NDC +-1, 2 triangles, 6 non-indexed vertices) sampling an 8x8-texel texture, so each texel is a 32x32-pixel cell. UV = (NDC+1)/2, so u = px/256 exactly. Texel (tx,ty) = R `(parity?200:0)+8ty`, G `(parity?0:200)+8ty`, B `32tx`, A 255 (a tx/ty transposition is detectable). Checks: all 65536 pixels (+-1/channel), except within 1.5 px of a cell edge, where either neighbouring cell's colour is accepted. Also all 64 cell centres. Options: `--dump --only 1 --timeout MS --hmask --pk --zs --slot --chunk`. VS = `pos.vert` unchanged (attribute 1 "col" carries the UV in .xy). The FS is `tex.frag`: `texture(tex, v_col.xy)`.

## 1. Fields and Mesa sources
| Item | Value | Source |
|---|---|---|
| Layout choice | 2D **linear**, not AFBC, not 16x16 tiled; CPU-written BO; row stride = `align(w*4, 64)` = 64; base 64-aligned | `lib/pan_mod.c` (~980, `align_mask = MAX2(align_mask, 63)` for linear); Generic plane `clump ordering Linear` |
| TEXTURE w0 | type 2 (Texture), dimension 2D = 2<<4, format `KB_PIXFMT_RGBA8` (187<<12) << 10 | `v11.xml:1589`, `lib/pan_texture.c:1272-1339` |
| TEXTURE w1 | (w-1) \| (h-1)<<16 | same |
| TEXTURE w2 | swizzle 0x688 (RGBA identity, 3 bits each), texel interleave 0, levels-1 = 0 | same; format word = same as the attribute/blend pixel format already working in t_triangle |
| TEXTURE w3 | 0 (min/max LOD, sample count log2) | same |
| TEXTURE w4:5 | GPU address of the plane descriptor | same (payload pointer) |
| GENERIC PLANE w0 | type 10 (Plane), Generic 1<<4, clump ordering Linear 2<<8, clump format RAW32 2<<24 | `v11.xml:1620-1634`, `lib/pan_texture.c:426-459` |
| PLANE w1 | size = row_stride*h = 512 | same |
| PLANE w2:3 | texel base pointer (w3 bits 31:16 = size hi = 0) | same |
| PLANE w4 | row stride (27 bits); slice stride hi = 0 | same |
| PLANE w6 / w7 | slice stride 0 / (w-1)\|(h-1)<<16 | same |
| SAMPLER w0 | type 1; wrap R/T/S = 9 (clamp to edge) at 8/12/16; seamless cube bit 23; normalized coords bit 25; minify/magnify nearest bits 27/28; round-to-nearest-even **0** (bit 21); mipmap mode Nearest 0 | `v11.xml:1393-1420`, `vulkan/panvk_vX_sampler.c:112-178` (NEAREST/NEAREST clears `round_to_nearest_even`) |
| SAMPLER w1..w7 | 0 (LOD 0..0, compare Never, border colour 0) | same |
| Shader handle | `pan_res_handle(table=1, tex idx 0)`, sampler (table 1, idx 1); packed `(tex_table<<27)\|(tex_idx<<16)\|(samp_table<<11)\|samp_idx` = `0x08000801`, baked in the FS as an FAU immediate (word 8) | `compiler/pan_nir_lower_tex.c:735-765`, `vulkan/panvk_vX_nir_lower_descriptors.c:140-168,921-953`, `valhall.h:146` |
| spike `lower_tex_handles` | replaces the tex/sampler derefs with that immediate; env `SPIKE_TEX_TABLE/IDX/SAMP_IDX` (1/0/1) | as above |
| FS resource table | entry 0 = driver set (varying attr desc at index 0, zeroed dummy sampler at index 1; 64 B, contains_descriptors); entry 1 = user set 0 (texture at 0, sampler at 1; 64 B). `kb_draw_srt_ptr(va, 2)` rounds to 4 entries | `vulkan/panvk_vX_cmd_desc_state.c:257-313`, `panvk_descriptor_set_layout.h:165-200` |
| FS FAU | 9 words (8 reserved + the handle) | compiler output `FS_TEX_FAU_COUNT` |

## 2. Guessed (no direct Mesa line)
- Texture w2 swizzle 0x688 is taken from the working v6 swizzle value used for attributes; the real value comes from the format table at pack time.
- Driver-set size (2 entries) and the dummy sampler at driver index 1: PanVK's FS table 0 holds more entries; we only need the varying attribute and the sampler slot not to trap.
- Sampler `maxLod` = 0 and `round_to_nearest_even` = 0; any LOD is 0 so the sampler should be fine.
- Row stride alignment 64 (could need only 16 for linear, not an issue since 64 is stricter).
- Whether the CS-start `FLUSH_CACHE2(3,3,2)` also invalidates the texture cache (the texels are written by the CPU before submit).
- Plane descriptor placed in its own 32-aligned allocation; the texture's payload pointer is assumed to point at it directly.

## 3. Ranked fallbacks (if the output is wrong or faults)
1. **Fault / hang with `INSTR_INVALID_PC` or a data fault**: wrong handle -> table index. Try `SPIKE_TEX_TABLE=0` with the user set moved to entry 0 (driver set last), or change the texture to descriptor index 1.
2. **Garbage or clear colour, no fault**: row stride / alignment. Try stride 256 (w*32) or 16; look at `--dump` of the plane descriptor.
3. **Right cells, wrong colours** (R/B swapped, A wrong): texture swizzle (w2) or pixel-format word; try `KB_PIXFMT_V6_SWIZZLE_RGBA` OR'd into the format.
4. **Black / zero texels**: clump format RAW32 vs a 4x8 clump format; or the texture cache not invalidated (add a second `FLUSH_CACHE2(3,3,2)`, or write the texels before an earlier job).
5. **Handle packing**: the descriptors are read via a 64-bit handle in some arch revisions; try the wide handle form from `pan_res_handle`.
6. **Cells shifted by half a texel / edge noise**: sampler rounding, flip bit 21 (round to nearest even) or the sample-corner behaviour; unlikely for exact cell centres.
7. **Normalized-coordinates bit** (bit 25): if everything samples one texel, the coordinates are being treated as integers.

## 4. Hardware test order
1. `./t_texture-aarch64 --only 1 --dump` (baseline; check `pixels_bad`, `sample`, `unexpected_values`).
2. If it hangs: `--timeout 8000`, then read `fault_state` / events; check the dump of `fs_res_table`, `fs_user_set0`, `tex_plane`.
3. Re-run the earlier rungs (t_triangle, t_uniform, t_depth) to confirm they pass unchanged.
4. Apply the fallbacks in the order of section 3, one at a time.

## 5. Hardware result (CMF Phone 1, 2026-11-16, 1 run)
`t_texture --only 1 --dump`: PASS on the FIRST run, no fallback needed. 65536/65536 px (0 bad, max channel err 0), 64/64 cell centres, edge-tolerant px 7936, fence signalled 6.7 ms, 0 leaked BOs. All section-1 guessed fields (swizzle 0x688, stride 64, RAW32 plane, handle 0x08000801, round-even 0) were accepted as written. Raw: runs/20261116-cmf-libkbase/t_texture-r1.jsonl.
Sanity: t_clear, t_uniform, t_depth PASS. t_triangle full run FAILS `triangle_varying` (0 px, polylist_hi16=0, no fault) when run after triangle_flat in the same process; `--only 2` PASSES (runs/20261116-cmf-libkbase/t_triangle-sanity-only2.jsonl). Pre-existing state interaction between two consecutive draws (not texture related; t_texture has a single draw). Hardware runs used: 6 of 8.
