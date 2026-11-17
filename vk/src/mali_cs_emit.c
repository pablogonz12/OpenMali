/* SPDX-License-Identifier: MIT */
/* M8: frame -> descriptors + CSF command stream, for v11 (RUN_IDVS) and v13 (RUN_IDVS2). Sequence and register maps are the
 * hardware-proven ones of libkbase tests/rung2.h / rung2_v13.h, generalised to per-draw state; every descriptor is built by a
 * libkbase packer. Rules kept from libkbase/INTERMITTENT.md and V13-NEXT2.md:
 *   - the caller's alloc is a rolling arena; job_begin (kb_arena_begin) runs once per frame
 *   - after the head FLUSH_CACHE2 there is always a WAIT on slot 0 before the first RUN_*
 *   - kb_wmb() before the submit hook
 *   - v13: FS FAU words 4,5 = blend descriptor words 2,3; FAU blocks sized by R2_FAU_BYTES (VS and FS blocks never overlap);
 *     tiler flags | env.tf_or (0x40000); heap context in r88; RUN_IDVS2(0,0); VS FAU count from the compiled shader
 *   - FAU pointers are written as two MOVE32 (a MOVE48 would drop the entry count in bits 63:56)
 * A shadow-register cache emits MOVEs only for changed registers (the CS ring is only 4 KiB). It is valid because each frame's CS
 * starts empty and every register is (re)written by the first draw. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "mali_pipeline_layout_flat.h"

#include "kbase.h"
#include "kbase_cs.h"
#include "kbase_cs_v13.h"
#include "kbase_draw.h"
#include "kbase_draw_v13.h"
#include "kbase_fb.h"
#include "kbase_samples.h"
#include "mali_cs_emit.h"
#include "mali_event.h"

/* FAU block size in bytes: the qword count given to the hardware, rounded up to 64 B (V13-NEXT2.md). */
#define FAU_BYTES(words32) (((((size_t)(words32) + 1) / 2) * 8 + 63) & ~(size_t)63)

struct cs {
   uint64_t *v;
   unsigned n, cap;
   int err;
   uint32_t sh[128];
   uint8_t sv[16];   /* bitmap of known shadow registers */
};

static void
E(struct cs *c, uint64_t i)
{
   if (c->n == c->cap) {
      unsigned nc = c->cap ? c->cap * 2 : 256;
      uint64_t *nv = realloc(c->v, nc * sizeof *nv);
      if (!nv) {
         c->err = -ENOMEM;
         return;
      }
      c->v = nv;
      c->cap = nc;
   }
   c->v[c->n++] = i;
}

static bool
known(const struct cs *c, unsigned r, uint32_t v)
{
   return (c->sv[r >> 3] >> (r & 7) & 1) && c->sh[r] == v;
}
static void
set_known(struct cs *c, unsigned r, uint32_t v)
{
   c->sv[r >> 3] |= 1u << (r & 7);
   c->sh[r] = v;
}

static void
M32(struct cs *c, unsigned r, uint32_t v)
{
   if (known(c, r, v))
      return;
   E(c, kb_cs_move32(r, v));
   set_known(c, r, v);
}

static void
M48(struct cs *c, unsigned r, uint64_t v)
{
   uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32) & 0xffff;
   if (known(c, r, lo) && known(c, r + 1, hi))
      return;
   E(c, kb_cs_move48(r, v));
   set_known(c, r, lo);
   set_known(c, r + 1, hi);
}

static void
M64(struct cs *c, unsigned r, uint64_t v)
{
   M32(c, r, (uint32_t)v);
   M32(c, r + 1, (uint32_t)(v >> 32));
}

/* Registers of the two arches (kbase_cs.h KB_IDVS_*, kbase_cs_v13.h KB13_IDVS_*). */
struct regs {
   unsigned flush_id, tctx_copy, heap, sync_addr, sync_val;
};
static const struct regs R11 = { 72, 74, 64, 68, 70 };
static const struct regs R13 = { 80, 82, 88, 84, 86 };

struct ddesc {   /* per-draw GPU blocks */
   uint64_t spd_p, spd_v, spd_f, vst, fst, bl, zsd, vfau, ffau;
};

static void *
A(const struct mali_hw *hw, size_t n, size_t al, uint64_t *va)
{
   return hw->alloc(hw->user, n, al, va);
}

static int
build_draw_descs(const struct mali_hw *hw, const struct mali_frame *f, const struct mali_draw *d, struct ddesc *o)
{
   const struct mali_env *env = &hw->env;
   const struct mali_gfx_state *g = &d->gfx;
   const struct mali_compiled *vs = g->vs, *fs = g->fs;
   bool v13 = env->arch == 13;
   uint64_t va_vsset, va_fsset, va_vfau, va_ffau;
   uint32_t *spd_p, *spd_v = NULL, *spd_f, *vst, *fst, *bl, *zsd, *vfau, *ffau;
   uint8_t *vsset, *fsset;

   if (!vs || !fs || !vs->bin_size || !fs->bin_size)
      return -EINVAL;
   if (d->indexed && d->index_size != 1 && d->index_size != 2 && d->index_size != 4)
      return -EINVAL;

   unsigned nvb = 0;
   for (unsigned i = 0; i < g->nattr; i++)
      if (g->attr[i].binding >= nvb)
         nvb = g->attr[i].binding + 1;
   if (nvb > MALI_MAX_VB)
      return -EINVAL;
   unsigned vs_entries = KB_VS_ATTRS + 1 + (nvb ? nvb : 1);
   unsigned nres = 1 + d->nsets;
   unsigned res_alloc = ((nres + 3) & ~3u) * KB_RES_SIZE;

   spd_p = A(hw, KB_SPD_SIZE, 32, &o->spd_p);
   if (!v13)
      spd_v = A(hw, KB_SPD_SIZE, 32, &o->spd_v);
   spd_f = A(hw, KB_SPD_SIZE, 32, &o->spd_f);
   vs_entries = vs_entries > MALI_DUMMY_SAMPLER_IDX + 1 ? vs_entries : MALI_DUMMY_SAMPLER_IDX + 1;
   vsset = A(hw, vs_entries * KB_DESC_SET_STRIDE, 64, &va_vsset);
   vst = A(hw, res_alloc, 64, &o->vst);
   fsset = A(hw, (MALI_DUMMY_SAMPLER_IDX + 1) * KB_DESC_SET_STRIDE, 64, &va_fsset);
   fst = A(hw, res_alloc, 64, &o->fst);
   bl = A(hw, KB_BLEND_SIZE, 16, &o->bl);
   zsd = A(hw, KB_ZSD_SIZE, 32, &o->zsd);
   vfau = A(hw, FAU_BYTES(vs->fau_count), 64, &va_vfau);
   ffau = A(hw, FAU_BYTES(fs->fau_count), 64, &va_ffau);
   if (!spd_p || (!v13 && !spd_v) || !spd_f || !vsset || !vst || !fsset || !fst || !bl || !zsd || !vfau || !ffau)
      return -ENOMEM;
   o->vfau = kb_draw_fau_ptr(va_vfau, vs->fau_count);
   o->ffau = kb_draw_fau_ptr(va_ffau, fs->fau_count);

   /* shader programs */
   if (v13) {
      kb_v13_spd(spd_p, 3, g->vs_va, vs->work_reg_count, vs->preload, 0);
      kb_v13_spd(spd_f, 2, g->fs_va, fs->work_reg_count, fs->preload, 0);
   } else {
      kb_draw_spd(spd_p, 3, g->vs_va, vs->work_reg_count, vs->preload);
      kb_draw_spd_vs_varying(spd_v, g->vs_va + vs->vs_secondary_offset, vs->vs_secondary_work_reg_count,
                             vs->vs_secondary_preload);
      kb_draw_spd(spd_f, 2, g->fs_va, fs->work_reg_count, fs->preload);
   }

   /* VS driver set: attributes 0..15 (unused ones filled), buffer descriptors from entry 17 (vertex binding b = 17 + b) */
   for (unsigned i = 0; i < KB_VS_ATTRS; i++) {
      uint32_t *e = (uint32_t *)(vsset + i * KB_DESC_SET_STRIDE);
      if (i < g->nattr)
         kb_draw_attr_1d(e, g->attr[i].pixfmt, 0, KB_VS_ATTRS + 1 + g->attr[i].binding, g->attr[i].offset, g->attr[i].stride);
      else
         kb_draw_attr_unused(e);
   }
   for (unsigned b = 0; b < nvb; b++)
      kb_draw_buffer((uint32_t *)(vsset + (KB_VS_ATTRS + 1 + b) * KB_DESC_SET_STRIDE), d->vb_va[b], d->vb_size[b]);
   /* resource tables: entry 0 = driver set, entries 1.. = bound user descriptor sets (KB_TEX_USER_TABLE) */
   kb_draw_resource(vst, va_vsset, vs_entries * KB_DESC_SET_STRIDE, 1);
   kb_draw_resource(fst, va_fsset, (MALI_DUMMY_SAMPLER_IDX + 1) * KB_DESC_SET_STRIDE, 1);
   for (unsigned s = 0; s < d->nsets; s++) {
      kb_draw_resource(vst + (1 + s) * (KB_RES_SIZE / 4), d->set_va[s], d->set_bytes[s], 1);
      kb_draw_resource(fst + (1 + s) * (KB_RES_SIZE / 4), d->set_va[s], d->set_bytes[s], 1);
   }
   if (f->colour.rt)
      kb_draw_blend_opaque_fmt(bl, f->colour.rt->blend_pixfmt, f->colour.rt->ncomp);
   else
      kb_draw_blend_opaque_rgba8(bl);

   /* depth/stencil */
   if (g->depth_test && f->has_zs) {
      if (v13)
         kb_v13_zsd_depth(zsd, (enum kb_func)g->depth_func, g->depth_write);
      else
         kb_draw_zsd_depth(zsd, (enum kb_func)g->depth_func, g->depth_write);
   } else if (v13) {
      kb_v13_zsd_depth(zsd, KB_FUNC_ALWAYS, 0);
   } else {
      kb_draw_zsd_off(zsd);
   }

   /* FAU: VS = immediates + viewport sysvals (words 0..5) + push block; FS = immediates + push block */
   const float *vp = d->vp;
   kb_draw_fau_imm(vfau, (const uint32_t(*)[2])vs->imm, vs->nimm);
   kb_draw_fau_viewport(vfau, vp[2], vp[3], vp[5] - vp[4], vp[4]);
   if (vp[0] != 0.0f || vp[1] != 0.0f) {
      float ox = vp[0] + vp[2] * 0.5f, oy = vp[1] + vp[3] * 0.5f;
      memcpy(&vfau[3], &ox, 4);
      memcpy(&vfau[4], &oy, 4);
   }
   kb_draw_fau_imm(ffau, (const uint32_t(*)[2])fs->imm, fs->nimm);
   for (int st = 0; st < 2; st++) {
      const struct mali_compiled *c = st ? fs : vs;
      uint32_t *fau = st ? ffau : vfau;
      size_t room = FAU_BYTES(c->fau_count);
      unsigned n = c->push_const_bytes > MALI_PUSH_MAX ? MALI_PUSH_MAX : c->push_const_bytes;
      if (g->push_base[st] + n > room)
         n = room > g->push_base[st] ? (unsigned)(room - g->push_base[st]) : 0;
      memcpy((uint8_t *)fau + g->push_base[st], d->push[st], n);
   }
   if (v13) {
      ffau[4] = bl[2];
      ffau[5] = bl[3];
   }
   return 0;
}

/* v11 IDVS SR (genxml v11.xml "IDVS SR"): not in libkbase's header. */
#define KB_IDVS_INDEX_BUFFER_SIZE 39
#define KB_IDVS_INDEX_BUFFER 54

/* PRIMITIVE_FLAGS bits 10:8 Index type (genxml v11 and v13: 0 none, 1 u8, 2 u16, 3 u32). */
static uint32_t
idx_type_bits(const struct mali_draw *d)
{
   if (!d->indexed)
      return 0;
   return (d->index_size == 1 ? 1u : d->index_size == 2 ? 2u : 3u) << 8;
}

/* Tiler flags: draw mode (bits 3:0), point size array FP16 (v11, bits 12:11), extended FIFO (bits 23:22 = 2) when the VS writes the
 * point size of a point list, primitive restart (19). `base` = the triangle flags. */
static uint32_t
prim_flag_bits(const struct mali_gfx_state *g, uint32_t base, bool v13)
{
   unsigned m = g->prim ? g->prim : 8;
   uint32_t f = (base & ~0xfu) | m;
   if (m == 1 && g->vs->vs_writes_point_size) {
      if (!v13)
         f |= 2u << 11;
      f = (f & ~(3u << 22)) | (2u << 22);
   }
   if (g->prim_restart && (m == 4 || m == 10 || m == 12))
      f |= 1u << 19;
   return f;
}

static uint32_t
prim_size_bits(const struct mali_gfx_state *g)
{
   unsigned m = g->prim ? g->prim : 8;
   return m == 1 || m == 2 || m == 4 ? 0x3f800000u : 0;   /* 1.0f: default point size / line width */
}

/* The per-draw register file (everything but the FAU pointers, which come after the heap prologue like in the references). */
static void
emit_draw_regs(struct cs *c, const struct mali_env *env, const struct mali_frame *f, const struct mali_draw *d,
               const struct ddesc *o, uint64_t va_tsd, uint64_t va_tctx)
{
   const struct mali_gfx_state *g = &d->gfx;
   bool v13 = env->arch == 13;
   unsigned pk = (g->depth_test && f->has_zs) ? 0 : 2;
   struct kb_dcd_opts dop = { pk, 0, 1, 1, 1 };
   unsigned vx = d->vp[0] < 0 ? 0 : (unsigned)d->vp[0], vy = d->vp[1] < 0 ? 0 : (unsigned)d->vp[1];
   unsigned vmx = vx + (d->vp[2] > 1 ? (unsigned)d->vp[2] : 1) - 1, vmy = vy + (d->vp[3] > 1 ? (unsigned)d->vp[3] : 1) - 1;
   int32_t sx = d->sc[0] < 0 ? 0 : d->sc[0], sy = d->sc[1] < 0 ? 0 : d->sc[1];
   uint64_t scis = kb_draw_scissor((unsigned)sx, (unsigned)sy, (unsigned)(sx + (d->sc[2] > 1 ? d->sc[2] : 1) - 1),
                                   (unsigned)(sy + (d->sc[3] > 1 ? d->sc[3] : 1) - 1));
   uint32_t minz, maxz;
   memcpy(&minz, &d->vp[4], 4);
   memcpy(&maxz, &d->vp[5], 4);

   if (!v13) {
      M48(c, KB_IDVS_VERTEX_SRT, kb_draw_srt_ptr(o->vst, 1 + d->nsets));
      M48(c, KB_IDVS_FRAGMENT_SRT, kb_draw_srt_ptr(o->fst, 1 + d->nsets));
      M48(c, KB_IDVS_VERTEX_POS_SPD, o->spd_p);
      M48(c, KB_IDVS_VERTEX_VARY_SPD, o->spd_v);
      M48(c, KB_IDVS_FRAGMENT_SPD, o->spd_f);
      M48(c, KB_IDVS_TSD, va_tsd);
      M32(c, KB_IDVS_GLOBAL_ATTR_OFFSET, 0);
      M32(c, KB_IDVS_INDEX_COUNT, d->count);
      M32(c, KB_IDVS_INSTANCE_COUNT, d->instance_count);
      M32(c, KB_IDVS_INDEX_OFFSET, d->first_index);
      M32(c, KB_IDVS_INDEX_BUFFER_SIZE, d->indexed ? d->index_bytes : 0);
      M48(c, KB_IDVS_INDEX_BUFFER, d->indexed ? d->index_va : 0);
      M32(c, KB_IDVS_VERTEX_OFFSET, d->first_vertex);   /* UNVERIFIED for firstVertex != 0 (no hardware evidence) */
      M32(c, KB_IDVS_INSTANCE_OFFSET, d->first_instance);
      M32(c, KB_IDVS_DCD2, kb_draw_dcd2(0, 1, 1, 1));
      M48(c, KB_IDVS_TILER_CTX, va_tctx);
      M32(c, KB_IDVS_SCISSOR, (uint32_t)scis);
      M32(c, KB_IDVS_SCISSOR + 1, (uint32_t)(scis >> 32));
      M32(c, KB_IDVS_LOW_DEPTH_CLAMP, minz);
      M32(c, KB_IDVS_HIGH_DEPTH_CLAMP, maxz);
      M48(c, KB_IDVS_OQ, d->occ_mode ? d->occ_va : 0);
      M32(c, KB_IDVS_VARY_SIZE, g->vs->generic_size_B);
      M48(c, KB_IDVS_BLEND_DESC, o->bl | 1);
      M48(c, KB_IDVS_ZSD, o->zsd);
      M32(c, KB_IDVS_TILER_FLAGS, prim_flag_bits(g, kb_draw_prim_flags_triangles(g->vs->vs_secondary_enable), false) | idx_type_bits(d));
      M32(c, KB_IDVS_DCD0, kb_draw_dcd0(&dop) | ((d->occ_mode & 3u) << 14));
      M32(c, KB_IDVS_DCD1, kb_draw_dcd1(0xffff, 1));
      M32(c, KB_IDVS_PRIMITIVE_SIZE, g->vs->vs_writes_point_size && g->prim == 1 ? 0 : prim_size_bits(g));
      M32(c, KB_IDVS_TILER_FLAGS2, 0);
   } else {
      uint64_t vh = kb_v13_viewport_hi(vx, vy, vmx, vmy), vl = kb_v13_viewport_lo(d->vp[4], d->vp[5]);
      M48(c, KB13_IDVS_VERTEX_SRT, kb_draw_srt_ptr(o->vst, 1 + d->nsets));
      M48(c, KB13_IDVS_FRAGMENT_SRT, kb_draw_srt_ptr(o->fst, 1 + d->nsets));
      M48(c, KB13_IDVS_VERTEX_SPD, o->spd_p);
      M48(c, KB13_IDVS_FRAGMENT_SPD, o->spd_f);
      M48(c, KB13_IDVS_VERTEX_TSD, va_tsd);
      M48(c, KB13_IDVS_FRAGMENT_TSD, va_tsd);
      M32(c, KB13_IDVS_GLOBAL_ATTR_OFFSET, 0);
      M32(c, KB13_IDVS_INDEX_COUNT, d->count);
      M32(c, KB13_IDVS_INSTANCE_COUNT, d->instance_count);
      M32(c, KB13_IDVS_INDEX_OFFSET, d->first_index);
      M32(c, KB13_IDVS_VERTEX_OFFSET, d->first_vertex);   /* UNVERIFIED for firstVertex != 0 */
      M32(c, KB13_IDVS_INSTANCE_OFFSET, d->first_instance);
      M48(c, KB13_IDVS_TILER_CTX, va_tctx);
      M32(c, KB13_IDVS_SCISSOR, (uint32_t)scis);
      M32(c, KB13_IDVS_SCISSOR + 1, (uint32_t)(scis >> 32));
      M32(c, KB13_IDVS_VIEWPORT_HIGH, (uint32_t)vh);
      M32(c, KB13_IDVS_VIEWPORT_HIGH + 1, (uint32_t)(vh >> 32));
      M32(c, KB13_IDVS_VIEWPORT_LOW, (uint32_t)vl);
      M32(c, KB13_IDVS_VIEWPORT_LOW + 1, (uint32_t)(vl >> 32));
      M64(c, KB13_IDVS_INDEX_BUFFER, d->indexed ? d->index_va : 0);
      M32(c, KB13_IDVS_INDEX_BUFFER_SIZE, d->indexed ? d->index_bytes : 0);
      M32(c, KB13_IDVS_VARY_SIZE, g->vs->generic_size_B);
      M48(c, KB13_IDVS_ZSD, o->zsd);
      M48(c, KB13_IDVS_BLEND_DESC, o->bl | 1);
      M32(c, KB13_IDVS_TILER_FLAGS, prim_flag_bits(g, kb_v13_prim_flags_triangles(), true) | env->tf_or | idx_type_bits(d));
      M32(c, KB13_IDVS_DCD0, kb_draw_dcd0(&dop) | ((d->occ_mode & 3u) << 14));
      M32(c, KB13_IDVS_DCD1, kb_draw_dcd1(0xffff, 1));
      M32(c, KB13_IDVS_DCD2, kb_v13_dcd2(0, 1, 1, 1, 0, 0));
      M32(c, KB13_IDVS_LINE_WIDTH, prim_size_bits(g));
      M32(c, KB13_IDVS_TILER_FLAGS2, 0);
      M64(c, KB13_IDVS_OQ, d->occ_mode ? d->occ_va : 0);
   }
}

static void
emit_fragment(struct cs *c, const struct mali_env *env, const struct mali_frame *f, const struct regs *R, uint64_t va_fbd,
              uint64_t va_tctx, bool geometry)
{
   unsigned slot = env->slot;
   int x0 = f->area[0] < 0 ? 0 : f->area[0], y0 = f->area[1] < 0 ? 0 : f->area[1];
   unsigned x1 = x0 + (f->area[2] > 1 ? f->area[2] : 1) - 1, y1 = y0 + (f->area[3] > 1 ? f->area[3] : 1) - 1;

   E(c, kb_cs_req_res(KB_CS_RES_FRAGMENT));
   E(c, kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, slot));
   M48(c, 40, kb_fb_pointer_ex(va_fbd, 1, f->has_zs));
   M32(c, 42, kb_fb_bbox_min(x0, y0));
   M32(c, 43, kb_fb_bbox_max(x1, y1));
   if (geometry)
      M48(c, R->tctx_copy, va_tctx);
   M48(c, 44, 0);   /* FRAGMENT TEM_POINTER / ROW_STRIDE alias the IDVS depth clamps / viewport regs: zero */
   M32(c, 46, 0);
   E(c, kb_cs_run_fragment(0));
   E(c, kb_cs_wait(1u << slot));
   if (geometry) {
      E(c, kb_cs_load_multiple(10, R->tctx_copy, 0xf, 40));
      E(c, kb_cs_wait(1u << KB_CS_SLOT_LS));
      E(c, kb_cs_finish_fragment_heap(true, 10, 12));
   }
   E(c, kb_cs_req_res(0));
}

/* ---- M11 compute: RUN_COMPUTE (genxml v11/v13 "COMPUTE SR": SRT r0, FAU r8, SPD r16, TSD r24, GLOBAL_ATTR_OFFSET r32, WG_SIZE r33,
 * JOB_OFFSET r34..36, JOB_SIZE r37..39 (workgroup counts)). Task axis X, increment 1 (always valid; panvk picks a better split). ---- */
#define CR_SRT 0
#define CR_FAU 8
#define CR_SPD 16
#define CR_TSD 24
#define CR_GAO 32
#define CR_WG_SIZE 33
#define CR_JOB_OFFSET 34
#define CR_JOB_SIZE 37
#define CR_TMP 60
#define WLS_CORE_ID_RANGE 16u
#define WLS_MAX_TOTAL (384u * 1024u)

static uint64_t
run_compute_instr(unsigned task_incr, unsigned axis)
{
   return ((uint64_t)4 << 56) | (task_incr & 0x3fff) | ((uint64_t)(axis & 3) << 14);
}

static int
build_compute(const struct mali_hw *hw, const struct mali_frame *f, uint64_t **out, unsigned *nout)
{
   const struct mali_env *env = &hw->env;
   const struct mali_dispatch *d = &f->disp;
   const struct mali_compiled *cs = d->cs;
   bool v13 = env->arch == 13;
   const struct regs *R = v13 ? &R13 : &R11;
   struct cs c;
   uint64_t va_spd, va_set, va_srt, va_fau, va_tsd, va_wls = 0;
   uint32_t *spd, *tsd, *fau;
   uint8_t *set;

   *out = NULL;
   *nout = 0;
   if (!cs || !cs->bin_size || !d->cs_va)
      return -EINVAL;
   for (int i = 0; i < 3; i++)
      if (!cs->local_size[i])
         return -EINVAL;
   unsigned nres = 1 + d->nsets, res_alloc = ((nres + 3) & ~3u) * KB_RES_SIZE;

   spd = A(hw, KB_SPD_SIZE, 32, &va_spd);
   set = A(hw, (MALI_DUMMY_SAMPLER_IDX + 1) * KB_DESC_SET_STRIDE, 64, &va_set);   /* entry MALI_DUMMY_SAMPLER_IDX (all zero) serves texelFetch */
   uint32_t *srt = A(hw, res_alloc, 64, &va_srt);
   fau = A(hw, FAU_BYTES(cs->fau_count), 64, &va_fau);
   tsd = A(hw, KB_LS_SIZE, 64, &va_tsd);
   if (!spd || !set || !srt || !fau || !tsd)
      return -ENOMEM;

   if (v13)
      kb_v13_spd(spd, 1, d->cs_va, cs->work_reg_count, cs->preload, 0);
   else {
      kb_draw_spd(spd, 1, d->cs_va, cs->work_reg_count, cs->preload);
      spd[0] &= ~(3u << 8);   /* bits 9:8 are fragment/vertex only */
   }

   if (cs->wls_size) {
      unsigned adj = 1;
      while (adj < (cs->wls_size > 128 ? cs->wls_size : 128))
         adj <<= 1;
      unsigned inst = 16, log2i = 4;
      while (inst > 1 && (uint64_t)adj * inst * WLS_CORE_ID_RANGE > WLS_MAX_TOTAL) {
         inst >>= 1;
         log2i--;
      }
      size_t total = (size_t)adj * inst * WLS_CORE_ID_RANGE;
      if (total > WLS_MAX_TOTAL)
         return -ENOMEM;
      void *w = A(hw, total, 4096, &va_wls);
      if (!w)
         return -ENOMEM;
      unsigned lg = 0;
      while ((1u << lg) < adj)
         lg++;
      memset(tsd, 0, KB_LS_SIZE);
      tsd[1] = log2i | ((lg + 1) << 8);
      tsd[4] = (uint32_t)va_wls;
      tsd[5] = (uint32_t)(va_wls >> 32);
   } else {
      kb_draw_local_storage_none(tsd);
   }

   if (cs->tls_size) {
      /* TLS spill (panvk GENX(pan_emit_tls), PAN_ARCH >= 9: packed addressing, base >> 8). Size as pan_get_total_stack_size(). */
      unsigned per = cs->tls_size < 16 ? 16 : (cs->tls_size + 15) & ~15u, pot = 1, shift = 0;
      while (pot < per)
         pot <<= 1;
      while ((16u << shift) < cs->tls_size)
         shift++;
      if (!hw->tls || !env->tls_threads || !env->core_id_range)
         return -ENOTSUP;
      uint64_t total = (uint64_t)pot * env->tls_threads * env->core_id_range, va_tls = 0;
      void *tp = hw->tls(hw->user, (size_t)((total + 4095) & ~4095ull), &va_tls);
      if (!tp || (va_tls & 4095))
         return -ENOMEM;
      tsd[0] = shift;
      tsd[2] = (uint32_t)(va_tls >> 8);
      tsd[3] = (uint32_t)(va_tls >> 40) | (1u << 28);
   }

   kb_draw_resource(srt, va_set, (MALI_DUMMY_SAMPLER_IDX + 1) * KB_DESC_SET_STRIDE, 1);
   for (unsigned s = 0; s < d->nsets; s++)
      kb_draw_resource(srt + (1 + s) * (KB_RES_SIZE / 4), d->set_va[s], d->set_bytes[s], 1);
   kb_draw_fau_imm(fau, (const uint32_t(*)[2])cs->imm, cs->nimm);
   {
      size_t room = FAU_BYTES(cs->fau_count);
      unsigned n = cs->push_const_bytes > MALI_PUSH_MAX ? MALI_PUSH_MAX : cs->push_const_bytes;
      if (n > room)
         n = (unsigned)room;
      memcpy((uint8_t *)fau, d->push, n);
   }

   memset(&c, 0, sizeof c);
   E(&c, kb_cs_nop());
   M32(&c, R->flush_id, 0);
   E(&c, kb_cs_flush_cache2(3, 3, 2, R->flush_id));
   E(&c, kb_cs_wait(1u << KB_CS_SLOT_LS));
   M48(&c, CR_SRT, kb_draw_srt_ptr(va_srt, 1 + d->nsets));
   M64(&c, CR_FAU, kb_draw_fau_ptr(va_fau, cs->fau_count));
   M48(&c, CR_SPD, va_spd);
   M48(&c, CR_TSD, va_tsd);
   M32(&c, CR_GAO, 0);
   M32(&c, CR_WG_SIZE, (cs->local_size[0] - 1) | ((cs->local_size[1] - 1) << 10) | ((cs->local_size[2] - 1) << 20));
   M32(&c, CR_JOB_OFFSET, 0);
   M32(&c, CR_JOB_OFFSET + 1, 0);
   M32(&c, CR_JOB_OFFSET + 2, 0);
   if (d->indirect_va) {
      M48(&c, CR_TMP, d->indirect_va);
      E(&c, kb_cs_load_multiple(CR_JOB_SIZE, CR_TMP, 0x7, 0));
      E(&c, kb_cs_wait(1u << KB_CS_SLOT_LS));
   } else {
      M32(&c, CR_JOB_SIZE, d->groups[0]);
      M32(&c, CR_JOB_SIZE + 1, d->groups[1]);
      M32(&c, CR_JOB_SIZE + 2, d->groups[2]);
   }
   E(&c, kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, env->slot));
   E(&c, kb_cs_req_res(KB_CS_RES_COMPUTE));
   E(&c, run_compute_instr(1, 0));
   E(&c, kb_cs_wait(1u << env->slot));
   E(&c, kb_cs_req_res(0));
   M32(&c, R->flush_id, 0);
   E(&c, kb_cs_flush_cache2(1, 1, 0, R->flush_id));
   E(&c, kb_cs_wait(1u << KB_CS_SLOT_LS));
   M48(&c, R->sync_addr, env->sync_va);
   M48(&c, R->sync_val, 1);
   E(&c, kb_cs_sync_add64(R->sync_addr, R->sync_val, KB_CS_SCOPE_SYSTEM));
   if (c.err) {
      free(c.v);
      return c.err;
   }
   *out = c.v;
   *nout = c.n;
   return 0;
}

int
mali_frame_build(const struct mali_hw *hw, const struct mali_frame *f, uint64_t **out, unsigned *nout)
{
   const struct mali_env *env = &hw->env;
   bool v13 = env->arch == 13;
   const struct regs *R = v13 ? &R13 : &R11;
   struct cs c;
   struct ddesc *dd = NULL;
   int rc = 0;
   uint64_t va_smp = 0, va_tctx = 0, va_fbd = 0, va_tsd = 0;

   if (f->nops)
      return mali_ops_build(env, f->ops, f->nops, out, nout);
   if (f->compute)
      return build_compute(hw, f, out, nout);
   *out = NULL;
   *nout = 0;
   if ((env->arch != 11 && env->arch != 13) || !f->has_colour)
      return -EINVAL;
   memset(&c, 0, sizeof c);
   bool geometry = f->ndraws > 0;

   void *smp = A(hw, sizeof kb_sample_positions, 64, &va_smp);
   uint32_t *tctx = A(hw, KB_TILER_CTX_SIZE, 64, &va_tctx);
   uint8_t *fbd = A(hw, KB_FB_DESC_SIZE_ZS, 64, &va_fbd);
   uint32_t *tsd = A(hw, KB_LS_SIZE, 64, &va_tsd);
   if (!smp || !tctx || !fbd || !tsd)
      return -ENOMEM;
   kb_draw_local_storage_none(tsd);
   memcpy(smp, kb_sample_positions, sizeof kb_sample_positions);

   if (geometry) {
      dd = calloc(f->ndraws, sizeof *dd);
      if (!dd)
         return -ENOMEM;
      kb_draw_tiler_heap_desc(env->geo_cpu, env->heap_first_chunk_va, env->heap_chunk_size);
      uint64_t geom = (env->geo_va + 4096) | (65536u >> 12);
      if (v13)
         kb_v13_tiler_ctx(tctx, f->width, f->height, env->hmask, env->tile_log2, env->geo_va, geom);
      else
         kb_draw_tiler_ctx(tctx, f->width, f->height, env->hmask, env->geo_va, geom);
      for (unsigned i = 0; i < f->ndraws; i++)
         if ((rc = build_draw_descs(hw, f, &f->draws[i], &dd[i])))
            goto out;
   }
   if ((rc = mali_render_pack_fbd(env, f, fbd, geometry ? va_tctx : 0, va_smp)))
      goto out;

   /* head: flush stale descriptor lines, WAIT for it (async), then registers */
   E(&c, kb_cs_nop());
   M32(&c, R->flush_id, 0);
   E(&c, kb_cs_flush_cache2(3, 3, 2, R->flush_id));
   E(&c, kb_cs_wait(1u << KB_CS_SLOT_LS));
   if (geometry) {
      for (unsigned i = 0; i < f->ndraws; i++) {
         const struct mali_draw *d = &f->draws[i];
         emit_draw_regs(&c, env, f, d, &dd[i], va_tsd, va_tctx);
         if (i == 0) {
            M48(&c, R->heap, env->heap_ctx_va);
            E(&c, kb_cs_heap_set(R->heap));
            E(&c, kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, env->slot));
            E(&c, kb_cs_set_state_imm32(KB_CS_SB_SEL_OTHER, KB_CS_SLOT_LS));
            E(&c, kb_cs_req_res(KB_CS_RES_IDVS | KB_CS_RES_TILER));
            E(&c, kb_cs_heap_operation(KB_CS_HEAP_VT_STARTED));
         }
         M64(&c, v13 ? KB13_IDVS_VERTEX_FAU : KB_IDVS_VERTEX_FAU, dd[i].vfau);
         M64(&c, v13 ? KB13_IDVS_FRAGMENT_FAU : KB_IDVS_FRAGMENT_FAU, dd[i].ffau);
         E(&c, v13 ? kb13_cs_run_idvs2(0, 0) : kb_cs_run_idvs());
      }
      E(&c, kb_cs_finish_tiling());
      E(&c, kb_cs_wait(1u << env->slot));
      E(&c, kb_cs_heap_operation(KB_CS_HEAP_VT_COMPLETED));
   }
   emit_fragment(&c, env, f, R, va_fbd, va_tctx, geometry);
   if (geometry) {
      M32(&c, R->flush_id, 0);
      E(&c, kb_cs_flush_cache2(1, 1, 0, R->flush_id));
      E(&c, kb_cs_wait(1u << KB_CS_SLOT_LS));
   }
   M48(&c, R->sync_addr, env->sync_va);
   M48(&c, R->sync_val, 1);
   E(&c, kb_cs_sync_add64(R->sync_addr, R->sync_val, KB_CS_SCOPE_SYSTEM));
   rc = c.err;
out:
   free(dd);
   if (rc) {
      free(c.v);
      return rc;
   }
   *out = c.v;
   *nout = c.n;
   return 0;
}

int
mali_frame_submit(const struct mali_hw *hw, const struct mali_frame *f)
{
   uint64_t *cs;
   unsigned n;
   int rc;

   if (!hw || !hw->alloc || !hw->submit)
      return -ENODEV;
   if (hw->job_begin)
      hw->job_begin(hw->user);
   if ((rc = mali_frame_build(hw, f, &cs, &n)))
      return rc;
   kb_wmb();
   rc = hw->submit(hw->user, cs, n);
   free(cs);
   return rc;
}

int
mali_frame_add_draw(struct mali_frame *f, const struct mali_draw *d)
{
   if (f->ndraws == f->cap) {
      unsigned nc = f->cap ? f->cap * 2 : 8;
      struct mali_draw *nd = realloc(f->draws, nc * sizeof *nd);
      if (!nd)
         return -ENOMEM;
      f->draws = nd;
      f->cap = nc;
   }
   f->draws[f->ndraws++] = *d;
   return 0;
}

void
mali_frame_finish(struct mali_frame *f)
{
   free(f->draws);
   free(f->ops);
   f->ops = NULL;
   f->nops = 0;
   f->draws = NULL;
   f->ndraws = f->cap = 0;
}
