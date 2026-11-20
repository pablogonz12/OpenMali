/* SPDX-License-Identifier: MIT */
/* M13 offline test: ops frame CS words (timestamp, availability, event set/reset/wait, flush) + ring dump for decode_stream.py's static
 * rules (async FLUSH_CACHE2/STORE_STATE need WAIT slot 0 before RUN_*). Usage: t_query <out.jsonl> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/mali_event.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define OPC(w) ((unsigned)((w) >> 56))

static unsigned count(const uint64_t *c, unsigned n, unsigned op) { unsigned k = 0; for (unsigned i = 0; i < n; i++) k += OPC(c[i]) == op; return k; }

int main(int argc, char **argv)
{
   FILE *jf = argc > 1 ? fopen(argv[1], "w") : NULL;
   for (unsigned arch = 11; arch <= 13; arch += 2) {
      struct mali_env env = { .arch = arch, .sync_va = 0x7f0000009000ull };
      struct mali_op ops[] = {
         { MALI_OP_TIMESTAMP, 0x7f0000100008ull, 0x7f0000100000ull, 0 },
         { MALI_OP_AVAIL, 0x7f0000200000ull, 0, 1 },
         { MALI_OP_AVAIL, 0x7f0000200000ull, 0, 0 },
         { MALI_OP_EVENT_WAIT, 0x7f0000200000ull, 0, 0 },
         { MALI_OP_FLUSH, 0, 0, 0 },
      };
      uint64_t *cs; unsigned n;
      CHECK(mali_ops_build(&env, ops, 5, &cs, &n) == 0, "build");
      CHECK(count(cs, n, 40) == 1 && count(cs, n, 39) == 1 && count(cs, n, 38) == 3 && count(cs, n, 51) == 1, "op counts st=%u wait=%u set=%u add=%u",
            count(cs, n, 40), count(cs, n, 39), count(cs, n, 38), count(cs, n, 51));
      /* every STORE_STATE and FLUSH_CACHE2 must be followed by a WAIT of slot 0 before any SYNC/other consumer */
      for (unsigned i = 0; i < n; i++) if (OPC(cs[i]) == 40 || OPC(cs[i]) == 36) {
         unsigned j = i + 1;
         while (j < n && OPC(cs[j]) != 3 && OPC(cs[j]) != 40 && OPC(cs[j]) != 36) j++;
         CHECK(j < n && OPC(cs[j]) == 3 && ((cs[j] >> 16) & 1), "async op at %u not followed by WAIT slot 0", i);
      }
      for (unsigned i = 0; i < n; i++) if (OPC(cs[i]) == 40) {
         CHECK(((cs[i] >> 32) & 0xff) == 0 && ((cs[i] >> 48) & 0xf) == 0, "store_state state/slot");
      }
      for (unsigned i = 0; i < n; i++) if (OPC(cs[i]) == 39) CHECK(((cs[i] >> 28) & 0xf) == 1, "cond greater");
      CHECK(OPC(cs[n - 1]) == 51, "ends with SYNC_ADD64");
      if (jf) {
         unsigned pad = (8 - n % 8) % 8;
         fprintf(jf, "{\"ev\":\"ring\",\"va\":\"0x7f0000300000\",\"size\":4096,\"start\":0,\"insert\":%u,\"hex\":\"", (n + pad) * 8);
         for (unsigned i = 0; i < n + pad; i++) { uint64_t w = i < n ? cs[i] : 0; for (int k = 0; k < 8; k++) fprintf(jf, "%02x", (unsigned)(w >> (8 * k)) & 0xff); }
         fprintf(jf, "\"}\n");
      }
      free(cs);
   }
   /* Two devices: a light ops frame built for device A must leave the head-flush request with A. Device B's submission taking its own
    * flag must not consume it (a process-global flag lost A's flush), and B's frame must not set A's. */
   {
      int pa = 0, pb = 0;
      struct mali_env ea = { .arch = 13, .sync_va = 0x7f0000009000ull, .light_pending = &pa };
      struct mali_env eb = { .arch = 13, .sync_va = 0x7f000000a000ull, .light_pending = &pb };
      struct mali_op lo[] = { { MALI_OP_AVAIL, 0x7f0000200000ull, 0, 1 } };
      struct mali_op fo[] = { { MALI_OP_FLUSH, 0, 0, 0 } };
      uint64_t *c1; unsigned k1;
      CHECK(mali_ops_build(&ea, lo, 1, &c1, &k1) == 0, "light build A");
      free(c1);
      CHECK(mali_ops_take_light(&pb) == 0, "device B's submission consumed device A's pending head flush");
      CHECK(mali_ops_build(&eb, fo, 1, &c1, &k1) == 0, "heavy build B");
      free(c1);
      CHECK(pa == 1 && pb == 0, "heavy frame of B touched the flags (a=%d b=%d)", pa, pb);
      CHECK(mali_ops_take_light(&pa) == 1, "device A lost its pending head flush");
      CHECK(mali_ops_take_light(&pa) == 0, "flag not cleared by take");
   }
   struct mali_env bad = { .arch = 9 };
   uint64_t *cs; unsigned n;
   CHECK(mali_ops_build(&bad, NULL, 0, &cs, &n) != 0, "arch 9 rejected");
   if (jf) fclose(jf);
   printf("t_query: %s\n", g_fail ? "FAIL" : "ok");
   return g_fail != 0;
}
