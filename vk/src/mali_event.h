/* SPDX-License-Identifier: MIT */
/* M13, Mesa-free core of barriers, events and timestamp/availability writes as a small CS "ops frame" (mali_ops_build), and the query
 * pool layout. An ops frame is a frame of its own (like a compute frame): same prologue/epilogue conventions (flush, WAIT slot 0, then
 * SYNC_ADD64 of env->sync_va), so frames stay serialised and ordered with everything else.
 *
 * Query pool entry (16 B): { u32 avail; u32 pad; u64 value }. Pool memory is zeroed by reset (fill job). Event = one u32 word. */
#ifndef MALI_EVENT_H
#define MALI_EVENT_H

#include <stdint.h>
#include "mali_cs_emit.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MALI_QUERY_ENTRY 16u
#define MALI_QUERY_VALUE_OFF 8u

enum mali_op_kind {
   MALI_OP_FLUSH = 0,       /* clean L2+LSC (barrier for cached memory), then WAIT slot 0 */
   MALI_OP_TIMESTAMP,       /* STORE_STATE Timestamp -> u64 at va, WAIT slot 0, then (va2 != 0) SYNC_SET32 1 at va2 (availability) */
   MALI_OP_AVAIL,           /* SYNC_SET32 value `val` (0/1) at va (event set/reset, query availability) */
   MALI_OP_EVENT_WAIT,      /* SYNC_WAIT32 until u32 at va > 0 */
};
struct mali_op { enum mali_op_kind kind; uint64_t va, va2; uint32_t val; };

/* CS in a malloc'd array (caller frees). 0, -EINVAL (arch) or -ENOMEM. */
/* True once after a light ops frame (no own flush) was built; the caller must then run submit_signal for its head flush. */
int mali_ops_take_light(int *pending);   /* pending = env.light_pending of the submitting device */
int mali_ops_build(const struct mali_env *env, const struct mali_op *ops, unsigned nops, uint64_t **cs, unsigned *ninstr);

#ifdef __cplusplus
}
#endif
#endif
