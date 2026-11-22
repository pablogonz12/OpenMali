/* SPDX-License-Identifier: MIT */
/* mali_compile(): SPIR-V -> NIR -> Mesa panfrost compiler (bi_* Bifrost/Valhall backend) -> binary + metadata.
 * The lowering sequence is the one proven by driver/compiler-spike/spike.c (PanVK flow, panvk_vX_shader.c, as oracle only).
 * Output is deterministic and cached on disk keyed by blake3(spirv, arch, stage, options, Mesa tag, lowering version). */
#include "compiler/glsl_types.h"
#include "compiler/spirv/nir_spirv.h"
#include "panfrost/compiler/bifrost/bifrost_compile.h"
#include "panfrost/compiler/pan_compiler.h"
#include "panfrost/compiler/pan_nir.h"
#include "panfrost/model/pan_model.h"
#include "util/blake3/blake3.h"
#include "nir_xfb_info.h"
#include "nir.h"
#include "nir_builder.h"

#include "panfrost/lib/pan_blend.h"
#include "vk_blend.h"
#include "vk_format.h"
#include "vk_nir_convert_ycbcr.h"
#include "vk_ycbcr_conversion.h"

#include "mali_compile.h"
#include "mali_descriptor_lower.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Bump when the lowering or the serialized layout changes. */
#define MALI_COMPILE_VERSION 41 /* 41: partially-bound descriptor arrays select the Null Descriptor for an out-of-range index (bflags in mali_lbinding), so the code differs from 40 ; 40: the robust store bound spans the lowest..highest written component (a sparse write mask left a gap uncounted) and rejects a wrapping component offset, so the code differs from 39 ; 39: robust SSBO stores are bounded by our own wrap-safe predicate instead of Mesa's 32-bit "offset + size - 1" test (a scalar-layout vec4 at 0xfffffff4 wrapped to 3 and wrote past the descriptor), so their code differs from 38 ; 38: the get_ssbo_size/get_ubo_size lowering runs for every shader (a non-robust shader with a size query kept an intrinsic Valhall does not have), so its code differs from 37 ; 37: robust load bounds reject a wrapping end offset (offset 0xfffffff4 + 4 summed to 3 and passed the size test) and the pass invalidates metadata (it inserts an if), so a robust program's code differs from 36 ; 36: number collision on 34 merged -- main's 34 (noperspective varyings) and this lane's 34/35 (robustness2 size query, then out-of-bounds loads returning zero) both change generated code, so neither alone may be reused ; 35: robustness2 out-of-bounds LOADS return zero (mali_descriptor_lower.c lower_load_bounds wraps them in an if; Mesa's pass only rewrote the offset, which read element 0), so a robust program's code differs from 34 ; 34: robustness2 buffer bounds: get_ssbo_size/get_ubo_size is lowered to a load of dw1 of the hardware Buffer descriptor through resource table 62 (mali_descriptor_lower.c lower_size_query), so a robust program's code differs from 33 ; 33: a texelFetch (txf/txf_ms) of a combined image sampler takes the sampler half of that binding instead of the all-zero dummy sampler (the fetch returned 0 on the phone) ; 32: nir_lower_xfb_to_stores keeps the captured outputs for rasterization (keep_outputs), so the generated code of an XFB vertex shader differs from 31 ; 31: mali_spirv_has_xfb() keys on OpExecutionMode Xfb (16/11) from word 5, the same word spirv_to_nir reads (29/30 detected nothing, so no stores were ever emitted) ; 29: transform-feedback outputs lowered to global stores, per-buffer bases loaded from the driver's push-constant area ; 27: transform-feedback vertex shaders are detected and refused (MALI_COMPILE_UNSUPPORTED), before anything is cached ; 26: requested subgroup size (arch warp, 16) is validated, part of the cache key and recorded in mali_compiled ; 25: logicOp blend shader; logicOp in mali_compile_blend (opts struct grew) ; 24: merged fau_reserved API-push-block fix with the unique cache temporary (pid + sequence, O_EXCL) ; 23: fau_reserved covers the API push block (promoted immediates could land in it: >56 B VS, >32 B CS/FS-v11) / cache_store unique temporary ; 22: nir_lower_wpos_center (gl_FragCoord.xy is a pixel centre in Vulkan, hardware gives the corner) ; 21: MALI_OPT_GCM env pass added (output unchanged when unset) ; 20: 4a10c07 moved the dynamic-buffer handles (table 0 entry 64 -> table 9 entry 0) without a bump: cached v19 shaders addressed the old table 34: noperspective varyings: the VS gets the linked FS's noperspective bitfield inlined (nir_intrinsic_load_noperspective_varyings_pan), so the VS stores x w wherever the FS multiplies by gl_FragCoord.w; cached 33 VS code of a noperspective pipeline has no multiply and renders c/w */

_Static_assert(sizeof(struct pan_varying_layout) <= MALI_VARYING_BLOB_MAX, "varying blob too small");
_Static_assert(PAN_MAX_VARYINGS <= 64 || 1, "");

static atomic_ullong g_hits, g_misses, g_stores;
static atomic_uint g_tmp_seq;   /* makes cache_store's temporary name unique between threads of one process */

unsigned
mali_subgroup_size_for_arch(unsigned arch)
{
   /* Mesa panfrost/compiler/pan_compiler.h pan_subgroup_size(): the lane count of a warp is a property of the architecture,
    * not of the number of cores (shader_present is non-contiguous: 0xee0057 on G925 still has 16-wide warps). */
   if (arch >= 9)
      return 16;
   if (arch >= 7)
      return 8;
   if (arch >= 6)
      return 4;
   return 1;   /* Midgard: no subgroups */
}
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static const struct pan_model *g_model[3];   /* [0] v10 (G610), [1] v11 (G615), [2] v13 (G925 class) */

static void
init_once(void)
{
   glsl_type_singleton_init_or_ref();
   /* GPU ids: G615 (v11) 0xB8030000, G725/G925 class (v13) 0xD8000000 (see compiler-spike/NOTES.md) */
   /* v10: G610 0xA8670000 (measured on the phone: product_id 0xA867); G710/G510 have no model entry in this Mesa and use it too */
   static const uint64_t ids[3] = {0xA8670000ull, 0xB8030000ull, 0xD8000000ull};
   for (int i = 0; i < 3; i++)
      for (unsigned v = 0; v < 16 && !g_model[i]; v++)
         g_model[i] = pan_get_model(ids[i], v, 16, 0);
}

/* ---- NIR passes (same as spike.c) ---- */

static bool
lower_sysvals(nir_builder *b, nir_intrinsic_instr *i, void *d)
{
   unsigned off;
   switch (i->intrinsic) {
   case nir_intrinsic_load_viewport_scale:  off = 0;  break;
   case nir_intrinsic_load_viewport_offset: off = 12; break;
   case nir_intrinsic_load_first_vertex:    off = 24; break;   /* shaderDrawParameters: BaseVertex / BaseInstance / DrawIndex, written per draw (mali_cs_emit.c) */
   case nir_intrinsic_load_base_instance:   off = 28; break;
   case nir_intrinsic_load_draw_id:         off = 32; break;
   case nir_intrinsic_load_base_global_invocation_id:   /* compute stub: no vkCmdDispatchBase, base is 0 */
   case nir_intrinsic_load_base_workgroup_id:
      b->cursor = nir_before_instr(&i->instr);
      nir_def_replace(&i->def, nir_imm_zero(b, i->def.num_components, i->def.bit_size));
      return true;
   default: return false;
   }
   b->cursor = nir_before_instr(&i->instr);
   nir_def *v = nir_load_push_constant(b, i->def.num_components, i->def.bit_size, nir_imm_int(b, off));
   nir_def_replace(&i->def, v);
   return true;
}

struct push_ctx { unsigned base; unsigned max_end; };

/* The hardware instance id already includes the IDVS instance offset (= firstInstance). nir_lower_system_values makes
 * InstanceIndex = instance_id + base_instance and InstanceId = instance_id, so: instance_id := hw_id - base_instance
 * (InstanceId excludes firstInstance, InstanceIndex = hw_id) and base_instance is the real firstInstance (a sysval). */
static bool
lower_base_inst(nir_builder *b, nir_intrinsic_instr *i, void *d)
{
   if (i->intrinsic != nir_intrinsic_load_instance_id)
      return false;
   b->cursor = nir_after_instr(&i->instr);
   nir_def *sub = nir_isub(b, &i->def, nir_load_base_instance(b));
   nir_def_rewrite_uses_after(&i->def, sub);
   return true;
}

static bool
add_push_base(nir_builder *b, nir_intrinsic_instr *i, void *d)
{
   struct push_ctx *c = d;
   if (i->intrinsic != nir_intrinsic_load_push_constant)
      return false;
   unsigned bytes = i->def.num_components * (i->def.bit_size / 8);
   if (nir_src_is_const(i->src[0]))
      c->max_end = MAX2(c->max_end, nir_src_as_uint(i->src[0]) + bytes);
   else
      c->max_end = UINT32_MAX;
   /* bifrost_compile.c asserts range == 0 (the value is ignored in release builds) */
   bool changed = nir_intrinsic_range(i) != 0;
   nir_intrinsic_set_range(i, 0);
   if (!c->base)
      return changed;
   b->cursor = nir_before_instr(&i->instr);
   nir_src_rewrite(&i->src[0], nir_iadd_imm(b, i->src[0].ssa, c->base));
   return true;
}

/* Texture/sampler derefs -> constant Valhall resource handles: table 1, texture at 0, sampler at 1 (spike defaults). M7 replaces this. */
static bool
lower_tex_handles(nir_builder *b, nir_instr *instr, void *d)
{
   if (instr->type != nir_instr_type_tex)
      return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   int ti = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
   int si = nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);
   if (ti < 0 || si < 0)
      return false;
   b->cursor = nir_before_instr(instr);
   nir_tex_instr_remove_src(tex, MAX2(ti, si));
   nir_tex_instr_remove_src(tex, MIN2(ti, si));
   nir_tex_instr_add_src(tex, nir_tex_src_texture_handle, nir_imm_int(b, pan_res_handle(1, 0)));
   nir_tex_instr_add_src(tex, nir_tex_src_sampler_handle, nir_imm_int(b, pan_res_handle(1, 1)));
   tex->texture_index = 0;
   tex->sampler_index = 0;
   return true;
}

static unsigned
type_size(const struct glsl_type *t, bool bindless)
{
   return glsl_count_attribute_slots(t, false);
}

/* ---- cache ---- */

struct opts_key {   /* everything besides the SPIR-V that changes the output */
   uint32_t version, arch, stage, fau_reserved, push_base, vs_layout_size, sizeof_compiled, strip_psiz, clip_distances, sample_shading;
   uint32_t subgroup_size;   /* required subgroup size (0 = arch default): a requested 8/32 must not reuse a 16-wide binary */
   /* The resource-handle layout the descriptor lowering bakes into the code (mali_descriptor_lower.c): a changed constant must miss the cache */
   uint32_t max_sets, dyn_table, dyn_buf_start, dummy_sampler_idx, max_push_bytes;
   char mesa[24];
};

static void
mkey(const struct mali_compile_opts *o, unsigned fau_reserved, unsigned push_base, char hex[65])
{
   struct opts_key k;
   memset(&k, 0, sizeof k);
   k.version = MALI_COMPILE_VERSION;
   k.arch = o->arch;
   k.stage = o->stage;
   k.fau_reserved = fau_reserved;
   k.push_base = push_base;
   k.strip_psiz = o->strip_psiz != 0;
   k.clip_distances = o->clip_distances;
   k.sample_shading = o->sample_shading != 0;
   k.subgroup_size = o->subgroup_size;
   k.vs_layout_size = o->link_vs ? o->link_vs->size : 0;
   k.sizeof_compiled = sizeof(struct mali_compiled);
   k.max_sets = MALI_MAX_SETS;
   k.dyn_table = MALI_DYN_TABLE;
   k.dyn_buf_start = MALI_DYN_BUF_START;
   k.dummy_sampler_idx = MALI_DUMMY_SAMPLER_IDX;
   k.max_push_bytes = MALI_MAX_PUSH_BYTES;
   snprintf(k.mesa, sizeof k.mesa, "%s", PACKAGE_VERSION);
   blake3_hasher h;
   blake3_hasher_init(&h);
   blake3_hasher_update(&h, &k, sizeof k);
   if (o->link_vs && o->link_vs->size)
      blake3_hasher_update(&h, o->link_vs->data, o->link_vs->size);
   /* The VS code now depends on the FS's interpolation qualifiers (noperspective mask): hash the module, not the mask,
    * so a cache hit needs no parse of the FS at all and a changed FS can never reuse a VS. */
   if (o->fs_spirv && o->fs_spirv_words) {
      blake3_hasher_update(&h, o->fs_spirv, o->fs_spirv_words * 4);
      const char *fe = o->fs_entry ? o->fs_entry : "main";
      blake3_hasher_update(&h, fe, strlen(fe) + 1);
   }
   if (o->layout) {
      const struct mali_compile_layout *L = o->layout;
      blake3_hasher_update(&h, &L->nsets, sizeof L->nsets);
      blake3_hasher_update(&h, L->dyn_start, sizeof L->dyn_start);
      blake3_hasher_update(&h, &L->push_size, sizeof L->push_size);
      blake3_hasher_update(&h, &L->robust_access, sizeof L->robust_access);
      for (unsigned i = 0; i < L->nsets && i < MALI_MAX_SETS; i++) {
         blake3_hasher_update(&h, &L->set[i].nbindings, sizeof(uint32_t));
         blake3_hasher_update(&h, L->set[i].b, L->set[i].nbindings * sizeof *L->set[i].b);
         /* YCbCr conversions (model, range, siting, swizzle) change the generated code */
         for (unsigned n = 0; n < L->set[i].nbindings && L->set[i].ycbcr; n++) {
            const struct mali_lbinding *b = &L->set[i].b[n];
            if (b->type != MALI_DT_NONE && b->ycbcr_planes)
               blake3_hasher_update(&h, (const struct vk_ycbcr_conversion_state *)L->set[i].ycbcr + b->ycbcr_off,
                                    b->count * sizeof(struct vk_ycbcr_conversion_state));
         }
      }
   }
   const char *e = o->entry ? o->entry : "main";
   blake3_hasher_update(&h, e, strlen(e) + 1);
   for (unsigned i = 0; i < o->nspec; i++) {   /* specialization constants change the shader */
      blake3_hasher_update(&h, &o->spec[i], sizeof o->spec[i]);
      blake3_hasher_update(&h, (const uint8_t *)o->spec_data + o->spec[i].offset, o->spec[i].size);
   }
   blake3_hasher_update(&h, o->spirv, o->spirv_words * 4);
   uint8_t d[32];
   blake3_hasher_finalize(&h, d, 32);
   for (int i = 0; i < 32; i++)
      sprintf(hex + 2 * i, "%02x", d[i]);
}

static int
cache_dir(const struct mali_compile_opts *o, char *buf, size_t n)
{
   const char *d = o->cache_dir;
   if (d) {
      if (!*d) return 0;
      snprintf(buf, n, "%s", d);
      return 1;
   }
   if ((d = getenv("MALI_SHADER_CACHE"))) {
      if (!*d) return 0;
      snprintf(buf, n, "%s", d);
      return 1;
   }
   const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
   if (x && *x) snprintf(buf, n, "%s/mali-gaming/shaders", x);
   else if (h && *h) snprintf(buf, n, "%s/.cache/mali-gaming/shaders", h);
   else return 0;
   return 1;
}

static void
mkdirs(char *p)
{
   for (char *s = p + 1; *s; s++)
      if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
   mkdir(p, 0755);
}

#define CACHE_MAGIC 0x4d414c43u   /* 'MALC' */

static void
cache_store(const char *dir, const char *hex, const struct mali_compiled *c)
{
   char path[1024], tmp[1100], d2[1024];
   snprintf(d2, sizeof d2, "%s", dir);
   mkdirs(d2);
   snprintf(path, sizeof path, "%s/%s.bin", dir, hex);
   /* pid + a per-process sequence. Two workers storing the same key at the same time used to share "<path>.tmp<pid>":
    * one could truncate the file the other was writing and then rename half-written data into the cache (cache_load
    * rejects it, so it cost a recompile rather than a wrong shader). "x" makes the create exclusive: a collision is an
    * error we retry, never a silent truncation. The temporary is unlinked on every failure path below. */
   for (unsigned try_ = 0; try_ < 8; try_++) {
      snprintf(tmp, sizeof tmp, "%s.tmp%d.%u", path, (int)getpid(), atomic_fetch_add(&g_tmp_seq, 1));
      FILE *f = fopen(tmp, "wbx");
      if (f) {
         struct mali_compiled h = *c;
         uint32_t magic = CACHE_MAGIC;
         h.bin = NULL; h.imm = NULL; h.cache_hit = 0;
         int ok = fwrite(&magic, 4, 1, f) == 1 && fwrite(&h, sizeof h, 1, f) == 1 &&
                  fwrite(c->bin, 1, c->bin_size, f) == c->bin_size &&
                  (!c->nimm || fwrite(c->imm, sizeof *c->imm, c->nimm, f) == c->nimm);
         ok = (fclose(f) == 0) && ok;
         if (ok && rename(tmp, path) == 0) atomic_fetch_add(&g_stores, 1);
         else unlink(tmp);
         return;
      }
      if (errno != EEXIST) return;
   }
   return;
}

static int
cache_load(const char *dir, const char *hex, struct mali_compiled *c)
{
   char path[1024];
   snprintf(path, sizeof path, "%s/%s.bin", dir, hex);
   FILE *f = fopen(path, "rb");
   if (!f) return 0;
   uint32_t magic = 0;
   struct mali_compiled h;
   int ok = fread(&magic, 4, 1, f) == 1 && magic == CACHE_MAGIC && fread(&h, sizeof h, 1, f) == 1 &&
            h.bin_size < (64u << 20) && h.nimm < (1u << 20);
   if (ok) {
      h.bin = malloc(h.bin_size ? h.bin_size : 1);
      h.imm = malloc((h.nimm ? h.nimm : 1) * sizeof *h.imm);
      ok = h.bin && h.imm && fread(h.bin, 1, h.bin_size, f) == h.bin_size &&
           (!h.nimm || fread(h.imm, sizeof *h.imm, h.nimm, f) == h.nimm);
      if (ok) { char x; ok = fread(&x, 1, 1, f) == 0; }   /* no trailing bytes */
      if (!ok) { free(h.bin); free(h.imm); }
   }
   fclose(f);
   if (!ok) return 0;
   h.cache_hit = 1;
   *c = h;
   return 1;
}

/* ---- compile ---- */

static int
fail(char *err, size_t n, const char *m)
{
   if (err && n) snprintf(err, n, "%s", m);
   return -1;
}

/* spirv_to_nir debug sink: keep the last ERROR message (e.g. "Unimplemented SPIR-V capability: X") so the caller sees why. */
struct vtn_diag {
   char msg[200];
};

static void
vtn_diag_cb(void *priv, enum nir_spirv_debug_level level, size_t offset, const char *message)
{
   struct vtn_diag *d = priv;
   if (level != NIR_SPIRV_DEBUG_LEVEL_ERROR)
      return;
   /* The message is multi-line ("SPIR-V parsing FAILED:", "In file ...", the reason, "N bytes into ..."): keep the reason lines. */
   size_t n = 0;
   d->msg[0] = 0;
   for (const char *p = message; *p;) {
      while (*p == ' ' || *p == '\n')
         p++;
      const char *e = strchr(p, '\n');
      size_t len = e ? (size_t)(e - p) : strlen(p);
      if (len && strncmp(p, "SPIR-V parsing", 14) && strncmp(p, "In file", 7) && n + 1 < sizeof d->msg)
         n += snprintf(d->msg + n, sizeof d->msg - n, "%s%.*s", n ? "; " : "", (int)len, p);
      p += len;
   }
   if (n >= sizeof d->msg)
      n = sizeof d->msg - 1;
   (void)offset;
}

static const struct vk_ycbcr_conversion_state *
ycbcr_lookup(const void *data, uint32_t set, uint32_t binding, uint32_t array_index)
{
   const struct mali_compile_layout *l = data;
   if (set >= l->nsets || binding >= l->set[set].nbindings || !l->set[set].ycbcr) return NULL;
   const struct mali_lbinding *b = &l->set[set].b[binding];
   if (!b->ycbcr_planes || array_index >= b->count) return NULL;
   return (const struct vk_ycbcr_conversion_state *)l->set[set].ycbcr + b->ycbcr_off + array_index;
}

/* MALI_OPT_GCM gate. nir_opt_gcm pins ddx/ddy intrinsics and lets implicit-derivative tex only move earlier; belt and braces: only compute
 * shaders without any derivative are touched (the whole measured gain; demote.frag changed bytes in the host corpus). */
static bool
gcm_enabled(nir_shader *nir)
{
   if (!getenv("MALI_OPT_GCM") || nir->info.stage != MESA_SHADER_COMPUTE)
      return false;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex && nir_tex_instr_has_implicit_derivative(nir_instr_as_tex(instr)))
               return false;
            if (instr->type == nir_instr_type_intrinsic) {
               switch (nir_instr_as_intrinsic(instr)->intrinsic) {
               case nir_intrinsic_ddx: case nir_intrinsic_ddy: case nir_intrinsic_ddx_fine: case nir_intrinsic_ddy_fine:
               case nir_intrinsic_ddx_coarse: case nir_intrinsic_ddy_coarse:
                  return false;
               default: break;
               }
            }
         }
      }
   }
   return true;
}

/* The fragment shader's noperspective varying bitfield (VARYING_SLOT_VAR0 upward), i.e. the value
 * nir_intrinsic_load_noperspective_varyings_pan must return in the VS. PanVK inlines it at
 * panfrost/vulkan/panvk_vX_shader.c:1019 (collected once from the FS, pan_nir_collect_varyings.c:218). Our pipeline
 * compiles the VS first and only passes it the varying layout (mali_compile.h link_vs), so the VS had no way to know:
 * the backend still multiplied nothing (pan_nir_lower_noperspective.c:68) while the FS divided by gl_FragCoord.w
 * (pan_nir_lower_fs_inputs.c:167), which renders c/w instead of the affine value. Collecting here, from the same NIR
 * pass PanVK uses, makes the two stages agree by construction: one FS module, one bitfield, both stages.
 * Returns 0 on success and sets *mask; -1 on a parse failure (the FS compile will fail with the same shader). */
static int
fs_noperspective_mask(unsigned arch, const struct mali_compile_opts *o, uint32_t *mask, char *err, size_t errsz)
{
   const nir_shader_compiler_options *opts = pan_get_nir_shader_compiler_options(arch, MESA_SHADER_FRAGMENT, false);
   struct vtn_diag diag = {{0}};
   struct spirv_to_nir_options so = {
      .environment = NIR_SPIRV_VULKAN,
      .debug = {.func = vtn_diag_cb, .private_data = &diag},
      .skip_os_break_in_debug_build = true,
      /* the same address formats as the FS compile below, or a descriptor-heavy FS would parse differently here */
      .ubo_addr_format = o->layout ? MALI_DESC_BUFFER_ADDR_FORMAT : nir_address_format_32bit_index_offset,
      .ssbo_addr_format = o->layout ? MALI_DESC_BUFFER_ADDR_FORMAT : nir_address_format_64bit_global_32bit_offset,
      .phys_ssbo_addr_format = nir_address_format_64bit_global,
      .shared_addr_format = nir_address_format_32bit_offset,
      .min_ubo_alignment = 16, .min_ssbo_alignment = 16,
      .push_const_addr_format = nir_address_format_32bit_offset,
   };
   /* Specialization constants do not select an interpolation qualifier (SPIR-V decorations are outside the module's
    * spec-constant work), so the default values are enough to collect the mask. */
   nir_shader *fs = spirv_to_nir(o->fs_spirv, o->fs_spirv_words, NULL, MESA_SHADER_FRAGMENT,
                                 o->fs_entry ? o->fs_entry : "main", &so, opts);
   if (!fs) {
      if (diag.msg[0] && err && errsz) snprintf(err, errsz, "linked fragment shader: %s", diag.msg);
      else if (err && errsz) snprintf(err, errsz, "linked fragment shader does not parse");
      return -1;
   }
   *mask = pan_nir_collect_noperspective_varyings_fs(fs);
   ralloc_free(fs);
   return 0;
}

static int
do_compile(const struct mali_compile_opts *o, unsigned fau_reserved, unsigned push_base, struct mali_compiled *out,
           char *err, size_t errsz)
{
   pthread_once(&g_once, init_once);
   unsigned arch = o->arch;
   const struct pan_model *model = g_model[arch == 13 ? 2 : arch == 10 ? 0 : 1];
   if (!model)
      return fail(err, errsz, "no GPU model");
   /* One warp width per architecture: only the hardware's own size is a legal request (VUID-requiredSubgroupSize-02414 range is
    * [minSubgroupSize,maxSubgroupSize], and both are this value). Refusing here means a pipeline that asks for 32 can never get
    * 16-wide code, which is what silently corrupts ballot/shuffle/quad lowerings. */
   const unsigned subgroup_size = o->subgroup_size ? o->subgroup_size : mali_subgroup_size_for_arch(arch);
   if (subgroup_size != mali_subgroup_size_for_arch(arch)) {
      char msg[128];
      snprintf(msg, sizeof msg, "subgroup size %u is not supported on arch %u (warp is %u)",
               subgroup_size, arch, mali_subgroup_size_for_arch(arch));
      return fail(err, errsz, msg);
   }
   /* The Bifrost/Valhall backend lowers every subgroup op with pan_subgroup_size(arch) (bifrost_nir.c); a driver table that
    * disagrees would validate one width and emit another. Tie the two together so the mismatch is loud. */
   assert(subgroup_size == pan_subgroup_size(arch));
   mesa_shader_stage stage = o->stage == MALI_STAGE_VERTEX ? MESA_SHADER_VERTEX :
                             o->stage == MALI_STAGE_FRAGMENT ? MESA_SHADER_FRAGMENT : MESA_SHADER_COMPUTE;
   const nir_shader_compiler_options *opts = pan_get_nir_shader_compiler_options(arch, stage, false);

   struct vtn_diag diag = {{0}};
   struct spirv_to_nir_options so = {
      .environment = NIR_SPIRV_VULKAN,
      .debug = {.func = vtn_diag_cb, .private_data = &diag},
      .skip_os_break_in_debug_build = true,   /* a bad shader must fail the compile, not trap a debug build */
      .ubo_addr_format = o->layout ? MALI_DESC_BUFFER_ADDR_FORMAT : nir_address_format_32bit_index_offset,
      .ssbo_addr_format = o->layout ? MALI_DESC_BUFFER_ADDR_FORMAT : nir_address_format_64bit_global_32bit_offset,
      .phys_ssbo_addr_format = nir_address_format_64bit_global,
      .shared_addr_format = nir_address_format_32bit_offset,
      .min_ubo_alignment = 16, .min_ssbo_alignment = 16,
      .push_const_addr_format = nir_address_format_32bit_offset,
   };
   struct nir_spirv_specialization spec = {0};
   struct nir_spirv_specialization_entry *se = NULL;
   if (o->nspec) {
      se = calloc(o->nspec, sizeof *se);
      if (!se)
         return fail(err, errsz, "out of memory");
      for (unsigned i = 0; i < o->nspec; i++)
         se[i] = (struct nir_spirv_specialization_entry){.id = o->spec[i].id, .size = o->spec[i].size,
                                                         .data = (uint8_t *)o->spec_data + o->spec[i].offset};
      spec.num_entries = o->nspec;
      spec.entries = se;
   }
   nir_shader *nir = spirv_to_nir(o->spirv, o->spirv_words, o->nspec ? &spec : NULL, stage, o->entry ? o->entry : "main", &so, opts);
   free(se);
   if (!nir) {
      if (diag.msg[0] && err && errsz)
         snprintf(err, errsz, "spirv_to_nir failed: %s", diag.msg);
      else
         return fail(err, errsz, "spirv_to_nir failed");
      return -1;
   }

   /* VK_EXT_transform_feedback: the SPIR-V sets has_transform_feedback_varyings and the xfb_* decorations; the stores
    * are created later, once IO is lowered to store_output intrinsics (below, where panfrost does the same:
    * src/gallium/drivers/panfrost/pan_shader.c:124-136 runs nir_io_add_intrinsic_xfb_info + nir_lower_xfb_to_stores). */
   if (stage == MESA_SHADER_FRAGMENT && o->sample_shading)
      nir->info.fs.uses_sample_shading = true;
   if (stage == MESA_SHADER_COMPUTE) {
      /* Explicit, so no pass can assume the 32-wide SPIR-V default. The bifrost backend reads pan_subgroup_size(arch) too;
       * keeping both in sync here is what makes a wrong request visible instead of silently changing the mask width. */
      nir->info.api_subgroup_size = subgroup_size;
      nir->info.min_subgroup_size = subgroup_size;
      nir->info.max_subgroup_size = subgroup_size;
   }
   if (stage == MESA_SHADER_VERTEX) {
      bool has_clip = false;
      nir_foreach_shader_out_variable(var, nir) {
         if (var->data.location == VARYING_SLOT_CLIP_DIST0)
            has_clip = true;
         for (unsigned i = 0; i < var->num_members; i++)
            if (var->members[i].location == VARYING_SLOT_CLIP_DIST0)
               has_clip = true;
      }
      if (has_clip)
         NIR_PASS(_, nir, nir_split_per_member_structs);   /* gl_PerVertex block: one variable per member (ClipDistance) */
   }
   if (o->strip_psiz && stage == MESA_SHADER_VERTEX)
      nir_foreach_shader_out_variable(var, nir)
         if (var->data.location == VARYING_SLOT_PSIZ) {
            var->data.mode = nir_var_shader_temp;
            var->data.location = 0;
         }
   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp | nir_var_shader_temp | nir_var_shader_out);   /* shader_temp: DXVK/dxc Private globals with initializers (lower_global_vars_to_local keeps them, vars_to_ssa asserts) */
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   nir_remove_non_entrypoints(nir);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   if (stage != MESA_SHADER_COMPUTE)
      NIR_PASS(_, nir, nir_lower_io_vars_to_temporaries, nir_shader_get_entrypoint(nir), nir_var_shader_out);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_lower_system_values);
   /* Vulkan: FragCoord.xy is x + 0.5 (sample position when sample shading); the hardware reports the integer corner. Before IO lowering. */
   if (stage == MESA_SHADER_FRAGMENT)
      NIR_PASS(_, nir, nir_lower_wpos_center);
   if (stage == MESA_SHADER_VERTEX)
      NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_base_inst, nir_metadata_control_flow, NULL);
   pan_preprocess_nir(nir, arch);

   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const, nir_address_format_32bit_offset);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   struct push_ctx pc = { push_base, 0 };
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, add_push_base, nir_metadata_control_flow, &pc);
   NIR_PASS(_, nir, nir_opt_constant_folding);

   struct pan_compile_inputs in = { .arch = arch, .model = model };
   struct pan_varying_layout vl;
   memset(&vl, 0, sizeof vl);
   const struct pan_varying_layout *vs_layout = NULL;
   unsigned clip_distances = 0;

   if (stage == MESA_SHADER_COMPUTE) {
      NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, nir_var_mem_shared, glsl_get_natural_size_align_bytes);
      NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_shared, nir_address_format_32bit_offset);
      if (nir->info.zero_initialize_shared_memory && nir->info.shared_size > 0) {
         /* shaderZeroInitializeWorkgroupMemory (same recipe as PanVK): zero the whole block, 16-byte chunks */
         nir->info.shared_size = ALIGN_POT(nir->info.shared_size, 16);
         NIR_PASS(_, nir, nir_zero_initialize_shared_memory, nir->info.shared_size, 16);
      }
      /* load_local_invocation_index (and the id generated by the zero-init pass) must be lowered after IO lowering */
      NIR_PASS(_, nir, nir_lower_compute_system_values, NULL);
   } else {
      if (stage == MESA_SHADER_VERTEX) {
         /* gl_ClipDistance: written as ordinary varyings (VARYING_SLOT_CLIP_DIST0/1); the FS kills where it is < 0 (nir_lower_clip_fs) */
         nir_gather_clip_cull_distance_sizes_from_vars(nir);
         if (nir->info.clip_distance_array_size)
            NIR_PASS(_, nir, nir_merge_clip_cull_distance_vars);
         clip_distances = nir->info.clip_distance_array_size;
         nir_foreach_shader_in_variable(var, nir)
            var->data.driver_location = var->data.location - VERT_ATTRIB_GENERIC0;
         nir_assign_io_var_locations(nir, nir_var_shader_out);
      } else {
         nir_assign_io_var_locations(nir, nir_var_shader_in);
         nir_assign_io_var_locations(nir, nir_var_shader_out);
         /* after the location assignment: the pass emits load_interpolated_input intrinsics with the var's driver_location as base */
         if (o->clip_distances)
            NIR_PASS(_, nir, nir_lower_clip_fs, (1u << o->clip_distances) - 1, true, true);
      }
      NIR_PASS(_, nir, nir_lower_var_copies);
      NIR_PASS(_, nir, nir_lower_indirect_derefs_to_if_else_trees, nir_var_shader_in | nir_var_shader_out, UINT32_MAX);
      NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out, type_size,
               nir_lower_io_use_interpolated_input_intrinsics);
      NIR_PASS(_, nir, nir_opt_constant_folding);
      if (stage == MESA_SHADER_VERTEX && !o->xfb && nir->info.has_transform_feedback_varyings) {
         /* The layout was built without the 32 B address area, so the stores would read bases that were never
          * written. Refuse with a name instead of compiling the XFB semantics away silently (the #296 trap). */
         free(out->bin); free(out->imm);
         ralloc_free(nir);
         return fail(err, errsz, "transform feedback varyings without a reserved push-constant area");
      }
      if (stage == MESA_SHADER_VERTEX && o->xfb && nir->info.has_transform_feedback_varyings) {
         /* The lowered stores index their buffer with instance_id * num_vertices + raw_vertex_id
          * (nir_lower_xfb_to_stores.c:44-46) and read the base with load_xfb_address. Panfrost answers those from
          * driver sysvals (pan_cmdstream.c PAN_SYSVAL_NUM_VERTICES / PAN_SYSVAL_XFB); this driver has no sysval for
          * either, so raw_vertex_id becomes the ordinary vertex ID and num_vertices 0, and the per-buffer base is
          * the push-constant slot the draw fills (mali_cmd.c: the bound buffer address + offset). That is exact for
          * instance_count == 1, and instanced XFB is refused at draw time (mali_cmd.c) rather than overlapping. */
         /* Transform feedback: turn the declared outputs into global stores (nir_lower_xfb_to_stores.c:53 loads the
          * per-buffer base with nir_intrinsic_load_xfb_address). No streamout job is needed: the stores go through the
          * ordinary memory path, exactly like an SSBO store. */
         static const nir_lower_xfb_to_stores_options xfb_opts = {.address_format = nir_address_format_64bit_global,
                                                           /* the same vertex shader still rasterizes: a captured output the
                                                            * rasterizer or the fragment shader reads must stay written */
                                                           .keep_outputs = true};
         nir_shader_gather_xfb_info(nir);
         NIR_PASS(_, nir, nir_opt_constant_folding);
         NIR_PASS(_, nir, nir_io_add_intrinsic_xfb_info);
         NIR_PASS(_, nir, nir_lower_xfb_to_stores, &xfb_opts);
         /* the two sysvals the lowering introduced, replaced before the backend sees them */
         nir_foreach_block(blk, nir_shader_get_entrypoint(nir)) {
            nir_foreach_instr_safe(ins, blk) {
               if (ins->type != nir_instr_type_intrinsic)
                  continue;
               nir_intrinsic_instr *it = nir_instr_as_intrinsic(ins);
               nir_builder b = nir_builder_at(nir_before_instr(ins));
               nir_def *repl = NULL;
               if (it->intrinsic == nir_intrinsic_load_raw_vertex_id)
                  repl = nir_load_vertex_id(&b);
               else if (it->intrinsic == nir_intrinsic_load_num_vertices)
                  repl = nir_imm_int(&b, 0);
               else if (it->intrinsic == nir_intrinsic_load_xfb_address) {
                  /* The four buffer addresses sit in the push block right after the application's constants
                   * (o->xfb_pc + k*8); mali_cmd.c fills them at draw time from the bound
                   * VkBindTransformFeedbackBuffersEXT buffers. */
                  const unsigned k = nir_intrinsic_base(it);
                  /* The builder helper, like the sysval lowering above (mali_compile.c:93); the offset is src[0]
                   * and must already include push_base, because add_push_base() has run: the emitter copies the
                   * application's block to FAU byte push_base and this API-only area follows it. */
                  repl = nir_load_push_constant(&b, 1, 64, nir_imm_int(&b, push_base + o->xfb_pc + k * 8));
               }
               if (repl)
                  nir_def_replace(&it->def, repl);
            }
         }
         /* add_push_base() already ran, so it did not see these loads: the block the emitter copies must reach the
          * last address word, or the XFB bases never leave the draw record. */
         pc.max_end = MAX2(pc.max_end, o->xfb_pc + 32);
         if (getenv("MALI_TF_DBG")) {
            unsigned nst = 0, nld = 0, nraw = 0;
            nir_foreach_block(blk2, nir_shader_get_entrypoint(nir))
               nir_foreach_instr(ins2, blk2) {
                  if (ins2->type != nir_instr_type_intrinsic)
                     continue;
                  nir_intrinsic_instr *i2 = nir_instr_as_intrinsic(ins2);
                  if (i2->intrinsic == nir_intrinsic_store_global) nst++;
                  else if (i2->intrinsic == nir_intrinsic_load_push_constant) nld++;
                  else if (i2->intrinsic == nir_intrinsic_load_xfb_address) nraw++;
               }
            fprintf(stderr, "mali_tf: compiled xfb %d xfb_pc %u push_base %u push_bytes %u stores %u push_loads %u raw_xfb %u\n",
                    (int)nir->info.has_transform_feedback_varyings, o->xfb_pc, push_base, pc.max_end, nst, nld, nraw);
         }
      }
      pan_nir_lower_mediump_io(nir);

      if (stage == MESA_SHADER_VERTEX) {
         pan_varying_collect_formats(&vl, nir, arch);
         pan_build_varying_layout_compact(&vl, nir, arch);
         in.varying_layout = &vl;
         vs_layout = &vl;
      } else {
         /* nir_shader_gather_info is never run here, so info.outputs_written is empty and pan_nir_lower_fs_outputs only adds the colour
          * bits. Record gl_FragDepth / stencil-export writes before that pass turns them into zs_emit_pan (which nothing gathers):
          * the compiler derives fs.writes_depth/writes_stencil (and can_early_z/can_fpk) from these bits. Without them the draw is
          * emitted with fixed-function depth and early ZS, and the shader's depth/stencil never reaches the tile buffer. */
         nir_foreach_block(blk, nir_shader_get_entrypoint(nir))
            nir_foreach_instr(ins, blk) {
               if (ins->type != nir_instr_type_intrinsic || nir_instr_as_intrinsic(ins)->intrinsic != nir_intrinsic_store_output)
                  continue;
               unsigned loc = nir_intrinsic_io_semantics(nir_instr_as_intrinsic(ins)).location;
               if (loc == FRAG_RESULT_DEPTH || loc == FRAG_RESULT_STENCIL)
                  nir->info.outputs_written |= BITFIELD64_BIT(loc);
            }
         NIR_PASS(_, nir, pan_nir_lower_fs_outputs, false, 0);
         if (o->link_vs && o->link_vs->size == sizeof vl) {
            memcpy(&vl, o->link_vs->data, sizeof vl);
            in.varying_layout = &vl;
         }
      }
   }

   /* bufferDeviceAddress (PhysicalStorageBuffer derefs) in every stage, not only compute */
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_global, nir_address_format_64bit_global);

   if (o->layout) {
      /* YCbCr (NV12): before the descriptors are lowered, while the texture derefs still name set/binding/array index. It turns each
       * sample of a converted combined image sampler into per-plane samples (nir_tex_src_plane) plus the model/range/siting maths. */
      NIR_PASS(_, nir, nir_vk_lower_ycbcr_tex, ycbcr_lookup, o->layout);
      if (!mali_nir_lower_descriptors(nir, o->layout, err, errsz)) {
         ralloc_free(nir);
         if (err && errsz && !err[0]) snprintf(err, errsz, "descriptor lowering failed");
         return -1;
      }
   } else {
      NIR_PASS(_, nir, nir_shader_instructions_pass, lower_tex_handles, nir_metadata_control_flow, NULL);
   }
   nir_remove_dead_variables(nir, nir_var_uniform | nir_var_image, NULL);

   struct pan_shader_info info = {0};
   /* Experimental, off by default: global code motion + value numbering (-5.2% instructions on the Steam/gamescope/bench corpus, all in compute). The disk cache is bypassed while set. */
   if (gcm_enabled(nir)) {
      NIR_PASS(_, nir, nir_opt_gcm, true);
      NIR_PASS(_, nir, nir_opt_dce);
   }
   /* pan_nir_lower_noperspective_vs (bifrost_nir.c:884) returns immediately unless info.outputs_written names
    * gl_Position (pan_nir_lower_noperspective.c:129). This driver never runs nir_shader_gather_info (the FS branch
    * below works around the same gap), so that bit was empty: the pass never inserted the "mask ? v * w : v" and the
    * mask inlined next had nothing to select. Set exactly the bit the guard reads, from the lowered POS store. */
   if (stage == MESA_SHADER_VERTEX) {
      nir_foreach_block(blk, nir_shader_get_entrypoint(nir))
         nir_foreach_instr(ins, blk) {
            if (ins->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *it = nir_instr_as_intrinsic(ins);
            if (it->intrinsic == nir_intrinsic_store_output &&
                nir_intrinsic_io_semantics(it).location == VARYING_SLOT_POS)
               nir->info.outputs_written |= BITFIELD64_BIT(VARYING_SLOT_POS);
         }
   }
   pan_postprocess_nir(nir, &in, &info);
   /* pan_postprocess_nir -> bifrost_nir.c:884 has just wrapped every non-special output store in
    * "mask ? v * w : v" (pan_nir_lower_noperspective.c:68) with the mask loaded from the sysval; inline the FS's
    * bitfield before lower_sysvals (which has no answer for this sysval) and before the backend emits IDVS words. */
   uint32_t nv_mask = 0, nv_loads = 0;
   if (stage == MESA_SHADER_VERTEX) {
      /* One sysval load per output store the pass wrapped: it is the multiplier this inline selects, so 0 means the
       * pass still bailed (that is what a missing outputs_written POS bit did) even with the mask right. */
      nir_foreach_block(blk, nir_shader_get_entrypoint(nir))
         nir_foreach_instr(ins, blk)
            if (ins->type == nir_instr_type_intrinsic &&
                nir_instr_as_intrinsic(ins)->intrinsic == nir_intrinsic_load_noperspective_varyings_pan)
               nv_loads++;
      if (o->fs_spirv && o->fs_spirv_words &&
          fs_noperspective_mask(arch, o, &nv_mask, err, errsz)) {
         ralloc_free(nir);   /* err already names the reason */
         return -1;
      }
      NIR_PASS(_, nir, nir_inline_sysval, nir_intrinsic_load_noperspective_varyings_pan, nv_mask);
   } else if (stage == MESA_SHADER_FRAGMENT) {
      nv_mask = info.varyings.noperspective;
   }
   if (getenv("MALI_INTERP_DBG"))
      fprintf(stderr, "mali_interp: stage=%s mask=%08x inlined=%d sel=%u\n",
              stage == MESA_SHADER_VERTEX ? "VS" : stage == MESA_SHADER_FRAGMENT ? "FS" : "CS", nv_mask,
              stage == MESA_SHADER_VERTEX && o->fs_spirv ? 1 : 0, nv_loads);
   bool can_merge = stage == MESA_SHADER_COMPUTE && arch >= 9 && !getenv("MALI_NO_MERGE") && valhall_can_merge_workgroups(nir);
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_sysvals, nir_metadata_control_flow, NULL);
   in.fau.reserved = fau_reserved;
   in.fau.promote_immediates = true;

   struct util_dynarray bin;
   util_dynarray_init(&bin, NULL);
   pan_shader_compile(nir, &in, &bin, &info);

   memset(out, 0, sizeof *out);
   out->arch = arch;
   out->stage = o->stage;
   out->noperspective_mask = nv_mask;
   out->bin_size = util_dynarray_num_elements(&bin, uint8_t);
   out->bin = malloc(out->bin_size ? out->bin_size : 1);
   if (!out->bin) {
      util_dynarray_fini(&bin);
      ralloc_free(nir);
      return fail(err, errsz, "out of memory");
   }
   memcpy(out->bin, util_dynarray_begin(&bin), out->bin_size);
   util_dynarray_fini(&bin);

   if (stage == MESA_SHADER_COMPUTE)
      for (int i = 0; i < 3; i++) out->local_size[i] = nir->info.workgroup_size[i];
   out->allow_merge = can_merge;
   out->subgroup_size = subgroup_size;
   out->work_reg_count = info.work_reg_count;
   out->tls_size = info.tls_size;
   out->wls_size = info.wls_size;
   out->preload = info.preload;
   out->fau_count = info.fau.count;
   out->fau_reserved = info.fau.reserved;
   for (unsigned i = info.fau.reserved; i < info.fau.count; i++)
      if (BITSET_TEST(info.fau.is_const, i))
         out->nimm++;
   out->imm = malloc((out->nimm ? out->nimm : 1) * sizeof *out->imm);
   unsigned k = 0;
   for (unsigned i = info.fau.reserved; i < info.fau.count && out->imm; i++)
      if (BITSET_TEST(info.fau.is_const, i)) {
         out->imm[k].word = i;
         out->imm[k].value = info.fau.words[i].constant;
         k++;
      }
   out->push_const_bytes = pc.max_end;
   if (o->layout && pc.max_end != UINT32_MAX && pc.max_end > o->layout->push_size) {
      free(out->bin); free(out->imm);
      ralloc_free(nir);
      return fail(err, errsz, "shader reads push constants beyond the layout's push range");
   }
   out->ubo_count = info.ubo_count;
   out->attribute_count = info.attribute_count;
   out->texture_count = info.texture_count;
   out->sampler_count = info.sampler_count;
   out->uses_ld_var = info.bifrost.uses_ld_var;
   out->wait_6 = info.bifrost.wait_6;
   out->wait_7 = info.bifrost.wait_7;
   out->writes_global = info.writes_global;
   if (stage == MESA_SHADER_FRAGMENT) {
      out->fs_can_discard = info.fs.can_discard;
      out->fs_writes_depth = info.fs.writes_depth;
      out->fs_writes_stencil = info.fs.writes_stencil;
      out->fs_writes_colour = (info.outputs_written >> 4 /* FRAG_RESULT_DATA0 */) & 0xff ? 1 : 0;
      out->fs_early_fragment_tests = info.fs.early_fragment_tests;
      out->fs_can_early_z = info.fs.can_early_z;
      out->fs_can_fpk = info.fs.can_fpk;
      out->fs_sidefx = info.fs.sidefx;
      out->fs_reads_frag_coord = info.fs.reads_frag_coord;
      out->fs_sample_shading = info.fs.sample_shading;
      out->fs_blend0_type = info.bifrost.blend[0].type;
      out->fs_blend_src1_type = info.bifrost.blend_src1_type;
   } else if (stage == MESA_SHADER_VERTEX) {
      out->vs_writes_point_size = info.vs.writes_point_size;
      out->vs_secondary_enable = info.vs.secondary_enable;
      out->vs_secondary_offset = info.vs.secondary_offset;
      out->vs_secondary_work_reg_count = info.vs.secondary_work_reg_count;
      out->vs_secondary_preload = info.vs.secondary_preload;
      out->generic_size_B = vl.generic_size_B;
      out->vs_clip_distances = clip_distances;
      out->layout.size = sizeof vl;
      memcpy(out->layout.data, &vl, sizeof vl);
   }
   if (stage != MESA_SHADER_COMPUTE) {
      const struct pan_varying_layout *L = vs_layout ? vs_layout : &info.varyings.formats;
      for (unsigned i = 0; i < L->count && out->nvarying < 64; i++) {
         const struct pan_varying_slot *s = pan_varying_layout_slot_at(L, i);
         if (!s) continue;
         struct mali_varying_slot *d = &out->varying[out->nvarying++];
         d->location = s->location;
         d->section = s->section;
         d->ncomps = s->ncomps;
         d->offset = s->offset;
         d->alu_type = s->alu_type;
      }
   }
   ralloc_free(nir);
   if (!out->imm) {
      free(out->bin);
      return fail(err, errsz, "out of memory");
   }
   return 0;
}

int
mali_spirv_has_xfb(const uint32_t *w, uint32_t nwords)
{
   /* OpExecutionMode (16) <id> Xfb (11): the mode the SPIR-V spec requires on any module with XFB decorations,
    * and the exact word spirv_to_nir.c:6110 turns into info.has_transform_feedback_varyings, so this scan and the
    * lowering always agree. The instruction stream starts after the five header words (magic, version, generator,
    * bound, schema); scanning from word 0 reads the magic as a 1827-word instruction and finds nothing (which is
    * what silently disabled the whole path in 29). */
   for (uint32_t i = 5; i + 2 < nwords;) {
      const uint32_t wc = w[i] >> 16;
      if (!wc)
         break;
      if ((w[i] & 0xffff) == 16 && w[i + 2] == 11)
         return 1;
      i += wc;
   }
   return 0;
}

int
mali_compile(const struct mali_compile_opts *o, struct mali_compiled *out, char *err, size_t errsz)
{
   if (!o || !out || !o->spirv || !o->spirv_words)
      return fail(err, errsz, "bad arguments");
   /* spirv_to_nir dereferences a NULL builder option on a bad header: check it ourselves */
   if (o->spirv_words < 5 || o->spirv[0] != 0x07230203u)
      return fail(err, errsz, "not a SPIR-V module (bad magic or too short)");
   if (o->arch != 10 && o->arch != 11 && o->arch != 13)
      return fail(err, errsz, "arch must be 10, 11 or 13");
   if (o->stage > MALI_STAGE_COMPUTE)
      return fail(err, errsz, "bad stage");
   unsigned fau_reserved = o->fau_reserved ? o->fau_reserved : (o->stage == MALI_STAGE_VERTEX ? 24 : (o->arch == 13 && o->stage == MALI_STAGE_FRAGMENT ? 16 : 8));
   unsigned push_base = o->push_base_set ? o->push_base : (o->stage == MALI_STAGE_VERTEX ? 40 : (o->arch == 13 && o->stage == MALI_STAGE_FRAGMENT ? 24 : 0)); /* v13 FS: blend words 4,5 live in the FAU */
   /* The API push block is driver-owned FAU space, so the reservation must cover it: the compiler places promoted immediates from
    * `fau_reserved` up, and the draw/dispatch emitter writes the immediates and then the push block into the same table
    * (mali_cs_emit.c kb_draw_fau_imm + memcpy). With only the default reservation, an immediate lands in the push block for a
    * block above 56 B in the VS (words 10..) or 32 B in the CS/FS-v11 (word 0), and one of the two is silently corrupted. */
   if (!o->fau_reserved && o->layout && o->layout->push_size) {
      unsigned push_end = (push_base + o->layout->push_size + 3) / 4;
      fau_reserved = MAX2(fau_reserved, push_end);
   }
   if (fau_reserved > PAN_MAX_PUSH)   /* 64 FAU slots of 64 bits: the Bifrost/Valhall implementation limit (pan_compiler.h) */
      return fail(err, errsz, "push constants leave no FAU space for immediates");

   char dir[900], hex[65];
   int use_cache = cache_dir(o, dir, sizeof dir) && !getenv("MALI_OPT_GCM");
   if (use_cache) {
      mkey(o, fau_reserved, push_base, hex);
      if (cache_load(dir, hex, out)) {
         atomic_fetch_add(&g_hits, 1);
         return 0;
      }
   }
   atomic_fetch_add(&g_misses, 1);
   int r = do_compile(o, fau_reserved, push_base, out, err, errsz);
   if (r == 0 && use_cache)
      cache_store(dir, hex, out);
   return r;
}

void
mali_compiled_free(struct mali_compiled *c)
{
   if (!c) return;
   free(c->bin);
   free(c->imm);
   memset(c, 0, sizeof *c);
}

void
mali_compile_stats(struct mali_compile_stats *s)
{
   s->hits = g_hits;
   s->misses = g_misses;
   s->stores = g_stores;
}

/* ---- blend shader ---- */
nir_shader *pan_blend_create_shader_v10(const struct pan_blend_state *, nir_alu_type, nir_alu_type, unsigned);
nir_shader *pan_blend_create_shader_v11(const struct pan_blend_state *, nir_alu_type, nir_alu_type, unsigned);
nir_shader *pan_blend_create_shader_v13(const struct pan_blend_state *, nir_alu_type, nir_alu_type, unsigned);

static bool
lower_load_blend_const(nir_builder *b, nir_instr *instr, UNUSED void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_load_blend_const_color_rgba)
      return false;
   b->cursor = nir_before_instr(instr);
   /* blend constants live in FAU words 0..3 (written by the driver at draw time) */
   nir_def *c = nir_load_push_constant(b, intr->def.num_components, intr->def.bit_size, nir_imm_int(b, 0));
   nir_def_rewrite_uses(&intr->def, c);
   return true;
}

int
mali_compile_blend(const struct mali_blend_opts *o, struct mali_compiled *out, char *err, size_t errsz)
{
   if (!o || !out || (o->arch != 10 && o->arch != 11 && o->arch != 13))
      return fail(err, errsz, "bad blend arguments");
   pthread_once(&g_once, init_once);
   unsigned arch = o->arch;
   const struct pan_model *model = g_model[arch == 13 ? 2 : arch == 10 ? 0 : 1];
   if (!model)
      return fail(err, errsz, "no GPU model");

   struct pan_blend_state st = {0};
   st.rt_count = 1;
   struct pan_blend_rt_state *rt = &st.rts[0];
   rt->format = vk_format_to_pipe_format((VkFormat)o->vk_format);
   rt->nr_samples = o->samples ? o->samples : 1;
   rt->equation.blend_enable = true;
   rt->equation.is_float = util_format_is_float(rt->format);
   rt->equation.rgb_func = vk_blend_op_to_pipe((VkBlendOp)o->rgb_op);
   rt->equation.rgb_src_factor = vk_blend_factor_to_pipe((VkBlendFactor)o->rgb_src);
   rt->equation.rgb_dst_factor = vk_blend_factor_to_pipe((VkBlendFactor)o->rgb_dst);
   rt->equation.alpha_func = vk_blend_op_to_pipe((VkBlendOp)o->alpha_op);
   rt->equation.alpha_src_factor = vk_blend_factor_to_pipe((VkBlendFactor)o->alpha_src);
   rt->equation.alpha_dst_factor = vk_blend_factor_to_pipe((VkBlendFactor)o->alpha_dst);
   rt->equation.color_mask = o->color_mask & 0xf;
   st.logicop_enable = o->logic_op_enable != 0;
   if (st.logicop_enable)   /* an unused logicOp may hold any value (spec): convert only when it applies */
      st.logicop_func = vk_logic_op_to_pipe((VkLogicOp)o->logic_op);

   nir_shader *nir = arch == 13   ? pan_blend_create_shader_v13(&st, o->src0_type, o->src1_type, 0)
                     : arch == 10 ? pan_blend_create_shader_v10(&st, o->src0_type, o->src1_type, 0)
                                  : pan_blend_create_shader_v11(&st, o->src0_type, o->src1_type, 0);
   if (!nir)
      return fail(err, errsz, "pan_blend_create_shader failed");
   NIR_PASS(_, nir, nir_shader_instructions_pass, lower_load_blend_const, nir_metadata_control_flow, NULL);

   struct pan_compile_inputs in = { .arch = arch, .model = model, .is_blend = true };
   in.fau.reserved = 4;
   pan_preprocess_nir(nir, arch);
   struct pan_shader_info info = {0};
   pan_postprocess_nir(nir, &in, &info);

   struct util_dynarray bin;
   util_dynarray_init(&bin, NULL);
   pan_shader_compile(nir, &in, &bin, &info);

   memset(out, 0, sizeof *out);
   out->arch = arch;
   out->stage = MALI_STAGE_FRAGMENT;
   out->bin_size = util_dynarray_num_elements(&bin, uint8_t);
   out->bin = malloc(out->bin_size ? out->bin_size : 1);
   if (!out->bin) {
      util_dynarray_fini(&bin);
      ralloc_free(nir);
      return fail(err, errsz, "out of memory");
   }
   memcpy(out->bin, util_dynarray_begin(&bin), out->bin_size);
   util_dynarray_fini(&bin);
   out->work_reg_count = info.work_reg_count;
   out->tls_size = info.tls_size;
   out->preload = info.preload;
   out->fau_count = info.fau.count;
   out->fau_reserved = info.fau.reserved;
   out->imm = malloc(sizeof *out->imm);
   ralloc_free(nir);
   if (!out->imm) {
      free(out->bin);
      return fail(err, errsz, "out of memory");
   }
   return 0;
}
