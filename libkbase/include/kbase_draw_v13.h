/* SPDX-License-Identifier: MIT */
/*
 * kbase_draw_v13: arch-13 (Mali-G925) descriptor packers, additive to kbase_draw.h (reuses its heap-desc, attribute, buffer,
 * resource, blend, FAU helpers, which are unchanged between v11 and v13). Every field is annotated with its genxml v13.xml /
 * panvk source in src/draw_v13.c and V13-NEXT.md; GUESS marks what has no hardware evidence yet.
 */
#ifndef KBASE_DRAW_V13_H
#define KBASE_DRAW_V13_H

#include "kbase_draw.h"

/* Tiler Context (128 B, 64-aligned). tile_log2 = Effective Tile Size (log2 of pixels per tile; GUESS 10 to match the FBD). */
void kb_v13_tiler_ctx(uint32_t *d, unsigned fb_w, unsigned fb_h, uint32_t hierarchy_mask, unsigned tile_log2, uint64_t heap_desc_va,
                      uint64_t geom_buf);
/* Shader Program (32 B). stage: 2 fragment, 3 vertex. No warp-limit bits on v13; Max Warps (1:16) = max_warps (GUESS 0). */
void kb_v13_spd(uint32_t *d, unsigned stage, uint64_t binary_va, unsigned work_regs, uint64_t preload, unsigned max_warps);
/* Depth/stencil (32 B), v13 layout (no "separated dependency" bit 21). */
void kb_v13_zsd_depth(uint32_t *d, enum kb_func func, int write);
/* DCD Flags 2 with optional HSR bits (hsr = 0 gives the plain v11-style value). */
uint32_t kb_v13_dcd2(unsigned read_mask, unsigned write_mask, int no_shader_stencil_read, int no_shader_depth_read, int hsr_can_cull,
                     int hsr_can_be_culled);
/* Viewport registers r44:45 (high) and r46:47 (low); max inclusive. */
uint64_t kb_v13_viewport_hi(unsigned minx, unsigned miny, unsigned maxx, unsigned maxy);
uint64_t kb_v13_viewport_lo(float min_depth, float max_depth);
/* Primitive flags (r56) for triangles: no secondary shader, no point-size bits. */
uint32_t kb_v13_prim_flags_triangles(void);
/* Varying attribute for the FS driver set (entry 0): as kb_draw_attr_varying but buffer_index = 1 (panvk_vX_cmd_draw.c:504, arch>=12). */
void kb_v13_attr_varying(uint32_t *d, uint32_t pixfmt, uint32_t varying_offset, uint32_t attr_stride);
/* HSR bits of an already packed FBD (kb_fb_pack output, 64-aligned): word 0 bits 11..14, PanVK pan_desc.c:1573-1587. */
void kb_v13_fbd_hsr(uint32_t *fbd, int filter, int hier, int interleave, int prepass_late);

#endif
