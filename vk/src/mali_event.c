/* SPDX-License-Identifier: MIT */
/* M13 ops frame CS builder, see mali_event.h. Encodings from genxml/v10.xml (STORE_STATE 40, SYNC_WAIT32 39, SYNC_SET32 38). */
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>

#include "kbase_cs.h"
#include "mali_event.h"

#define OP_SYNC_WAIT32 39
#define OP_STORE_STATE 40
#define COND_GREATER 1
#define STATE_TIMESTAMP 0

struct regs { unsigned flush_id, addr, val, zero; };
static const struct regs R11 = { 72, 68, 70, 74 };   /* flush_id / sync_addr / sync_val / (heap-copy regs, unused here) */
static const struct regs R13 = { 80, 84, 86, 82 };

struct b { uint64_t *v; unsigned n, cap; int err; };
static void
E(struct b *b, uint64_t i)
{
   if (b->n == b->cap) {
      unsigned nc = b->cap ? b->cap * 2 : 64;
      uint64_t *nv = realloc(b->v, nc * sizeof *nv);
      if (!nv) {
         b->err = -ENOMEM;
         return;
      }
      b->v = nv;
      b->cap = nc;
   }
   b->v[b->n++] = i;
}

static void
flush(struct b *b, const struct regs *R)
{
   E(b, kb_cs_move32(R->flush_id, 0));
   E(b, kb_cs_flush_cache2(3, 3, 2, R->flush_id));
   E(b, kb_cs_wait(1u << KB_CS_SLOT_LS));
}

int
mali_ops_build(const struct mali_env *env, const struct mali_op *ops, unsigned nops, uint64_t **out, unsigned *nout)
{
   struct b b = { 0 };
   *out = NULL;
   *nout = 0;
   if (env->arch != 10 && env->arch != 11 && env->arch != 13)
      return -EINVAL;
   const struct regs *R = env->arch == 13 ? &R13 : &R11;
   E(&b, kb_cs_nop());
   for (unsigned i = 0; i < nops; i++) {
      const struct mali_op *o = &ops[i];
      switch (o->kind) {
      case MALI_OP_FLUSH:
         flush(&b, R);
         break;
      case MALI_OP_TIMESTAMP:
         /* The GPU timestamp is written asynchronously: wait on slot 0 before anything that depends on it. */
         E(&b, kb_cs_move48(R->addr, o->va));
         E(&b, kb_cs_raw(OP_STORE_STATE, ((uint64_t)R->addr << 40) | ((uint64_t)STATE_TIMESTAMP << 32) | ((uint64_t)KB_CS_SLOT_LS << 48)));
         E(&b, kb_cs_wait(1u << KB_CS_SLOT_LS));
         if (o->va2) {
            E(&b, kb_cs_move48(R->addr, o->va2));
            E(&b, kb_cs_move32(R->val, 1));
            E(&b, kb_cs_sync_set32(R->addr, R->val, KB_CS_SCOPE_SYSTEM));
         }
         break;
      case MALI_OP_AVAIL:
         E(&b, kb_cs_move48(R->addr, o->va));
         E(&b, kb_cs_move32(R->val, o->val));
         E(&b, kb_cs_sync_set32(R->addr, R->val, KB_CS_SCOPE_SYSTEM));
         break;
      case MALI_OP_EVENT_WAIT:
         E(&b, kb_cs_move48(R->addr, o->va));
         E(&b, kb_cs_move32(R->zero, 0));
         E(&b, kb_cs_raw(OP_SYNC_WAIT32, ((uint64_t)R->addr << 40) | ((uint64_t)R->zero << 32) | ((uint64_t)COND_GREATER << 28)));
         break;
      }
   }
   /* Frames that only store timestamps / availability words need no cache maintenance: nothing was read or written through the
    * caches except those words, which the clean flush at the head of the queue-submit tail (kbase_submit_signal) makes visible. */
   bool light = !getenv("MALI_OPS_FLUSH");
   for (unsigned i = 0; i < nops; i++)
      if (ops[i].kind != MALI_OP_TIMESTAMP && ops[i].kind != MALI_OP_AVAIL)
         light = false;
   if (!light)
      flush(&b, R);
   E(&b, kb_cs_move48(R->addr, env->sync_va));
   E(&b, kb_cs_move48(R->val, 1));
   E(&b, kb_cs_sync_add64(R->addr, R->val, KB_CS_SCOPE_SYSTEM));
   if (b.err) {
      free(b.v);
      return b.err;
   }
   *out = b.v;
   *nout = b.n;
   return 0;
}
