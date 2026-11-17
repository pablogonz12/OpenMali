/* SPDX-License-Identifier: MIT */
/*
 * kbase_draw: Mali Valhall v11 (arch 11.x) descriptor packers for an IDVS draw. v11 only, our own code.
 * Bit layouts are documented from Mesa's MIT genxml (src/panfrost/genxml/v11.xml): structs "Tiler Context", "Tiler Heap",
 * "Shader Program", "Attribute", "Buffer", "Resource", "Blend" (+ "Internal Blend", "Blend Equation"), "Depth/stencil",
 * "DCD Flags 0/1/2", "Primitive Flags", "Scissor"; the call sequence follows how panvk (src/panfrost/vulkan/csf) sets
 * the IDVS staging registers. Every packer writes whole descriptors into zero-initialised caller memory.
 */
#ifndef KBASE_DRAW_H
#define KBASE_DRAW_H

#include <stddef.h>
#include <stdint.h>

#include "kbase.h"

/* Bump allocator over one BO (descriptors, vertex data, FAU arrays). */
struct kb_arena { struct kb_bo bo; size_t off; size_t start; int rolling, lapped; };
void *kb_arena_alloc(struct kb_arena *a, size_t size, size_t align, uint64_t *gpu_va);   /* NULL when full; memory is zeroed */
/* Start the descriptors of a new job WITHOUT rewinding: the arena keeps advancing and wraps to offset 0 only when the BO end is
 * reached, so a job's descriptors land on the least recently used VAs instead of the previous job's (whose GPU-written copy may
 * still sit in the GPU L2 cache). A job never overwrites its own allocations (kb_arena_alloc returns NULL instead). Contract: when
 * calling this, every job built from this arena must have signalled, unless the BO holds all in-flight jobs. `a->off = 0` (no
 * kb_arena_begin) keeps the old rewind-every-job behaviour. */
void kb_arena_begin(struct kb_arena *a);

/* Pixel formats (Pixel Format = RGB component order | block<<12 | srgb<<20). On v7+ the low 12 bits are the "RGB Component Order"
 * enum, RGBA = 0. The 0x688 swizzle is the v6 encoding and is NOT a valid order on v11: PanVK's RGBA32F attribute word 0 is
 * 0x2fc00115, i.e. format 0xbf000 (see TRIANGLE-NEXT.md). */
#define KB_PIXFMT_RGBA32F  (0u | (191u << 12))
#define KB_PIXFMT_RGBA8    (0u | (187u << 12))
#define KB_PIXFMT_RGBA8_TB (0u | (237u << 12))    /* blendable tilebuffer format: the Blend "conversion memory format" for RGBA8 UNORM */
#define KB_PIXFMT_V6_SWIZZLE_RGBA 0x688u          /* old wrong value, only for bisecting (t_triangle --revert 1) */
#define KB_PIXFMT_R16F_RGBA (0u | (199u << 12))   /* panvk's "unused attribute" filler format */

#define KB_TILER_CTX_SIZE  128u
#define KB_TILER_HEAP_DESC_SIZE 32u
#define KB_SPD_SIZE        32u
#define KB_LS_SIZE         32u      /* Local Storage (thread storage descriptor), 64-aligned */
#define KB_ATTR_SIZE       32u
#define KB_BUF_SIZE        32u
#define KB_RES_SIZE        16u
#define KB_BLEND_SIZE      16u
#define KB_ZSD_SIZE        32u
#define KB_RES_TABLE_ALIGN 4u       /* MALI_RESOURCE_TABLE_SIZE_ALIGNMENT */
#define KB_DESC_SET_STRIDE 32u      /* descriptor-set entry size */
#define KB_VS_ATTRS        16u      /* attribute slots in the VS driver set; vertex-buffer Buffer descriptors start at 17 */

/* Tiler heap descriptor (32 B): the buffer the Tiler Context points at. base = first chunk, bottom = base+64, top = base+size. */
void kb_draw_tiler_heap_desc(uint32_t *d, uint64_t first_chunk_va, uint32_t chunk_size);
/* Tiler Context (128 B, 64-aligned). geom_buf = geometry-buffer VA (4 KiB aligned) | (size >> 12). */
void kb_draw_tiler_ctx(uint32_t *d, unsigned fb_w, unsigned fb_h, uint32_t hierarchy_mask, uint64_t heap_desc_va, uint64_t geom_buf);
/* Shader Program (32 B). stage: 2 fragment, 3 vertex. work_regs <= 32 selects 32 registers per thread. */
void kb_draw_spd(uint32_t *d, unsigned stage, uint64_t binary_va, unsigned work_regs, uint64_t preload);
/* Shader Program for the IDVS varying (secondary) vertex shader: as kb_draw_spd(stage 3) but warp limit Full (0), as panvk. */
void kb_draw_spd_vs_varying(uint32_t *d, uint64_t binary_va, unsigned work_regs, uint64_t preload);
/* Local Storage (32 B, 64-aligned) with no TLS and no workgroup memory: what panvk points IDVS TSD_0 (r24) at. */
void kb_draw_local_storage_none(uint32_t *d);
/* Attribute (32 B), 1D per-vertex attribute reading from vertex-buffer descriptor `buffer_index` of resource table `table`. */
void kb_draw_attr_1d(uint32_t *d, uint32_t pixfmt, unsigned table, unsigned buffer_index, uint32_t offset, uint32_t stride);
/* Attribute used as "unused" filler (invalid table 17 -> out-of-bounds behaviour). */
void kb_draw_attr_unused(uint32_t *d);
/* Attribute of type "vertex packet": the FS's view of the hardware varying buffer (table 61). */
void kb_draw_attr_varying(uint32_t *d, uint32_t pixfmt, uint32_t varying_offset, uint32_t attr_stride);
/* Buffer (32 B, simple). */
void kb_draw_buffer(uint32_t *d, uint64_t va, uint32_t size);
/* Resource table entry (16 B). */
void kb_draw_resource(uint32_t *d, uint64_t va, uint64_t size_bytes, int contains_descriptors);
/* Blend (16 B, 64-aligned) for RT 0, plain overwrite, fixed-function path in "opaque" mode, enable | round to FB precision,
 * conversion memory format RGBA8_TB, all four channels written. Bit-identical to panvk's descriptor for an RGBA8 UNORM target. */
void kb_draw_blend_opaque_rgba8(uint32_t *d);
/* Same for another blendable render target: conversion pixel format word (index << 12 | component order [| sRGB bit 20]), components 1..4. */
void kb_draw_blend_opaque_fmt(uint32_t *d, uint32_t pixfmt, unsigned ncomp);
/* Depth/stencil (32 B): depth and stencil disabled (compare Always, no writes). */
void kb_draw_zsd_off(uint32_t *d);

/* Depth compare functions (genxml "Func"). */
enum kb_func { KB_FUNC_NEVER, KB_FUNC_LESS, KB_FUNC_EQUAL, KB_FUNC_LEQUAL, KB_FUNC_GREATER, KB_FUNC_NOT_EQUAL, KB_FUNC_GEQUAL, KB_FUNC_ALWAYS };
/* Depth/stencil (32 B) with depth test `func` and optional depth write; stencil off, no bias, fixed-function depth, clamp [0,1],
 * depth cull on, separated dependency tracking (as build_zsd for PAN_ARCH 11). kb_draw_zsd_depth(d, ALWAYS, 0) == kb_draw_zsd_off. */
void kb_draw_zsd_depth(uint32_t *d, enum kb_func func, int write);

/* FAU (push-uniform) table helpers. A FAU table is an array of 32-bit words; hardware reads it as 64-bit entries (kb_draw_fau_ptr).
 * Layout is whatever the shader was compiled with (compiler-spike SPIKE_FAU_RESERVED / SPIKE_PUSH_BASE): words below `reserved`
 * are driver-owned (viewport sysvals, push constants), words from `reserved` up are the compiler's promoted immediates. */
void kb_draw_fau_imm(uint32_t *fau, const uint32_t (*imm)[2], unsigned nimm);            /* copy {word, value} pairs */
void kb_draw_fau_viewport(uint32_t *fau, float w, float h, float zscale, float zoffset);  /* words 0..5: scale xyz, offset xyz; x,y: w/2,h/2 */
void kb_draw_fau_push(uint32_t *fau, unsigned byte_off, const float *v, unsigned n);      /* n floats at FAU byte offset byte_off (multiple of 4) */

struct kb_dcd_opts { unsigned pixel_kill, zs_update; int allow_fpk, allow_fpk_killed, front_ccw; };   /* kill/update: 0 force early, 2 weak early, 3 force late */
uint32_t kb_draw_dcd0(const struct kb_dcd_opts *o);
uint32_t kb_draw_dcd1(uint16_t sample_mask, unsigned rt_mask);
uint32_t kb_draw_dcd2(unsigned read_mask, unsigned write_mask, int no_shader_stencil_read, int no_shader_depth_read);
uint32_t kb_draw_prim_flags_triangles(int secondary_shader);
uint64_t kb_draw_scissor(unsigned minx, unsigned miny, unsigned maxx, unsigned maxy);   /* 2 words, max inclusive */
uint64_t kb_draw_srt_ptr(uint64_t table_va, unsigned entries);                          /* entries are rounded up to KB_RES_TABLE_ALIGN */
uint64_t kb_draw_fau_ptr(uint64_t fau_va, unsigned words32);                            /* 64-bit entry count in bits 63:56 */

/* TODO(v13): the descriptor packers (fb.c, draw.c) encode the Mesa genxml v11 layouts. Mesa has separate v12/v13 genxml
 * (Framebuffer/Render Target/Tiler Context/SPD may differ) that we have not compared. For arch not in {0, 11} this warns
 * once on stderr and carries on with the v11 bit layout. */
void kb_arch_warn_v11_only(unsigned arch, const char *who);

/* Textures (R3-NEXT.md): 2D linear RGBA8 + nearest/clamp sampler. All three descriptors are 32 bytes, 32-aligned. */
#define KB_TEX_SIZE        32u
#define KB_PLANE_SIZE      32u
#define KB_SAMPLER_SIZE    32u
#define KB_TEX_ROW_ALIGN   64u      /* linear row stride alignment (pan_mod.c: align_mask 63) */
#define KB_TEX_USER_TABLE  1u       /* PanVK: FS resource table 0 = driver set, 1 = user set 0 (baked in the shader handle) */
uint32_t kb_tex_row_stride_rgba8(unsigned width);
void kb_draw_plane_linear(uint32_t *d, uint64_t va, unsigned w, unsigned h, uint32_t row_stride);
void kb_draw_texture_2d_rgba8(uint32_t *d, unsigned w, unsigned h, uint64_t plane_va);
void kb_draw_sampler_nearest_clamp(uint32_t *d);

#endif
