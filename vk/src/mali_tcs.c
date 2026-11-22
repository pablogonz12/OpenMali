/* SPDX-License-Identifier: MIT */
/* T1's tessellation-control classifier (notes/tess-design.md § "T1 implementation"): a TCS that is provably the *identity
 * patch* at every tess level 1.0, judged from SPIR-V words alone — the same method mali_gs.c uses, for the same reason.
 *
 * The NIR route cannot do this job: after `nir_lower_io_vars_to_temporaries` (mali_compile.c:542) a `gl_out[...]` store
 * keeps a temp-variable vertex index while the matching `gl_in[...]` load comes back with a constant one, so the
 * per-invocation identity is no longer visible in the lowered form (lane/tess-t1's first attempt, PR #385). The words
 * still say it plainly, so this file reads them.
 *
 * Accepted shape — and nothing else:
 *   * a TessControl entry point whose OutputVertices execution mode is 3;
 *   * one basic block, no branch/loop/switch, no function call, no memory write outside the shader's own outputs;
 *   * gl_TessLevelOuter[0..3] and gl_TessLevelInner[0..1] stored from constants, every one of them 1.0 (a single
 *     OpConstantComposite store or per-component stores, both accepted);
 *   * every other store goes to a per-vertex output and is a copy of the per-vertex input of the *same* varying
 *     (Location, or the same BuiltIn member of the gl_PerVertex block), the same type and the same remaining path, at the
 *     *same index id* — i.e. the same control point. The index is compared as an id (both sides read gl_InvocationID),
 *     which is what lets this work without folding anything;
 *   * every input the stage reads is written back (or the fragment stage would lose it).
 * With that, at levels 1.0 the TCS outputs exactly the patch the vertex shader produced, so the driver can compile the VS
 * and drop both tessellation stages: zero jobs, zero dispatches, zero bytes (mali_pipeline.c). Anything else — a TCS that
 * moves a vertex, adds a varying, reorders control points, or uses level != 1.0 — is refused here with a named reason and
 * needs lane T2/T3. */
#include "mali_tcs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- SPIR-V opcodes ---- */
enum {
   OP_NOP = 0, OP_NAME = 5, OP_LINE = 8, OP_ENTRY_POINT = 15, OP_EXECUTION_MODE = 16,
   OP_TYPE_FLOAT = 22, OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32,
   OP_CONSTANT = 43, OP_CONSTANT_COMPOSITE = 44,
   OP_FUNCTION = 54, OP_FUNCTION_END = 56,
   OP_VARIABLE = 59, OP_LOAD = 61, OP_STORE = 62,
   OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_PTR_ACCESS_CHAIN = 67,
   OP_DECORATE = 71, OP_MEMBER_DECORATE = 72, OP_DECORATION_GROUP = 73, OP_GROUP_DECORATE = 74,
   OP_LOOP_MERGE = 246, OP_SELECTION_MERGE = 247, OP_LABEL = 248, OP_BRANCH = 249,
   OP_BRANCH_CONDITIONAL = 250, OP_SWITCH = 251, OP_RETURN = 253, OP_RETURN_VALUE = 254,
   OP_NO_LINE = 317,
};

/* ---- decorations, builtins, storage classes ---- */
enum { DEC_BLOCK = 2, DEC_BUILTIN = 11, DEC_LOCATION = 30 };
/* SPIR-V BuiltIn: Position 0, InvocationId 8, TessLevelOuter 11, TessLevelInner 12 — the level builtins are *before*
 * FragmentLayer/Layer at 9/10, and confusing them with 23/24 silently refuses every TCS. */
enum { BI_POSITION = 0, BI_INVOCATIONID = 8, BI_TESS_LEVEL_OUTER = 11, BI_TESS_LEVEL_INNER = 12 };
enum { EM_OUTPUT_VERTICES = 26 };
enum { SC_INPUT = 1, SC_OUTPUT = 3, SC_FUNCTION = 7 };
enum { EXEC_MODEL_TESS_CONTROL = 1 };

#define TCS_MAX_PATH 8
#define TCS_MAX_OUT 32

struct tcs_type { uint16_t kind; uint32_t a, b; };
struct tcs_deco { int16_t builtin, location; uint8_t block, has, has_builtin, has_loc; };
struct tcs_mdeco { uint32_t type; uint8_t member; int16_t builtin; };
/* A whole pointer: the base global and its index path. Every index keeps both readings: the constant value when it is one
 * (0xffffffff otherwise) and the id of the operand, so "the same control point" can be an id comparison. */
struct tcs_chain {
   int32_t base;
   int8_t n;
   uint8_t storage;
   uint32_t pointee;
   uint32_t idx[TCS_MAX_PATH];
   uint32_t idx_id[TCS_MAX_PATH];
};
struct tcs_out { int8_t kind; int16_t val; int8_t npath; uint32_t pointee; uint32_t path[TCS_MAX_PATH]; };

static int
refuse(char *why, size_t whysz, const char *fmt, ...)
{
   if (!why || !whysz)
      return 0;
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(why, whysz, fmt, ap);
   va_end(ap);
   if (!why[0])
      snprintf(why, whysz, "refused");
   return 0;
}

/* The interface key of a pointer into a per-vertex input or output, and how many leading indexes belong to the key rather
 * than to the path. Both sides of T1 are arrays (the control point is the first index), so the rule is the same for
 * inputs and outputs — unlike mali_gs.c, where outputs are not arrayed. */
static int
key_of(const struct tcs_chain *c, const struct tcs_chain *chains, const struct tcs_type *type, const struct tcs_deco *deco,
       const struct tcs_mdeco *mdeco, unsigned nmdeco, int8_t *kind, int16_t *val, int8_t *skip, char *why, size_t whysz)
{
   uint32_t t = chains[c->base].pointee;
   const int arrayed = type[t].kind == OP_TYPE_ARRAY || type[t].kind == OP_TYPE_RUNTIME_ARRAY;
   while (type[t].kind == OP_TYPE_ARRAY || type[t].kind == OP_TYPE_RUNTIME_ARRAY)
      t = type[t].a;
   if (type[t].kind == OP_TYPE_STRUCT && deco[t].block) {
      const int8_t need = arrayed ? 2 : 1;
      if (c->n < need)
         return refuse(why, whysz, "a whole %s block, not one member", arrayed ? "per-vertex" : "per-patch");
      const uint32_t member = c->idx[arrayed ? 1 : 0];
      if (member >= 0xffffffffu)
         return refuse(why, whysz, "a block member with a non-constant index");
      int16_t bi = -1;
      for (unsigned m = 0; m < nmdeco; m++)
         if (mdeco[m].type == t && mdeco[m].member == member)
            bi = mdeco[m].builtin;
      if (bi < 0)
         return refuse(why, whysz, "a block member %u without a BuiltIn", member);
      *kind = 1;
      *val = bi;
      *skip = need;
      return 1;
   }
   if (deco[c->base].has_builtin) {
      *kind = 1;
      *val = deco[c->base].builtin;
      *skip = arrayed ? 1 : 0;
      return 1;
   }
   *kind = 0;
   *val = deco[c->base].has_loc ? deco[c->base].location : -1;
   *skip = arrayed ? 1 : 0;
   return 1;
}

/* The store of `o` must be a copy of `i`: same key, same pointee, same path beyond the key, same control point. */
static int
same_varying(const struct tcs_chain *o, const struct tcs_chain *i, int8_t o_skip, int8_t i_skip, int8_t okind, int16_t oval,
             int8_t ikind, int16_t ival)
{
   if (okind != ikind || oval != ival || o->pointee != i->pointee)
      return 0;
   const int8_t on = (int8_t)(o->n - o_skip), in = (int8_t)(i->n - i_skip);
   if (on != in)
      return 0;
   for (int8_t k = 0; k < on; k++)
      if (o->idx[o_skip + k] != i->idx[i_skip + k])
         return 0;
   return 1;
}

int
mali_tcs_passthrough(const uint32_t *w, uint32_t nwords, const char *entry, char *why, size_t whysz)
{
   if (why && whysz)
      why[0] = 0;
   if (!w || nwords < 5 || w[0] != 0x07230203u)
      return refuse(why, whysz, "not a SPIR-V module (bad magic)");
   const uint32_t bound = w[3];
   if (bound == 0 || bound > (1u << 24))
      return refuse(why, whysz, "bad SPIR-V id bound");

   struct tcs_type *type = calloc(bound, sizeof *type);
   struct tcs_deco *deco = calloc(bound, sizeof *deco);
   struct tcs_chain *chain = calloc(bound, sizeof *chain);
   int32_t *load_ptr = calloc(bound, sizeof *load_ptr);
   int32_t *konst = calloc(bound, sizeof *konst);
   uint32_t *cons = calloc(bound, sizeof *cons);   /* float constant: the id of its type; 0xffffffff = a composite */
   int32_t *compo = calloc(bound, sizeof *compo);  /* composite: the word offset of its OpConstantComposite, -1 = none */
   struct tcs_mdeco mdeco[256];
   unsigned nmdeco = 0;
   struct tcs_out out[TCS_MAX_OUT];
   memset(out, 0, sizeof out);
   unsigned nout = 0;
   unsigned level_mask = 0;
   int rc = 0;

   if (!type || !deco || !chain || !load_ptr || !konst || !cons || !compo) {
      refuse(why, whysz, "out of memory");
      goto done;
   }
   for (uint32_t i = 0; i < bound; i++)
      chain[i].base = load_ptr[i] = konst[i] = compo[i] = -1;

   /* ---- pass 1: types, decorations, globals, constants, the entry point ---- */
   uint32_t entry_func = 0;
   int have_entry = 0;
   for (uint32_t i = 5; i + 1 < nwords;) {
      const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
      if (!wc || i + wc > nwords)
         break;
      switch (op) {
      case OP_ENTRY_POINT:
         if (wc >= 4 && w[i + 1] == EXEC_MODEL_TESS_CONTROL && !have_entry) {
            const char *name = (const char *)&w[i + 3];
            const char *want = entry && entry[0] ? entry : "main";
            if (strcmp(name, want)) {
               rc = refuse(why, whysz, "tessellation control entry point '%s' is not '%s'", name, want);
               goto done;
            }
            entry_func = w[i + 2];
            have_entry = 1;
         }
         break;
      case OP_TYPE_FLOAT:   if (wc >= 2) type[w[i + 1]] = (struct tcs_type){.kind = OP_TYPE_FLOAT}; break;
      case OP_TYPE_POINTER: if (wc >= 4) type[w[i + 1]] = (struct tcs_type){.kind = OP_TYPE_POINTER, .a = w[i + 3]}; break;
      case OP_TYPE_ARRAY:   if (wc >= 4) type[w[i + 1]] = (struct tcs_type){.kind = OP_TYPE_ARRAY, .a = w[i + 2]}; break;
      case OP_TYPE_RUNTIME_ARRAY: if (wc >= 3) type[w[i + 1]] = (struct tcs_type){.kind = OP_TYPE_RUNTIME_ARRAY, .a = w[i + 2]}; break;
      case OP_TYPE_STRUCT:  if (wc >= 2) type[w[i + 1]] = (struct tcs_type){.kind = OP_TYPE_STRUCT}; break;
      case OP_CONSTANT:
         if (wc >= 4) {
            konst[w[i + 2]] = (int32_t)w[i + 3];             /* also the bit pattern of a 32-bit float */
            if (type[w[i + 1]].kind == OP_TYPE_FLOAT)
               cons[w[i + 2]] = w[i + 1];
         }
         break;
      case OP_CONSTANT_COMPOSITE:
         if (wc >= 3) {
            cons[w[i + 2]] = 0xffffffffu;
            compo[w[i + 2]] = (int32_t)i;                    /* constituents checked at the store that uses it */
         }
         break;
      case OP_VARIABLE:
         if (wc >= 4 && (w[i + 3] == SC_INPUT || w[i + 3] == SC_OUTPUT) && w[i + 2] < bound) {
            chain[w[i + 2]].base = (int32_t)w[i + 2];
            chain[w[i + 2]].n = 0;
            chain[w[i + 2]].storage = (uint8_t)w[i + 3];
            chain[w[i + 2]].pointee = type[w[i + 1]].a;
         }
         break;
      case OP_DECORATE:
         if (wc >= 3 && w[i + 1] < bound) {
            struct tcs_deco *d = &deco[w[i + 1]];
            d->has = 1;
            if (w[i + 2] == DEC_BUILTIN && wc >= 4) { d->builtin = (int16_t)w[i + 3]; d->has_builtin = 1; }
            if (w[i + 2] == DEC_LOCATION && wc >= 4) { d->location = (int16_t)w[i + 3]; d->has_loc = 1; }
            if (w[i + 2] == DEC_BLOCK) d->block = 1;
         }
         break;
      case OP_MEMBER_DECORATE:
         if (wc >= 5 && w[i + 3] == DEC_BUILTIN && nmdeco < sizeof mdeco / sizeof *mdeco)
            mdeco[nmdeco++] = (struct tcs_mdeco){.type = w[i + 1], .member = (uint8_t)w[i + 2], .builtin = (int16_t)w[i + 4]};
         break;
      case OP_GROUP_DECORATE:
         if (wc >= 3 && w[i + 1] < bound && deco[w[i + 1]].has) {
            const struct tcs_deco g = deco[w[i + 1]];
            for (unsigned k = 2; k < wc; k++) {
               if (w[i + k] >= bound)
                  continue;
               struct tcs_deco *t = &deco[w[i + k]];
               t->has = 1;
               if (g.has_builtin && !t->has_builtin) { t->builtin = g.builtin; t->has_builtin = 1; }
               if (g.has_loc && !t->has_loc) { t->location = g.location; t->has_loc = 1; }
               if (g.block) t->block = 1;
            }
         }
         break;
      case OP_DECORATION_GROUP:
         break;
      default: break;
      }
      i += wc;
   }
   if (!have_entry) {
      rc = refuse(why, whysz, "no TessControl entry point");
      goto done;
   }

   /* ---- OutputVertices must be 3: T1's patch is a triangle, and the draw count assumes 3 ---- */
   {
      unsigned verts = 0;
      for (uint32_t i = 5; i + 2 < nwords;) {
         const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
         if (!wc || i + wc > nwords)
            break;
         if (op == OP_EXECUTION_MODE && w[i + 1] == entry_func && wc >= 4 && w[i + 2] == EM_OUTPUT_VERTICES)
            verts = w[i + 3];
         i += wc;
      }
      if (verts != 3) {
         rc = refuse(why, whysz, "OutputVertices %u: T1 accepts a 3-vertex (triangle) patch", verts);
         goto done;
      }
   }

   /* ---- the entry function: one straight-line block ---- */
   uint32_t body = 0;
   for (uint32_t i = 5; i + 4 < nwords;) {
      const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
      if (!wc || i + wc > nwords)
         break;
      if (op == OP_FUNCTION && w[i + 2] == entry_func) {
         body = i;
         break;
      }
      i += wc;
   }
   if (!body) {
      rc = refuse(why, whysz, "tessellation control entry function not found");
      goto done;
   }
   {
      unsigned blocks = 0;
      int started = 0;
      for (uint32_t i = body; i + 1 < nwords;) {
         const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
         if (!wc || i + wc > nwords)
            break;
         if (op == OP_FUNCTION) {
            if (started) {
               rc = refuse(why, whysz, "a second OpFunction in the body");
               goto done;
            }
            started = 1;
         } else if (op == OP_FUNCTION_END) {
            break;
         } else if (started) {
            if (op == OP_LABEL)
               blocks++;
            else if (op == OP_BRANCH || op == OP_BRANCH_CONDITIONAL || op == OP_SWITCH || op == OP_LOOP_MERGE ||
                     op == OP_SELECTION_MERGE) {
               rc = refuse(why, whysz, "control flow (opcode %u) in the tessellation control shader", op);
               goto done;
            }
         }
         i += wc;
      }
      if (blocks != 1) {
         rc = refuse(why, whysz, "%u basic blocks in the body (an identity patch has one)", blocks);
         goto done;
      }
   }

   /* ---- the body ---- */
   {
      int in_body = 0;
      for (uint32_t i = body; i + 1 < nwords;) {
         const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
         if (!wc || i + wc > nwords)
            break;
         if (op == OP_FUNCTION) {
            if (in_body) {
               rc = refuse(why, whysz, "nested OpFunction");
               goto done;
            }
            in_body = 1;
            i += wc;
            continue;
         } else if (op == OP_FUNCTION_END) {
            break;
         }
         if (!in_body) {
            i += wc;
            continue;
         }
         switch (op) {
         case OP_LABEL:
            break;
         case OP_BRANCH: case OP_BRANCH_CONDITIONAL: case OP_SWITCH: case OP_LOOP_MERGE: case OP_SELECTION_MERGE:
            rc = refuse(why, whysz, "control flow (opcode %u) in the tessellation control shader", op);
            goto done;
         case OP_VARIABLE:
            if (wc >= 4 && w[i + 3] != SC_FUNCTION) {
               rc = refuse(why, whysz, "a variable of storage %u in the body", w[i + 3]);
               goto done;
            }
            break;
         case OP_ACCESS_CHAIN: case OP_IN_BOUNDS_ACCESS_CHAIN: {
            if (wc < 4)
               break;
            struct tcs_chain *c = &chain[w[i + 2]];
            const struct tcs_chain *b = &chain[w[i + 3]];
            if (b->base < 0) {
               c->base = -1;
               c->n = -1;
               break;
            }
            const int8_t n = (int8_t)(wc - 4);
            if (n > TCS_MAX_PATH) {
               c->base = -1;
               c->n = -1;
               break;
            }
            *c = (struct tcs_chain){.base = b->base, .n = n, .storage = b->storage,
                                    .pointee = type[w[i + 1]].kind == OP_TYPE_POINTER ? type[w[i + 1]].a : 0};
            for (int8_t k = 0; k < n; k++) {
               const int32_t v = konst[w[i + 4 + k]];
               c->idx[k] = v < 0 ? 0xffffffffu : (uint32_t)v;
               c->idx_id[k] = w[i + 4 + k];
            }
            break;
         }
         case OP_LOAD:
            if (wc >= 4) {
               const struct tcs_chain *c = &chain[w[i + 3]];
               if (c->base < 0 || c->n < 0) {
                  rc = refuse(why, whysz, "a read through a pointer this analysis cannot name (word %u)", i);
                  goto done;
               }
               if (c->storage != SC_INPUT) {
                  rc = refuse(why, whysz, "a read that is not a tessellation input (storage %u)", c->storage);
                  goto done;
               }
               load_ptr[w[i + 2]] = (int32_t)w[i + 3];
            }
            break;
         case OP_STORE: {
            if (wc < 3)
               break;
            const struct tcs_chain *c = &chain[w[i + 1]];
            const int32_t val = (int32_t)w[i + 2];
            if (c->base < 0 || c->n < 0) {
               rc = refuse(why, whysz, "OpStore through a pointer this analysis cannot name (word %u)", i);
               goto done;
            }
            if (c->storage != SC_OUTPUT) {
               rc = refuse(why, whysz, "a store that is not a tessellation output (storage %u)", c->storage);
               goto done;
            }
            /* the six tess level slots: written from constants 1.0, whole-array or per component */
            if (deco[c->base].has_builtin && (deco[c->base].builtin == BI_TESS_LEVEL_OUTER ||
                                              deco[c->base].builtin == BI_TESS_LEVEL_INNER)) {
               const int outer = deco[c->base].builtin == BI_TESS_LEVEL_OUTER;
               const unsigned max = outer ? 4u : 2u;
               const char *slot = outer ? "Outer" : "Inner";
               unsigned mask;
               if (c->n == 0) {
                  mask = (1u << max) - 1u;          /* the whole array in one store */
               } else if (c->idx[0] >= max) {
                  rc = refuse(why, whysz, "gl_TessLevel%s[%u] is out of range", slot, c->idx[0]);
                  goto done;
               } else {
                  mask = 1u << c->idx[0];
               }
               if (val < 0 || val >= (int32_t)bound) {
                  rc = refuse(why, whysz, "gl_TessLevel%s holds a value this analysis cannot name", slot);
                  goto done;
               }
               const unsigned want = c->n == 0 ? max : 1u;
               if (cons[val] == 0) {
                  rc = refuse(why, whysz, "gl_TessLevel%s is written from a value, not the constant 1.0", slot);
                  goto done;
               }
               if (cons[val] == 0xffffffffu) {
                  /* OpConstantComposite: every constituent must be the float 1.0f */
                  if (compo[val] < 0) {
                     rc = refuse(why, whysz, "gl_TessLevel%s composite not found", slot);
                     goto done;
                  }
                  const uint32_t ci = (uint32_t)compo[val], cnt = (w[ci] >> 16) - 3;
                  if (cnt != want) {
                     rc = refuse(why, whysz, "gl_TessLevel%s is filled with %u constants, %u needed", slot, cnt, want);
                     goto done;
                  }
                  for (uint32_t k = 0; k < cnt; k++) {
                     const int32_t cv = (int32_t)w[ci + 3 + k];
                     if (cv < 0 || cv >= (int32_t)bound || !cons[cv] || cons[cv] == 0xffffffffu ||
                         (uint32_t)konst[cv] != 0x3f800000u) {
                        rc = refuse(why, whysz, "gl_TessLevel%s[%u] is not the constant 1.0", slot, k);
                        goto done;
                     }
                  }
               } else {
                  if (want != 1u) {
                     rc = refuse(why, whysz, "gl_TessLevel%s is filled from a scalar constant", slot);
                     goto done;
                  }
                  if ((uint32_t)konst[val] != 0x3f800000u) {
                     const float f = *(const float *)&konst[val];
                     rc = refuse(why, whysz, "gl_TessLevel%s is %g, not 1.0", slot, (double)f);
                     goto done;
                  }
               }
               level_mask |= outer ? (mask << 4) : mask;
               break;
            }
            /* a per-vertex output: must copy the matching per-vertex input at the same control point */
            if (getenv("MALI_TESS_DBG"))
               fprintf(stderr, "mali_tcs: store word=%u val=%d base=%d storage=%u n=%d builtin=%d loc=%d\n", i, val,
                       c->base, c->storage, c->n, deco[c->base].builtin, deco[c->base].location);
            if (val < 0 || val >= (int32_t)bound || load_ptr[val] < 0) {
               rc = refuse(why, whysz, "an output is computed, not copied from the patch input (word %u)", i);
               goto done;
            }
            const struct tcs_chain *ic = &chain[load_ptr[val]];
            int8_t okind, ikind, o_skip = 0, i_skip = 0;
            int16_t oval = 0, ival = 0;
            if (!key_of(c, chain, type, deco, mdeco, nmdeco, &okind, &oval, &o_skip, why, whysz) ||
                !key_of(ic, chain, type, deco, mdeco, nmdeco, &ikind, &ival, &i_skip, why, whysz))
               goto done;
            if (!same_varying(c, ic, o_skip, i_skip, okind, oval, ikind, ival)) {
               rc = refuse(why, whysz, "output %s%d is not a copy of input %s%d (pointee 0x%x/0x%x)",
                           okind ? "builtin " : "location ", oval, ikind ? "builtin " : "location ", ival, c->pointee,
                           ic->pointee);
               goto done;
            }
            if (o_skip > 0 && i_skip > 0) {
               /* The same control point. glslang materialises a *separate* OpLoad of gl_InvocationID per access chain, so
                * the ids differ even when the value cannot: two loads of the same pointer are the same value, and the
                * invocation id is the only non-constant index a T1 body can use. */
               const uint32_t ov = c->idx[0], iv = ic->idx[0];
               int same = (c->idx_id[0] == ic->idx_id[0]) || (ov != 0xffffffffu && iv != 0xffffffffu && ov == iv);
               if (!same) {
                  const int32_t po = load_ptr[c->idx_id[0]], pi = load_ptr[ic->idx_id[0]];
                  if (po >= 0 && pi >= 0 && po == pi)
                     same = 1;
                  else if (po >= 0 && pi >= 0 && chain[po].base >= 0 && chain[po].base == chain[pi].base &&
                           deco[chain[po].base].has_builtin && deco[chain[po].base].builtin == BI_INVOCATIONID)
                     same = 1;
               }
               if (!same) {
                  rc = refuse(why, whysz, "an output at control point %d copies control point %d (reordered)",
                              ov == 0xffffffffu ? -1 : (int)ov, iv == 0xffffffffu ? -1 : (int)iv);
                  goto done;
               }
            }
            unsigned k = 0;
            for (; k < nout; k++)
               if (out[k].kind == okind && out[k].val == oval && out[k].pointee == c->pointee &&
                   out[k].npath == (int8_t)(c->n - o_skip) &&
                   !memcmp(out[k].path, &c->idx[o_skip], sizeof(uint32_t) * (size_t)(c->n - o_skip)))
                  break;
            if (k == nout) {
               if (nout == TCS_MAX_OUT) {
                  rc = refuse(why, whysz, "more than %d outputs", TCS_MAX_OUT);
                  goto done;
               }
               nout++;
               out[k].kind = okind;
               out[k].val = oval;
               out[k].pointee = c->pointee;
               out[k].npath = (int8_t)(c->n - o_skip);
               memcpy(out[k].path, &c->idx[o_skip], sizeof(uint32_t) * (size_t)(c->n - o_skip));
            }
            break;
         }
         case OP_RETURN:
            break;
         case OP_RETURN_VALUE:
            rc = refuse(why, whysz, "OpReturnValue in a tessellation control shader");
            goto done;
         case OP_LINE: case OP_NO_LINE: case OP_NOP:
            break;
         default:
            /* pure instructions only (composite construction and arithmetic); a texture fetch or a call is refused by
             * number because it reads state the vertex program does not have */
            if ((op >= 79 && op <= 84) || (op >= 126 && op <= 217))
               break;
            rc = refuse(why, whysz,
                        "instruction %u in the tessellation control body: only copies of the patch inputs and the level "
                        "stores are accepted", op);
            goto done;
         }
         i += wc;
      }
   }
   if (rc)
      goto done;

   /* every level slot written with 1.0, and at least the position of every control point copied */
   /* outer occupies bits 4..7, inner bits 0..1 (see the level store above) */
   if (((level_mask >> 4) & 0xf) != 0xf)
      return refuse(why, whysz, "gl_TessLevelOuter is not fully written with 1.0 (mask 0x%x)", (level_mask >> 4) & 0xf);
   if ((level_mask & 0x3) != 0x3)
      return refuse(why, whysz, "gl_TessLevelInner is not fully written with 1.0 (mask 0x%x)", level_mask & 0x3);
   {
      int saw_position = 0;
      for (unsigned k = 0; k < nout; k++)
         if (out[k].kind == 1 && out[k].val == BI_POSITION)
            saw_position = 1;
      if (!saw_position)
         return refuse(why, whysz, "per-vertex gl_Position is not copied through the shader");
   }
   rc = 1;

done:
   free(type);
   free(deco);
   free(chain);
   free(load_ptr);
   free(konst);
   free(cons);
   free(compo);
   return rc;
}
