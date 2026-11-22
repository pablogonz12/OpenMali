/* SPDX-License-Identifier: MIT */
/* Classify a geometry shader as a provable pass-through (mali_gs.h, notes/gs-design.md §4).
 *
 * The accepted shape — and nothing else — is: one basic block; for every vertex i of the input primitive, in order,
 * each output is stored from an OpLoad of the *matching* input at index i (same Location, or the same BuiltIn
 * member of the input/output block, same type, same remaining index path), followed by OpEmitVertex; an optional
 * OpEndPrimitive closes the stream after the last vertex. Such a GS emits exactly the vertex stream the vertex
 * shader already produced, in the same order, so compiling the VS alone and drawing the same vertices yields the
 * same picture and costs no job, no dispatch and no bytes. Every other geometry shader is refused here with a named
 * reason — a real GS (any cross-vertex read, amplification, EndPrimitive between vertices, side effects, streams,
 * invocations > 1) needs lane 2's compute pre-pass, and refusing loudly is what keeps the unadvertised bit safe.
 *
 * The analysis reads SPIR-V words only: no NIR, no lowering, no backend, so nothing downstream can change its
 * answer, and the same module gives the same answer on every arch. */
#include "mali_gs.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- SPIR-V opcodes we must name (anything else in the body is refused by number) ---- */
enum {
   OP_NOP = 0, OP_NAME = 5, OP_LINE = 8, OP_ENTRY_POINT = 15, OP_EXECUTION_MODE = 16,
   OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32,
   OP_CONSTANT = 43,
   OP_FUNCTION = 54, OP_FUNCTION_END = 56,
   OP_VARIABLE = 59, OP_LOAD = 61, OP_STORE = 62,
   OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_PTR_ACCESS_CHAIN = 67,
   OP_DECORATE = 71, OP_MEMBER_DECORATE = 72, OP_DECORATION_GROUP = 73, OP_GROUP_DECORATE = 74,
   OP_EMIT_VERTEX = 218, OP_END_PRIMITIVE = 219, OP_EMIT_STREAM_VERTEX = 220, OP_END_STREAM_PRIMITIVE = 222,
   OP_LOOP_MERGE = 246, OP_SELECTION_MERGE = 247, OP_LABEL = 248, OP_BRANCH = 249,
   OP_BRANCH_CONDITIONAL = 250, OP_SWITCH = 251, OP_RETURN = 253, OP_RETURN_VALUE = 254,
   OP_NO_LINE = 317,
};

/* ---- decorations, builtins, execution modes, storage classes ---- */
enum { DEC_BLOCK = 2, DEC_BUILTIN = 11, DEC_LOCATION = 30 };
enum { BI_POSITION = 0, BI_PRIMITIVEID = 7, BI_INVOCATIONID = 8, BI_LAYER = 9, BI_VIEWPORTINDEX = 10 };
/* SPIR-V ExecutionMode values (InputPoints 19 .. InputTrianglesAdjacency 23, OutputVertices 26, Xfb 11), not the
 * GLSL ordering they are easy to confuse with: a wrong constant here silently refuses every GS. */
enum { EM_INVOCATIONS = 0, EM_XFB = 11, EM_INPUT_POINTS = 19, EM_INPUT_LINES = 20, EM_INPUT_LINES_ADJ = 21,
       EM_INPUT_TRIANGLES = 22, EM_INPUT_TRIANGLES_ADJ = 23, EM_OUTPUT_VERTICES = 26, EM_OUTPUT_POINTS = 27,
       EM_OUTPUT_LINE_STRIP = 28, EM_OUTPUT_TRIANGLE_STRIP = 29 };
enum { SC_INPUT = 1, SC_OUTPUT = 3, SC_FUNCTION = 7 };
enum { EXEC_MODEL_GEOMETRY = 3 };   /* ExecutionModel: Vertex 0, TessControl 1, TessEval 2, Geometry 3, Fragment 4 */

#define GS_MAX_PATH 8
#define GS_MAX_OUT 32
#define GS_IDX_UNKNOWN 0xffffffffu

struct gs_type { uint16_t kind; uint32_t a, b; };
struct gs_deco { int16_t builtin, location; uint8_t block, has, has_builtin, has_loc; };
struct gs_mdeco { uint32_t type; uint8_t member; int16_t builtin; };
/* one whole pointer: the base id (a global variable) and the index path. n < 0 = a path with a non-constant index. */
struct gs_chain { int32_t base; int8_t n; uint8_t storage; uint32_t pointee; uint32_t idx[GS_MAX_PATH]; };
struct gs_out { int8_t kind; int16_t val; int8_t npath; uint32_t pointee; uint32_t path[GS_MAX_PATH]; int32_t stored_k; };

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

/* The store of `ptr` (an output) must be a copy of the input vertex `k`: same key (Location, or BuiltIn member),
 * same pointee type, same inner path. o_skip/i_skip = the indexes that belong to the key, not to the path. */
static int
same_varying(const struct gs_chain *o, const struct gs_chain *i, int8_t o_skip, int8_t i_skip, int8_t okind, int16_t oval,
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

/* The interface key of a pointer into a shader input (is_input) or output: the BuiltIn of the block member the path
 * names, or the variable's Location. The block is found through the pointee type's Block decoration (glslang decorates
 * the struct, never the variable), and an input block's first index is its vertex, so its member is index 1. */
static int
key_of(const struct gs_chain *c, int is_input, const struct gs_chain *chains, const struct gs_type *type,
       const struct gs_deco *deco, const struct gs_mdeco *mdeco, unsigned nmdeco, int8_t *kind, int16_t *val,
       int8_t *skip, char *why, size_t whysz)
{
   /* The block-ness lives on the *variable's* type (an array of the block for an input, the block itself for an output);
    * the pointer this store/load goes through has the leaf type (vec4), so it cannot answer this. */
   uint32_t t = chains[c->base].pointee;
   while (type[t].kind == OP_TYPE_ARRAY || type[t].kind == OP_TYPE_RUNTIME_ARRAY)
      t = type[t].a;
   if (type[t].kind == OP_TYPE_STRUCT && deco[t].block) {
      if (c->n < (is_input ? 2 : 1))
         return refuse(why, whysz, "a whole %s block, not one member", is_input ? "input vertex" : "output");
      const uint32_t member = c->idx[is_input ? 1 : 0];
      int16_t bi = -1;
      for (unsigned m = 0; m < nmdeco; m++)
         if (mdeco[m].type == t && mdeco[m].member == member)
            bi = mdeco[m].builtin;
      if (bi < 0)
         return refuse(why, whysz, "%s block member %u without a BuiltIn", is_input ? "an input" : "an output", member);
      *kind = 1;
      *val = bi;
      *skip = is_input ? 2 : 1;
      return 1;
   }
   if (deco[c->base].has_builtin) {   /* a plain builtin variable (gl_PrimitiveID and friends) */
      *kind = 1;
      *val = deco[c->base].builtin;
      *skip = 1;
      return 1;
   }
   if (!deco[c->base].has)
      return refuse(why, whysz, "a%s %s without Location or BuiltIn", is_input ? "n input" : "n output",
                    is_input ? "variable" : "variable");
   *kind = 0;
   *val = deco[c->base].location;
   *skip = is_input ? 1 : 0;
   return 1;
}

int
mali_gs_passthrough(const uint32_t *w, uint32_t nwords, const char *entry, unsigned *prims, char *why, size_t whysz)
{
   if (prims)
      *prims = 0;
   if (why && whysz)
      why[0] = 0;
   if (!w || nwords < 5 || w[0] != 0x07230203u)
      return refuse(why, whysz, "not a SPIR-V module (bad magic)");
   const uint32_t bound = w[3];
   if (bound == 0 || bound > (1u << 24))
      return refuse(why, whysz, "bad SPIR-V id bound");

   struct gs_type *type = calloc(bound, sizeof *type);
   struct gs_deco *deco = calloc(bound, sizeof *deco);
   struct gs_chain *chain = calloc(bound, sizeof *chain);
   int32_t *load_ptr = calloc(bound, sizeof *load_ptr);
   int32_t *konst = calloc(bound, sizeof *konst);
   struct gs_mdeco mdeco[256];
   unsigned nmdeco = 0;
   struct gs_out out[GS_MAX_OUT];
   memset(out, 0, sizeof out);
   unsigned nout = 0;
   unsigned in_prim = 0;
   int rc = 0;

   if (!type || !deco || !chain || !load_ptr || !konst) {
      refuse(why, whysz, "out of memory");
      goto done;
   }
   for (uint32_t i = 0; i < bound; i++)
      chain[i].base = load_ptr[i] = konst[i] = -1;

   /* ---- pass 1: types, decorations, globals, constants, the entry point ---- */
   uint32_t entry_func = 0;
   int have_entry = 0;
   for (uint32_t i = 5; i + 1 < nwords;) {
      const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
      if (!wc || i + wc > nwords)
         break;
      switch (op) {
      case OP_ENTRY_POINT:
         if (wc >= 4 && w[i + 1] == EXEC_MODEL_GEOMETRY && !have_entry) {
            const char *name = (const char *)&w[i + 3];
            const char *want = entry && entry[0] ? entry : "main";
            if (strcmp(name, want)) {
               rc = refuse(why, whysz, "geometry entry point '%s' is not '%s'", name, want);
               goto done;
            }
            entry_func = w[i + 2];
            have_entry = 1;
         }
         break;
      case OP_TYPE_POINTER: if (wc >= 4) type[w[i + 1]] = (struct gs_type){.kind = OP_TYPE_POINTER, .a = w[i + 3]}; break;
      case OP_TYPE_ARRAY:   if (wc >= 4) type[w[i + 1]] = (struct gs_type){.kind = OP_TYPE_ARRAY, .a = w[i + 2]}; break;
      case OP_TYPE_RUNTIME_ARRAY: if (wc >= 3) type[w[i + 1]] = (struct gs_type){.kind = OP_TYPE_RUNTIME_ARRAY, .a = w[i + 2]}; break;
      case OP_TYPE_STRUCT:  if (wc >= 2) type[w[i + 1]] = (struct gs_type){.kind = OP_TYPE_STRUCT}; break;
      case OP_CONSTANT:     if (wc >= 4) konst[w[i + 2]] = (int32_t)w[i + 3]; break;
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
            struct gs_deco *d = &deco[w[i + 1]];
            d->has = 1;
            /* presence is tracked separately: BuiltIn 0 (gl_Position) and Location 0 are real values, not "absent" */
            if (w[i + 2] == DEC_BUILTIN && wc >= 4) d->builtin = (int16_t)w[i + 3], d->has_builtin = 1;
            if (w[i + 2] == DEC_LOCATION && wc >= 4) d->location = (int16_t)w[i + 3], d->has_loc = 1;
            if (w[i + 2] == DEC_BLOCK) d->block = 1;
         }
         break;
      case OP_DECORATION_GROUP:   /* its OpDecorates are already collected against the group id */
         break;
      case OP_GROUP_DECORATE:     /* OpGroupDecorate %group %t...: SPIR-V applies the group's decorations to each target */
         if (wc >= 3 && w[i + 1] < bound && deco[w[i + 1]].has) {
            const struct gs_deco g = deco[w[i + 1]];
            for (unsigned k = 2; k < wc; k++) {
               if (w[i + k] >= bound)
                  continue;
               struct gs_deco *t = &deco[w[i + k]];
               t->has = 1;
               if (g.has_builtin && !t->has_builtin) t->builtin = g.builtin, t->has_builtin = 1;
               if (g.has_loc && !t->has_loc) t->location = g.location, t->has_loc = 1;
               if (g.block) t->block = 1;
            }
         }
         break;
      case OP_MEMBER_DECORATE:
         if (wc >= 5 && w[i + 3] == DEC_BUILTIN && nmdeco < sizeof mdeco / sizeof *mdeco)
            mdeco[nmdeco++] = (struct gs_mdeco){.type = w[i + 1], .member = (uint8_t)w[i + 2], .builtin = (int16_t)w[i + 4]};
         break;
      default: break;
      }
      i += wc;
   }
   if (!have_entry) {
      rc = refuse(why, whysz, "no Geometry entry point");
      goto done;
   }

   /* ---- execution modes: input/output primitive, 1-in/1-out, one invocation, no stream output ---- */
   {
      unsigned out_prim = 0, out_verts = 0, invocations = 1, xfb = 0;
      for (uint32_t i = 5; i + 2 < nwords;) {
         const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
         if (!wc || i + wc > nwords)
            break;
         if (op == OP_EXECUTION_MODE && w[i + 1] == entry_func && wc >= 3) {
            const uint32_t mode = w[i + 2], lit = wc >= 4 ? w[i + 3] : 0;
            switch (mode) {
            case EM_INPUT_POINTS: in_prim = MALI_GS_POINTS; break;
            case EM_INPUT_LINES: in_prim = MALI_GS_LINES; break;
            case EM_INPUT_TRIANGLES: in_prim = MALI_GS_TRIANGLES; break;
            case EM_INPUT_LINES_ADJ:
            case EM_INPUT_TRIANGLES_ADJ:
               rc = refuse(why, whysz, "adjacency input primitive (execution mode %u)", mode);
               goto done;
            case EM_OUTPUT_POINTS: out_prim = EM_OUTPUT_POINTS; break;
            case EM_OUTPUT_LINE_STRIP: out_prim = EM_OUTPUT_LINE_STRIP; break;
            case EM_OUTPUT_TRIANGLE_STRIP: out_prim = EM_OUTPUT_TRIANGLE_STRIP; break;
            case EM_OUTPUT_VERTICES: out_verts = lit; break;
            case EM_INVOCATIONS: invocations = lit ? lit : 1; break;
            case EM_XFB: xfb = 1; break;
            default: break;
            }
         }
         i += wc;
      }
      if (xfb) {
         rc = refuse(why, whysz, "stream output from the geometry shader");
         goto done;
      }
      if (invocations != 1) {
         rc = refuse(why, whysz, "%u invocations per primitive (1 needed)", invocations);
         goto done;
      }
      if (!in_prim) {
         rc = refuse(why, whysz, "no input primitive execution mode");
         goto done;
      }
      const unsigned n_in = in_prim == MALI_GS_POINTS ? 1 : in_prim == MALI_GS_LINES ? 2 : 3;
      const unsigned want_out = in_prim == MALI_GS_POINTS ? EM_OUTPUT_POINTS
                             : in_prim == MALI_GS_LINES ? EM_OUTPUT_LINE_STRIP : EM_OUTPUT_TRIANGLE_STRIP;
      if (out_prim != want_out) {
         rc = refuse(why, whysz, "output primitive mode %u does not match the input primitive family", out_prim);
         goto done;
      }
      if (out_verts != n_in) {
         rc = refuse(why, whysz, "max_vertices %u is not %u (1-in/1-out needed)", out_verts, n_in);
         goto done;
      }

      /* ---- the entry function body ---- */
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
         rc = refuse(why, whysz, "geometry entry function not found");
         goto done;
      }

      /* A pass-through is a straight line: check the shape of the block graph first, so a loop is named as a loop
       * instead of failing on whatever its counter does first. */
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
                  rc = refuse(why, whysz, "control flow (opcode %u): a loop or branch needs lane 2", op);
                  goto done;
               }
            }
            i += wc;
         }
         if (blocks != 1) {
            rc = refuse(why, whysz, "%u basic blocks in the body (a straight-line pass-through has one)", blocks);
            goto done;
         }
      }

      int32_t emit_k = 0;
      int ended = 0, saw_position = 0, in_body = 0;
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
            i += wc;   /* the OpFunction header itself is not body code */
            continue;
         } else if (op == OP_FUNCTION_END) {
            break;
         }
         if (!in_body) {
            i += wc;
            continue;
         }
         switch (op) {
         case OP_LABEL:   /* exactly one, checked above */
            break;
         case OP_BRANCH: case OP_BRANCH_CONDITIONAL: case OP_SWITCH:
            rc = refuse(why, whysz, "control flow (opcode %u): a loop or branch needs lane 2", op);
            goto done;
         case OP_LOOP_MERGE: case OP_SELECTION_MERGE:
            rc = refuse(why, whysz, "structured control flow (opcode %u) needs lane 2", op);
            goto done;
         case OP_VARIABLE:
            /* A local's *declaration* is harmless; every use of it is refused below (a store to it is "not an output",
             * a load from it "not an input"), so a loop's counter names the loop, not itself. */
            if (wc >= 4 && w[i + 3] != SC_FUNCTION) {
               rc = refuse(why, whysz, "a variable of storage %u in the geometry-shader body", w[i + 3]);
               goto done;
            }
            break;
         case OP_ACCESS_CHAIN: case OP_IN_BOUNDS_ACCESS_CHAIN: {
            if (wc < 4)
               break;
            struct gs_chain *c = &chain[w[i + 2]];
            const struct gs_chain *b = &chain[w[i + 3]];
            if (b->base < 0) {
               c->base = -1;
               c->n = -1;
               break;
            }
            const int8_t n = (int8_t)(wc - 4);
            if (n > GS_MAX_PATH) {
               c->base = -1;
               c->n = -1;
               break;
            }
            *c = (struct gs_chain){.base = b->base, .n = n, .storage = b->storage,
                                   .pointee = type[w[i + 1]].kind == OP_TYPE_POINTER ? type[w[i + 1]].a : 0};
            for (int8_t k = 0; k < n; k++) {
               const int32_t v = konst[w[i + 4 + k]];
               c->idx[k] = v < 0 ? GS_IDX_UNKNOWN : (uint32_t)v;
               if (v < 0)
                  c->n = -1;
            }
            break;
         }
         case OP_PTR_ACCESS_CHAIN:
            rc = refuse(why, whysz, "OpPtrAccessChain (a descriptor or storage access)");
            goto done;
         case OP_LOAD:
            if (wc >= 4) {
               const struct gs_chain *c = &chain[w[i + 3]];
               /* Per-primitive builtins exist only inside a geometry stage, and glslang declares gl_PrimitiveID as an
                * *output* variable it also reads, so this is checked before the storage class: our compiled program is the
                * vertex shader, which has no per-primitive input to read. */
               if (c->base >= 0 && deco[c->base].has_builtin && (deco[c->base].builtin == BI_PRIMITIVEID || deco[c->base].builtin == BI_INVOCATIONID)) {
                  rc = refuse(why, whysz, "reads gl_%s: a vertex program has no per-primitive input",
                              deco[c->base].builtin == BI_PRIMITIVEID ? "PrimitiveID" : "InvocationID");
                  goto done;
               }
               if (c->base < 0 || c->storage != SC_INPUT) {
                  rc = refuse(why, whysz, "a read that is not a geometry-shader input (storage %u)",
                              c->base < 0 ? 0xffu : c->storage);
                  goto done;
               }
               load_ptr[w[i + 2]] = (int32_t)w[i + 3];
            }
            break;
         case OP_STORE: {
            if (wc < 3)
               break;
            const struct gs_chain *c = &chain[w[i + 1]];
            const int32_t val = (int32_t)w[i + 2];
            if (c->base < 0 || c->n < 0) {
               rc = refuse(why, whysz, "OpStore through a pointer this analysis cannot name (word %u)", i);
               goto done;
            }
            if (c->storage != SC_OUTPUT) {
               rc = refuse(why, whysz, "a store that is not a geometry-shader output (storage %u): a side effect the vertex program cannot express",
                           c->storage);
               goto done;
            }
            if (ended) {
               rc = refuse(why, whysz, "an output written after OpEndPrimitive");
               goto done;
            }
            if (val < 0 || val >= (int32_t)bound || load_ptr[val] < 0) {
               rc = refuse(why, whysz, "an output is computed, not copied from the input vertex (word %u)", i);
               goto done;
            }
            const struct gs_chain *ic = &chain[load_ptr[val]];
            const int32_t ibase = ic->base;
            if (type[chain[ibase].pointee].kind != OP_TYPE_ARRAY) {
               rc = refuse(why, whysz, "geometry input without a vertex array");
               goto done;
            }
            if (ic->n < 1) {
               rc = refuse(why, whysz, "a whole input array is read, not one vertex");
               goto done;
            }
            if ((int32_t)ic->idx[0] != emit_k) {
               rc = refuse(why, whysz, "vertex %d is copied while emitting vertex %d (cross-vertex read)",
                           (int)ic->idx[0], (int)emit_k);
               goto done;
            }
            /* key of both sides: the BuiltIn of the block member the path names, or the variable's Location */
            int8_t okind, ikind, o_skip = 0, i_skip = 0;
            int16_t oval = 0, ival = 0;
            if (!key_of(c, 0, chain, type, deco, mdeco, nmdeco, &okind, &oval, &o_skip, why, whysz)) {
               rc = 0;
               goto done;
            }
            if (okind == 1 && (oval == BI_LAYER || oval == BI_VIEWPORTINDEX)) {
               rc = refuse(why, whysz, "writes gl_%s: no vertex-program equivalent", oval == BI_LAYER ? "Layer" : "ViewportIndex");
               goto done;
            }
            if (okind == 1 && oval == BI_POSITION)
               saw_position = 1;
            if (!key_of(ic, 1, chain, type, deco, mdeco, nmdeco, &ikind, &ival, &i_skip, why, whysz)) {
               rc = 0;
               goto done;
            }
            if (!same_varying(c, ic, o_skip, i_skip, okind, oval, ikind, ival)) {
               rc = refuse(why, whysz, "output %s%d is not a copy of input %s%d (pointee 0x%x/0x%x)",
                           okind ? "builtin " : "location ", oval, ikind ? "builtin " : "location ", ival, c->pointee, ic->pointee);
               goto done;
            }
            /* remember it for this emit */
            unsigned k = 0;
            for (; k < nout; k++)
               if (out[k].kind == okind && out[k].val == oval && out[k].pointee == c->pointee &&
                   out[k].npath == (int8_t)(c->n - o_skip) &&
                   !memcmp(out[k].path, &c->idx[o_skip], sizeof(uint32_t) * (size_t)(c->n - o_skip)))
                  break;
            if (k == nout) {
               if (nout == GS_MAX_OUT) {
                  rc = refuse(why, whysz, "more than %d outputs", GS_MAX_OUT);
                  goto done;
               }
               nout++;
               out[k].kind = okind;
               out[k].val = oval;
               out[k].pointee = c->pointee;
               out[k].npath = (int8_t)(c->n - o_skip);
               memcpy(out[k].path, &c->idx[o_skip], sizeof(uint32_t) * (size_t)(c->n - o_skip));
            }
            out[k].stored_k = emit_k;
            break;
         }
         case OP_EMIT_VERTEX: {
            if (ended) {
               rc = refuse(why, whysz, "OpEmitVertex after OpEndPrimitive");
               goto done;
            }
            if (!nout || !saw_position) {
               rc = refuse(why, whysz, "no gl_Position written before EmitVertex");
               goto done;
            }
            for (unsigned k = 0; k < nout; k++)
               if (out[k].stored_k != emit_k) {
                  rc = refuse(why, whysz, "output %s%d kept across emits (value from vertex %d, emitting vertex %d)",
                              out[k].kind ? "builtin " : "location ", out[k].val, (int)out[k].stored_k, (int)emit_k);
                  goto done;
               }
            emit_k++;
            break;
         }
         case OP_END_PRIMITIVE:
            if (emit_k != (int32_t)n_in) {
               rc = refuse(why, whysz, "OpEndPrimitive after %d of %u vertices", (int)emit_k, n_in);
               goto done;
            }
            ended = 1;
            break;
         case OP_EMIT_STREAM_VERTEX: case OP_END_STREAM_PRIMITIVE:
            rc = refuse(why, whysz, "a multi-stream emit (opcode %u)", op);
            goto done;
         case OP_RETURN:
            if (emit_k != (int32_t)n_in) {
               rc = refuse(why, whysz, "%d vertices emitted, %u expected (amplification or a loop)", (int)emit_k, n_in);
               goto done;
            }
            break;
         case OP_RETURN_VALUE:
            rc = refuse(why, whysz, "OpReturnValue in a geometry shader");
            goto done;
         case OP_LINE: case OP_NO_LINE: case OP_NOP:
            break;
         default:
            /* Values may be computed — the store check below is what refuses an output that is not a copy of input
             * vertex k, and that is the reason a reviewer needs to see. Only *pure* instructions are waved through:
             * composite/vector construction and arithmetic (79..84, 126..217). Anything else — a texture fetch, a
             * descriptor read, a call — is refused by number, because it reads state the vertex program does not have. */
            if ((op >= 79 && op <= 84) || (op >= 126 && op <= 217))
               break;
            rc = refuse(why, whysz, "instruction %u in the geometry-shader body: only copies of the input vertices and EmitVertex are accepted", op);
            goto done;
         }
         i += wc;
      }
      if (rc)
         goto done;
      if (emit_k != (int32_t)n_in) {
         rc = refuse(why, whysz, "%d vertices emitted, %u expected (amplification or a loop)", (int)emit_k, n_in);
         goto done;
      }
      /* outputs ⊇ inputs: every Location the GS reads must also be written, or the fragment stage loses it */
      for (uint32_t v = 0; v < bound; v++) {
         if (chain[v].base != (int32_t)v || chain[v].storage != SC_INPUT || !deco[v].has || deco[v].has_builtin || deco[v].block)
            continue;
         int found = 0;
         for (uint32_t o = 0; o < bound && !found; o++)
            if (chain[o].base == (int32_t)o && chain[o].storage == SC_OUTPUT && deco[o].has && !deco[o].has_builtin &&
                deco[o].location == deco[v].location)
               found = 1;
         if (!found) {
            rc = refuse(why, whysz, "input location %d is not written as an output location", deco[v].location);
            goto done;
         }
      }
   }

   if (prims)
      *prims = in_prim;
   rc = 1;

done:
   free(type);
   free(deco);
   free(chain);
   free(load_ptr);
   free(konst);
   return rc;
}
