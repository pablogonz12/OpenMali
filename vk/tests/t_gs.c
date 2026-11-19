/* SPDX-License-Identifier: MIT */
/* Offline tests of the GS front half (mali_gs.c): what a GS module declares, the record/index layout, and the
 * passthrough vertex shader. No GPU, no pipeline: see docs/gs-design.md for the remaining stages. */
#include "../src/mali_compile.h"
#include "../src/mali_gs.h"

#include "nir.h"
#include "nir_constant_expressions.h"
#include "panfrost/compiler/pan_compiler.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static const char *g_dir = "driver/vk/shader";

struct spv { uint32_t *w; size_t n; };

static struct spv
load(const char *name)
{
   char p[512];
   snprintf(p, sizeof p, "%s/%s", g_dir, name);
   struct spv s = {0};
   FILE *f = fopen(p, "rb");
   if (!f) { printf("  cannot open %s\n", p); g_fail++; return s; }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   s.w = malloc(n);
   if (fread(s.w, 1, n, f) != (size_t)n) { g_fail++; free(s.w); s.w = NULL; }
   else s.n = n / 4;
   fclose(f);
   return s;
}

static void
analyze(const char *what, unsigned arch, const struct spv *s, struct mali_gs_info *gs)
{
   struct mali_compile_opts o = { .arch = arch, .stage = MALI_STAGE_COMPUTE, .spirv = s->w, .spirv_words = s->n };
   char err[256] = "";
   int r = mali_gs_analyze(&o, gs, err, sizeof err);
   CHECK(r == 0, "%s (arch %u): analyze failed: %s", what, arch, err);
}

static int
output_index(const struct mali_gs_info *gs, unsigned location)
{
   for (unsigned i = 0; i < gs->noutputs; i++)
      if (gs->output[i].location == location)
         return (int)i;
   return -1;
}

static void
test_basic(unsigned arch)
{
   struct spv s = load("gs/basic.geom.spv");
   if (!s.w)
      return;
   struct mali_gs_info gs;
   analyze("basic", arch, &s, &gs);
   CHECK(gs.vertices_out == 3, "basic: vertices_out %u != 3", gs.vertices_out);
   CHECK(gs.invocations == 1, "basic: invocations %u != 1", gs.invocations);
   CHECK(gs.input_prim == MESA_PRIM_TRIANGLES, "basic: input prim %u != triangles", gs.input_prim);
   CHECK(gs.output_prim == MESA_PRIM_TRIANGLE_STRIP, "basic: output prim %u != triangle strip", gs.output_prim);
   CHECK(gs.noutputs == 2, "basic: %u outputs != 2", gs.noutputs);
   CHECK(gs.outputs_written & VARYING_BIT_POS, "basic: no gl_Position output");
   CHECK(output_index(&gs, VARYING_SLOT_VAR0) >= 0, "basic: out_col missing");
   int pos = output_index(&gs, VARYING_SLOT_POS);
   CHECK(pos >= 0 && gs.output[pos].ncomps == 4, "basic: gl_Position must take a 4-component slot");
   int col = output_index(&gs, VARYING_SLOT_VAR0);
   CHECK(col >= 0 && gs.output[col].ncomps == 3, "basic: out_col must take 3 components");
   printf("  basic v%u: %u outputs, max_vertices %u, invocations %u, in %u -> out %u, record %u B, index %u u32\n",
          arch, gs.noutputs, gs.vertices_out, gs.invocations, gs.input_prim, gs.output_prim,
          gs.record_stride, gs.index_stride);
   CHECK(gs.record_stride == 32, "basic: record stride %u != 32 (2 slots x 16)", gs.record_stride);
   CHECK(gs.index_stride == 8, "basic: index stride %u != 8 (2 * 3 padded)", gs.index_stride);
   free(s.w);
}

static void
test_points(unsigned arch)
{
   struct spv s = load("gs/points.geom.spv");
   if (!s.w)
      return;
   struct mali_gs_info gs;
   analyze("points", arch, &s, &gs);
   CHECK(gs.vertices_out == 4, "points: vertices_out %u != 4", gs.vertices_out);
   CHECK(gs.input_prim == MESA_PRIM_POINTS, "points: input prim %u != points", gs.input_prim);
   CHECK(gs.output_prim == MESA_PRIM_TRIANGLE_STRIP, "points: output prim %u != triangle strip", gs.output_prim);
   CHECK(gs.record_stride == 32, "points: record stride %u != 32", gs.record_stride);
   CHECK(gs.index_stride == 8, "points: index stride %u != 8", gs.index_stride);
   free(s.w);
}

static void
test_layout(void)
{
   struct mali_gs_info gs = { .noutputs = 2, .vertices_out = 3, .invocations = 1 };
   mali_gs_layout(&gs, 0);
   const uint64_t arena = 256 * 1024;
   unsigned cap = mali_gs_cap_slots(&gs, arena);
   CHECK(cap > 0, "layout: no slots in a 256 KiB arena");
   /* records and indices must both fit: cap * (stride + index_stride * 4) <= arena */
   uint64_t need = (uint64_t)cap * (gs.record_stride * (uint64_t)gs.vertices_out +
                                    4ull * gs.index_stride);
   CHECK(need <= arena, "layout: %u slots need %llu bytes of a %llu-byte arena", cap,
         (unsigned long long)need, (unsigned long long)arena);
}

/* The passthrough VS must read the record base from the params block (push constant at pc_off + 8, the second
 * 64-bit field of struct mali_gs_params) and reproduce exactly the GS's outputs. */
static void
test_passthrough(unsigned arch)
{
   struct spv s = load("gs/basic.geom.spv");
   if (!s.w)
      return;
   struct mali_gs_info gs;
   analyze("basic-pt", arch, &s, &gs);
   const unsigned pc_off = 64;   /* whatever the driver appends after the app's push range */
   const struct nir_shader_compiler_options *opts =
      pan_get_nir_shader_compiler_options(arch, MESA_SHADER_VERTEX, false);
   nir_shader *vs = mali_gs_passthrough_nir(opts, &gs, pc_off);
   CHECK(vs != NULL, "passthrough: builder returned NULL");
   if (!vs) { free(s.w); return; }
   CHECK(vs->info.stage == MESA_SHADER_VERTEX, "passthrough: stage");
   nir_shader_gather_info(vs, nir_shader_get_entrypoint(vs));
   CHECK(vs->info.outputs_written == (gs.outputs_written & ~VARYING_BIT_VIEWPORT),
         "passthrough: outputs 0x%llx != GS outputs 0x%llx",
         (unsigned long long)vs->info.outputs_written, (unsigned long long)gs.outputs_written);
   CHECK(vs->num_outputs == gs.noutputs, "passthrough: %u outputs != %u", vs->num_outputs, gs.noutputs);

   bool base_ok = false;
   nir_foreach_block(block, nir_shader_get_entrypoint(vs))
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_load_push_constant)
            continue;
         if (nir_src_is_const(intr->src[0]) && nir_src_as_uint(intr->src[0]) == pc_off + 8)
            base_ok = true;
      }
   CHECK(base_ok, "passthrough: no load of struct mali_gs_params.records at pc_off + 8");
   ralloc_free(vs);
   free(s.w);
}


/* ---- a tiny NIR interpreter: enough to run the kernel and the passthrough VS on the CPU ----
 * Global addresses are host pointers (x86-64 test only). Registers (after nir_convert_from_ssa) live in the
 * value of their decl_reg. */
enum { J_NONE, J_BREAK, J_CONT, J_RET };
struct ex {
   nir_const_value (*v)[NIR_MAX_VEC_COMPONENTS];
   const uint8_t *push;
   uint32_t gid[3], vertex_id;
   float out[VARYING_SLOT_MAX][4];
   int bad;
};

static uint64_t
ev64(const struct ex *e, nir_def *d)
{
   return e->v[d->index][0].u64;
}

static int
exec_list(struct ex *e, struct exec_list *l);

static void
exec_instr(struct ex *e, nir_instr *instr)
{
   switch (instr->type) {
   case nir_instr_type_load_const: {
      nir_load_const_instr *lc = nir_instr_as_load_const(instr);
      for (unsigned c = 0; c < lc->def.num_components; c++)
         e->v[lc->def.index][c] = lc->value[c];
      break;
   }
   case nir_instr_type_undef: {
      nir_undef_instr *u = nir_instr_as_undef(instr);
      memset(e->v[u->def.index], 0, sizeof e->v[0]);
      break;
   }
   case nir_instr_type_alu: {
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      nir_const_value srcv[4][NIR_MAX_VEC_COMPONENTS], *sp[4];
      memset(srcv, 0, sizeof srcv);
      unsigned n = alu->def.num_components;
      for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
         unsigned sz = nir_op_infos[alu->op].input_sizes[i] ? nir_op_infos[alu->op].input_sizes[i] : n;
         for (unsigned c = 0; c < sz; c++)
            srcv[i][c] = e->v[alu->src[i].src.ssa->index][alu->src[i].swizzle[c]];
         sp[i] = srcv[i];
      }
      nir_const_value dst[NIR_MAX_VEC_COMPONENTS];
      memset(dst, 0, sizeof dst);
      unsigned bs = nir_alu_type_get_type_size(nir_op_infos[alu->op].output_type);
      if (!bs)
         bs = nir_op_infos[alu->op].num_inputs ? alu->src[0].src.ssa->bit_size : alu->def.bit_size;
      /* nir_eval_const_opcode takes the *destination* bit size for the opcode's output, sources are typed. */
      nir_eval_const_opcode(alu->op, dst, NULL, n, alu->def.bit_size, sp, 0);
      for (unsigned c = 0; c < n; c++)
         e->v[alu->def.index][c] = dst[c];
      (void)bs;
      break;
   }
   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *i = nir_instr_as_intrinsic(instr);
      switch (i->intrinsic) {
      case nir_intrinsic_load_global: {
         const uint32_t *p = (const uint32_t *)(uintptr_t)ev64(e, i->src[0].ssa);
         for (unsigned c = 0; c < i->def.num_components; c++)
            e->v[i->def.index][c].u32 = p[c];
         break;
      }
      case nir_intrinsic_store_global: {
         uint32_t *p = (uint32_t *)(uintptr_t)ev64(e, i->src[1].ssa);
         for (unsigned c = 0; c < i->src[0].ssa->num_components; c++)
            if (nir_intrinsic_write_mask(i) & (1u << c))
               p[c] = e->v[i->src[0].ssa->index][c].u32;
         break;
      }
      case nir_intrinsic_load_push_constant: {
         unsigned off = e->v[i->src[0].ssa->index][0].u32 + nir_intrinsic_base(i);
         for (unsigned c = 0; c < i->def.num_components; c++)
            memcpy(&e->v[i->def.index][c].u32, e->push + off + 4 * c, 4);
         break;
      }
      case nir_intrinsic_load_global_invocation_id:
         for (unsigned c = 0; c < 3; c++)
            e->v[i->def.index][c].u32 = e->gid[c];
         break;
      case nir_intrinsic_load_vertex_id:
         e->v[i->def.index][0].u32 = e->vertex_id;
         break;
      case nir_intrinsic_store_output: {
         nir_io_semantics sem = nir_intrinsic_io_semantics(i);
         for (unsigned c = 0; c < i->src[0].ssa->num_components; c++)
            if (nir_intrinsic_write_mask(i) & (1u << c))
               memcpy(&e->out[sem.location][nir_intrinsic_component(i) + c],
                      &e->v[i->src[0].ssa->index][c].u32, 4);
         break;
      }
      case nir_intrinsic_decl_reg:
         memset(e->v[i->def.index], 0, sizeof e->v[0]);
         break;
      case nir_intrinsic_load_reg:
         for (unsigned c = 0; c < i->def.num_components; c++)
            e->v[i->def.index][c] = e->v[i->src[0].ssa->index][c];
         break;
      case nir_intrinsic_store_reg:
         for (unsigned c = 0; c < i->src[0].ssa->num_components; c++)
            if (nir_intrinsic_write_mask(i) & (1u << c))
               e->v[i->src[1].ssa->index][c] = e->v[i->src[0].ssa->index][c];
         break;
      default:
         printf("  interp: unhandled intrinsic %s\n", nir_intrinsic_infos[i->intrinsic].name);
         e->bad = 1;
      }
      break;
   }
   default:
      printf("  interp: unhandled instr type %d\n", instr->type);
      e->bad = 1;
   }
}

static int
exec_block(struct ex *e, nir_block *b)
{
   nir_foreach_instr(instr, b) {
      if (instr->type == nir_instr_type_jump) {
         switch (nir_instr_as_jump(instr)->type) {
         case nir_jump_break: return J_BREAK;
         case nir_jump_continue: return J_CONT;
         default: return J_RET;
         }
      }
      exec_instr(e, instr);
      if (e->bad)
         return J_RET;
   }
   return J_NONE;
}

static int
exec_list(struct ex *e, struct exec_list *l)
{
   foreach_list_typed(nir_cf_node, n, node, l) {
      int r = J_NONE;
      switch (n->type) {
      case nir_cf_node_block: r = exec_block(e, nir_cf_node_as_block(n)); break;
      case nir_cf_node_if: {
         nir_if *f = nir_cf_node_as_if(n);
         r = exec_list(e, e->v[f->condition.ssa->index][0].b ? &f->then_list : &f->else_list);
         break;
      }
      case nir_cf_node_loop: {
         nir_loop *lp = nir_cf_node_as_loop(n);
         for (unsigned guard = 0; guard < 100000; guard++) {
            r = exec_list(e, &lp->body);
            if (r == J_BREAK) { r = J_NONE; break; }
            if (r == J_RET) break;
            r = J_NONE;
         }
         break;
      }
      default: e->bad = 1; return J_RET;
      }
      if (r != J_NONE)
         return r;
   }
   return J_NONE;
}

/* Runs a (cloned) shader once. */
static void
run_shader(nir_shader *src, struct ex *e)
{
   nir_shader *s = nir_shader_clone(NULL, src);
   nir_convert_from_ssa(s, true, false);
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   nir_index_ssa_defs(impl);
   e->v = calloc(impl->ssa_alloc, sizeof *e->v);
   exec_list(e, &impl->body);
   free(e->v);
   ralloc_free(s);
}

/* ---- the CPU "pipeline": kernel dispatch, index walk, passthrough VS, rasteriser ---- */
#define FB 128
struct vtx { float pos[4], var[4]; };

struct gsrun {
   struct mali_gs_info gs;
   uint64_t vs_outputs;
   unsigned nprims, vs_stride, vcount;
   uint32_t *indices;
   uint8_t *records;
   unsigned cap_slots;
};

static int
slot_of(uint64_t outputs, unsigned loc)
{
   return __builtin_popcountll(outputs & ((1ull << loc) - 1));
}

/* Dispatch the kernel over nprims primitives in the given order (ascending or descending thread order), then
 * leave records/indices in r. vs_rec: the real VS's records. */
static int
dispatch(struct gsrun *r, nir_shader *kernel, uint8_t *vs_rec, unsigned nverts, int descending)
{
   const struct mali_gs_info *gs = &r->gs;
   r->nprims = nverts / (gs->input_prim == MESA_PRIM_POINTS ? 1 : gs->input_prim == MESA_PRIM_LINES ? 2 : 3);
   r->cap_slots = r->nprims;
   r->records = calloc(r->cap_slots * gs->vertices_out + 8, gs->record_stride);   /* slack: the mutation overflows */
   r->indices = calloc(r->cap_slots, gs->index_stride * 4);
   struct mali_gs_params p = {
      .vs_records = (uintptr_t)vs_rec, .records = (uintptr_t)r->records, .indices = (uintptr_t)r->indices,
      .vertex_count = nverts, .instance_count = 1, .cap_slots = r->cap_slots,
   };
   uint8_t push[256] = {0};
   memcpy(push + 64, &p, sizeof p);
   for (unsigned k = 0; k < r->nprims + 2; k++) {   /* two extra threads: bounds check must hold */
      struct ex e = { .push = push };
      e.gid[0] = descending ? r->nprims + 1 - k : k;
      run_shader(kernel, &e);
      if (e.bad)
         return -1;
   }
   return 0;
}

/* Index walk + passthrough VS: returns assembled triangle-strip triangles as vertices; counts emitted vertices. */
static unsigned
draw_strip(struct gsrun *r, nir_shader *pt, struct vtx *tris, unsigned max_tris, unsigned *nverts_out)
{
   uint8_t push[256] = {0};
   struct mali_gs_params p = { .records = (uintptr_t)r->records };
   memcpy(push + 64, &p, sizeof p);
   unsigned total = r->cap_slots * r->gs.index_stride, nt = 0, nv = 0;
   struct vtx strip[8];
   unsigned run = 0;
   for (unsigned i = 0; i < total; i++) {
      uint32_t idx = r->indices[i];
      if (idx == 0xffffffffu) { run = 0; continue; }
      struct ex e = { .push = push, .vertex_id = idx };
      run_shader(pt, &e);
      struct vtx v;
      memcpy(v.pos, e.out[VARYING_SLOT_POS], 16);
      memcpy(v.var, e.out[VARYING_SLOT_VAR0], 16);
      nv++;
      strip[run % 3] = v;
      run++;
      if (run >= 3 && nt < max_tris) {   /* strip: alternate winding */
         unsigned a = (run - 3) % 3, b = (run - 2) % 3, c = (run - 1) % 3;
         if (run & 1) { tris[nt * 3] = strip[a]; tris[nt * 3 + 1] = strip[b]; }
         else { tris[nt * 3] = strip[b]; tris[nt * 3 + 1] = strip[a]; }
         tris[nt * 3 + 2] = strip[c];
         nt++;
      }
   }
   *nverts_out = nv;
   return nt;
}

/* Barycentric raster of one pixel centre: returns 1 and the interpolated var if (x, y) is covered. */
static int
raster_px(const struct vtx *t, float px, float py, float out[4])
{
   float x[3], y[3];
   for (int k = 0; k < 3; k++) {
      x[k] = (t[k].pos[0] / t[k].pos[3] * 0.5f + 0.5f) * FB;
      y[k] = (t[k].pos[1] / t[k].pos[3] * 0.5f + 0.5f) * FB;
   }
   float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
   if (area == 0)
      return 0;
   float w0 = ((x[1] - px) * (y[2] - py) - (x[2] - px) * (y[1] - py)) / area;
   float w1 = ((x[2] - px) * (y[0] - py) - (x[0] - px) * (y[2] - py)) / area;
   float w2 = 1 - w0 - w1;
   if (w0 < 0 || w1 < 0 || w2 < 0)
      return 0;
   for (int c = 0; c < 4; c++)
      out[c] = w0 * t[0].var[c] + w1 * t[1].var[c] + w2 * t[2].var[c];
   return 1;
}

static int
near(float a, float b) { return fabsf(a - b) < 1e-4f; }

/* Compile a GS to the kernel + passthrough and run it. Returns 0 on success. */
static int
build_run(const char *file, unsigned arch, unsigned flags, uint8_t *vs_rec, unsigned nverts, uint64_t vs_outputs,
          int descending, struct gsrun *r, struct vtx *tris, unsigned max_tris, unsigned *nt, unsigned *nv)
{
   struct spv s = load(file);
   if (!s.w)
      return -1;
   struct mali_compile_opts o = { .arch = arch, .stage = MALI_STAGE_COMPUTE, .spirv = s.w, .spirv_words = s.n };
   char err[256] = "";
   memset(r, 0, sizeof *r);
   if (mali_gs_analyze(&o, &r->gs, err, sizeof err)) { printf("  analyze: %s\n", err); free(s.w); return -1; }
   r->vs_outputs = vs_outputs;
   nir_shader *k = mali_gs_kernel_nir(&o, &r->gs, vs_outputs, 64, flags, err, sizeof err);
   if (!k) { printf("  kernel: %s\n", err); free(s.w); return -1; }
   const struct nir_shader_compiler_options *vo = pan_get_nir_shader_compiler_options(arch, MESA_SHADER_VERTEX, false);
   nir_shader *pt = mali_gs_passthrough_nir(vo, &r->gs, 64);
   int rc = dispatch(r, k, vs_rec, nverts, descending);
   if (!rc)
      *nt = draw_strip(r, pt, tris, max_tris, nv);
   ralloc_free(k);
   ralloc_free(pt);
   free(s.w);
   return rc;
}

static unsigned
live_indices(const struct gsrun *r, unsigned slot)
{
   unsigned n = 0;
   for (unsigned i = 0; i < r->gs.index_stride; i++)
      n += r->indices[slot * r->gs.index_stride + i] != 0xffffffffu;
   return n;
}

static void
put4(uint8_t *rec, unsigned vertex, unsigned stride, unsigned slot, float a, float b, float c, float d)
{
   float f[4] = { a, b, c, d };
   memcpy(rec + vertex * stride + slot * 16, f, 16);
}

static void
test_e2e_basic(unsigned arch)
{
   /* VS records: pos + in_col (slot 0 = POS, slot 1 = VAR0: popcount-ranked) */
   uint64_t vso = VARYING_BIT_POS | VARYING_BIT_VAR(0);
   uint8_t vs[3 * 32] = {0};
   const float P[3][2] = { {-0.8f, -0.8f}, {0.8f, -0.8f}, {0.0f, 0.8f} };
   const float C[3][3] = { {1, 0, 0}, {0, 1, 0}, {0, 0, 1} };
   for (int i = 0; i < 3; i++) {
      put4(vs, i, 32, slot_of(vso, VARYING_SLOT_POS), P[i][0], P[i][1], 0, 1);
      put4(vs, i, 32, slot_of(vso, VARYING_SLOT_VAR0), C[i][0], C[i][1], C[i][2], 0);
   }
   struct gsrun r; struct vtx tris[24]; unsigned nt = 0, nv = 0;
   if (build_run("gs/basic.geom.spv", arch, 0, vs, 3, vso, 0, &r, tris, 8, &nt, &nv)) { g_fail++; return; }
   CHECK(nv == 3, "e2e basic: %u vertices through the passthrough != 3", nv);
   CHECK(nt == 1, "e2e basic: %u triangles != 1", nt);
   CHECK(live_indices(&r, 0) == 3, "e2e basic: %u live indices != 3", live_indices(&r, 0));
   CHECK(r.indices[0] == 0 && r.indices[1] == 1 && r.indices[2] == 2 && r.indices[3] == 0xffffffffu,
         "e2e basic: index slot %u %u %u %u", r.indices[0], r.indices[1], r.indices[2], r.indices[3]);
   for (int i = 0; i < 3 && nt; i++)
      CHECK(near(tris[i].pos[0], P[i][0]) && near(tris[i].pos[1], P[i][1]) && near(tris[i].pos[3], 1),
            "e2e basic: vertex %d position %g %g", i, tris[i].pos[0], tris[i].pos[1]);
   /* varying interpolation through the passthrough: compare against direct barycentrics of the inputs */
   int covered = 0, bad = 0;
   for (int y = 0; y < FB; y += 3)
      for (int x = 0; x < FB; x += 3) {
         float got[4], px = x + 0.5f, py = y + 0.5f;
         if (!nt || !raster_px(tris, px, py, got))
            continue;
         float X[3], Y[3];
         for (int k = 0; k < 3; k++) { X[k] = (P[k][0] * .5f + .5f) * FB; Y[k] = (P[k][1] * .5f + .5f) * FB; }
         float area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
         float w0 = ((X[1] - px) * (Y[2] - py) - (X[2] - px) * (Y[1] - py)) / area;
         float w1 = ((X[2] - px) * (Y[0] - py) - (X[0] - px) * (Y[2] - py)) / area;
         float w2 = 1 - w0 - w1;
         covered++;
         if (!near(got[0], w0) || !near(got[1], w1) || !near(got[2], w2))
            bad++;
      }
   CHECK(covered > 500, "e2e basic: only %d covered samples", covered);
   CHECK(!bad, "e2e basic: %d/%d samples with wrong interpolated colour", bad, covered);
   printf("  e2e basic v%u: 3 vertices, %d samples interpolate through the passthrough\n", arch, covered);
   free(r.records); free(r.indices);
}

static void
test_e2e_points(unsigned arch)
{
   uint64_t vso = VARYING_BIT_POS;
   uint8_t vs[16] = {0};
   put4(vs, 0, 16, 0, 0.25f, -0.25f, 0.5f, 1);
   struct gsrun r; struct vtx tris[24]; unsigned nt = 0, nv = 0;
   if (build_run("gs/points.geom.spv", arch, 0, vs, 1, vso, 0, &r, tris, 8, &nt, &nv)) { g_fail++; return; }
   CHECK(nv == 4, "e2e points: %u vertices != 4", nv);
   CHECK(nt == 2, "e2e points: %u triangles != 2", nt);
   const float s = 0.02f, ox[4] = {-s, s, -s, s}, oy[4] = {-s, -s, s, s};
   float uv[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
   for (unsigned k = 0; k < 4; k++) {
      float got[4];
      memcpy(got, &r.records[k * r.gs.record_stride], 16);
      CHECK(near(got[0], 0.25f + ox[k]) && near(got[1], -0.25f + oy[k]) && near(got[2], 0.5f) && near(got[3], 1),
            "e2e points: record %u position %g %g %g %g", k, got[0], got[1], got[2], got[3]);
      float *u = (float *)&r.records[k * r.gs.record_stride + 16];
      CHECK(near(u[0], uv[k][0]) && near(u[1], uv[k][1]), "e2e points: record %u uv %g %g", k, u[0], u[1]);
   }
   int covered = 0, bad = 0;
   for (int y = 0; y < FB; y++)
      for (int x = 0; x < FB; x++) {
         float px = x + 0.5f, py = y + 0.5f, nx = (px / FB - 0.5f) * 2, ny = (py / FB - 0.5f) * 2, got[4];
         for (unsigned t = 0; t < nt; t++)
            if (raster_px(&tris[t * 3], px, py, got)) {
               covered++;
               if (!near(got[0], (nx - (0.25f - s)) / (2 * s)) || !near(got[1], (ny - (-0.25f - s)) / (2 * s)))
                  bad++;
               break;
            }
      }
   CHECK(covered >= 4, "e2e points: quad covers %d pixels", covered);
   CHECK(!bad, "e2e points: %d/%d pixels with wrong uv", bad, covered);
   printf("  e2e points v%u: 4 vertices, 2 triangles, %d pixels uv-correct\n", arch, covered);
   free(r.records); free(r.indices);
}

/* trunc.geom emits 4 vertices with max_vertices = 3. The 4th must vanish: 3 live indices per slot, and slot N+1's
 * first record must not be overwritten by slot N's overflow, in either thread order. With the truncation
 * removed (mutation) the same checks must fail. Returns the number of detected violations. */
static int
trunc_violations(unsigned arch, unsigned flags, int descending)
{
   uint64_t vso = VARYING_BIT_POS | VARYING_BIT_VAR(0);
   uint8_t vs[6 * 32] = {0};
   for (int i = 0; i < 6; i++) {
      put4(vs, i, 32, 0, 0.1f * (i + 1), 0.2f, 0.3f, 1);
      put4(vs, i, 32, 1, i, i + 1, i + 2, 0);
   }
   struct gsrun r; struct vtx tris[24]; unsigned nt = 0, nv = 0;
   if (build_run("gs/trunc.geom.spv", arch, flags, vs, 6, vso, descending, &r, tris, 8, &nt, &nv))
      return 1000;
   int bad = 0;
   for (unsigned slot = 0; slot < 2; slot++) {
      if (live_indices(&r, slot) != 3) bad++;
      for (unsigned v = 0; v < 3; v++) {
         const float *p = (const float *)&r.records[(slot * 3 + v) * r.gs.record_stride];
         if (!near(p[0], 0.1f * (slot * 3 + v + 1)) || !near(p[1], 0.2f)) bad++;
      }
   }
   if (nv != 6) bad++;
   if (nt != 2) bad++;
   free(r.records); free(r.indices);
   return bad;
}

static void
test_truncation(unsigned arch)
{
   for (int d = 0; d < 2; d++)
      CHECK(trunc_violations(arch, 0, d) == 0, "truncation: violations with %s thread order",
            d ? "descending" : "ascending");
   /* Mutation check: dropping the vertex-count bound must be caught by the same assertions. */
   for (int d = 0; d < 2; d++) {
      int v = trunc_violations(arch, MALI_GS_LOWER_TEST_NO_TRUNC, d);
      CHECK(v > 0, "truncation mutation (%s order) NOT detected", d ? "descending" : "ascending");
   }
   printf("  truncation v%u: 4th vertex dropped, neighbour slot intact (mutation detected)\n", arch);
}

int
main(int argc, char **argv)
{
   if (argc > 1)
      g_dir = argv[1];
   printf("t_gs: %s\n", g_dir);
   for (unsigned arch = 11; arch <= 13; arch += 2) {
      test_basic(arch);
      test_points(arch);
      test_passthrough(arch);
      test_e2e_basic(arch);
      test_e2e_points(arch);
      test_truncation(arch);
   }
   test_layout();
   if (g_fail)
      printf("FAILED (%d)\n", g_fail);
   else
      printf("OK\n");
   return g_fail ? 1 : 0;
}
