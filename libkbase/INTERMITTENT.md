# Intermittent "nothing binned" (t_triangle, ~0.4 %/iteration, Mali-G615 CMF)
Status 2026-11-16: analysed offline from `runs/20261116-cmf-libkbase/loop-*.jsonl`. Fixes ran on hardware 2026-11-16 (qdirect cmf, see Results): 0 failures everywhere, but the OLD behaviour did not reproduce either, so the bisect is INCONCLUSIVE.

Signature: fence signals normally, no fault, target = clear value (the fragment job ran), tiler ctx at readback = the CPU-written bytes
(`tiler_polylist_hi16`=0, `tiler_state_w0`=0). So the fragment job read an empty tiler context and the tiler's own updates never reached that memory.
Data: default 5/1144, `--fresh-heap` 2/300, `--fresh-geo` 7/300, `--arena-adv` 0/900, `--loop-reset-ctx` 0/300; failures cluster in slow (loaded) runs.

## Ranked hypotheses

**H1 (~75 %): the head `FLUSH_CACHE2(3,3,2)` was never waited on, so RUN_IDVS raced the L2 invalidate.**
FLUSH_CACHE2 is always asynchronous (Mesa `cs_instr_is_asynchronous`); PanVK and kraid always emit `cs_wait_slot` on its signal slot. Our stream
went flush -> ~35 MOVEs -> RUN_IDVS with no WAIT on slot 0 (the old header comment "wait mask 0 = synchronous" was wrong). The previous draw's tail
flush is clean-only, so its GPU-written tiler ctx (state w0=1, list pointers into chunks FINISH_FRAGMENT already returned) stays valid in L2 at the
same VA. If the tiler reads it before the invalidate lands, it bins into the dead list; the fragment job then reads the fresh, empty CPU copy.
Predicts: needs the same tiler-ctx VA as the previous draw and a GPU that stayed powered (load); fresh heap/geo irrelevant; new VA or new context cures it.
This is the only hypothesis that matches all five rates.

**H2 (~12 %): state keyed on the tiler-ctx VA outside L2** (tiler hardware or firmware keeps the last context loaded and skips the re-read).
Predicts: same table as H1, but the WAIT does not help; only a different VA (rolling arena) or a new context does.

**H3 (~8 %): CPU descriptor writes not yet in RAM.** The arena is a Normal-NC (write-combining) mapping and only a DMB preceded the submit.
Predicts: `--arena-adv` should NOT help after the first 32 KB lap (it did: 0/900), and failures vanish with any delay before submit. Disfavoured.

**Refuted (~5 % together):** heap context or heap descriptor reset (`--fresh-heap` 2/300), geometry buffer reuse (`--fresh-geo` 7/300), fence/CQS ordering
(the target shows the clear colour, so the whole CS ran before the signal).

## Experiments (out/t_triangle-aarch64, run under the same load as the failing runs, e.g. compositor/game active)

0. Fix check: `--loop 600` x3. Expect 0/1800 (the old rate gives ~8).
1. H1: baseline `--loop 1200 --arena-rewind --no-flush-wait` (old stream, expect ~5 failures; if 0, the load is too low to judge anything),
   then `--loop 1200 --arena-rewind`. H1 predicts 0 (p ~ 0.005 at the old rate). Only the WAIT differs between the two.
2. H2: same second run. Any failure with `--arena-rewind` and the WAIT on, while the default (rolling) stays clean, means the stale state is not in L2:
   keep the rolling arena as the fix and move tiler-ctx init into the CS (STORE_MULTIPLE before RUN_IDVS, as PanVK `get_tiler_desc`).
3. H3: `--loop 1200 --arena-rewind --no-flush-wait --settle-us 200`. H3 predicts 0; H1 and H2 predict the baseline rate, because the race is inside
   the CS. Keep the delay at 200 us: longer than ~800 us lets the shader cores power off, which empties L2 and hides H1 too.

## What was changed

- `include/kbase_cs.h`: comment corrected; new `kb_cs_emit_flush_wait()` (FLUSH_CACHE2 + WAIT slot 0). `kb_cs_flush_cache2()` is unchanged.
- t_triangle, rung2.h (t_uniform, t_depth), t_texture, t_clear: WAIT on slot 0 right after the head flush. The tail flush was already waited.
- `kb_arena_begin()` (`kbase_draw.h`, `src/draw.c`): rolling arena. A job's descriptors go to the least recently used VAs, wrap at the BO end,
  and never overwrite the job's own allocations. All tests call it instead of `ar.off = 0`. Callers that never call it keep the old behaviour.
  Contract: earlier jobs must have signalled before their memory is lapped (the tests wait on every fence).
- `kb_wmb()` (`kbase.h`): `dsb st` on aarch64. Used for the ring and CS_INSERT in `kb_queue_submit` and before submit in all tests (was DMB).
- t_triangle flags: `--arena-rewind` (old same-VA behaviour), `--no-flush-wait` (old racy stream), `--settle-us N`. `--arena-adv` is now the default
  and accepted as a no-op. The `open` JSON line records the three new options.
- Not changed: the per-draw heap-descriptor rewrite. The data refutes it as the cause, and PanVK's write-once policy is untested with this flow.

Remaining risk: two mitigations now overlap (WAIT and rolling arena), so a clean default run does not say which one fixed it; experiment 1 does.
If experiment 0 still fails, dump with `--dump-on-fail` and compare tiler ctx words 10..13 (chunk list) against a passing iteration.

## Results 2026-11-16 (CMF, qdirect from adb shell, `--loop-max-fail 1000`, raw: runs/20261116-cmf-libkbase/intermittent-*.jsonl)
check.sh: OK. Failures/iterations (95 % upper bound by rule of 3 = 3/n):

| run | fail/iter | upper 95 % |
|---|---|---|
| a default (fix on) t_triangle | 0/3000 | 0.10 % |
| b `--arena-rewind` (wait on) | 0/3000 | 0.10 % |
| c `--arena-rewind --no-flush-wait` (old stream) | 0/3000 | 0.10 % |
| d `--no-flush-wait`, rolling arena | 0/3000 | 0.10 % |
| e t_uniform / t_depth / t_texture | 0/1000 each | 0.30 % |
| f `--loop-reset-ctx` | 0/3000 | 0.10 % |
| g = c under 8 busy CPU loops | 0/3000 | 0.10 % |

Conclusion: the control (c) shows 0 where 0.44 % predicts ~13 (P(0) ~ 2e-6), so this setup does not trigger the bug (old data came from guest/session runs
under compositor/game GPU load; direct shell runs on an idle GPU, CPU load did not help). Neither the WAIT nor the rolling arena can be called necessary or sufficient.
The fix is consistent with the data but unproven. To bisect: repeat c, then b and d, in the guest (qdrv) with the game/compositor active. No library edits were made.

## Guest-mode results 2026-11-16 (qdrv, Steam menu on screen, `--loop-max-fail 1000`; raw: runs/20261116-cmf-libkbase/intermittent-guest-*.jsonl)
Reproduced in guest mode. Same signature in every failure (fence signalled, tiler_state_w0=0, polylist_hi16=0, covered 0). Wilson 95 % intervals:

| run | fail/iter | rate | 95 % CI |
|---|---|---|---|
| c old (`--arena-rewind --no-flush-wait`), 2 runs | 7/3000 (4+3) | 0.23 % | 0.11-0.48 % |
| b `--arena-rewind` (wait ON, old arena) | 3/1500 | 0.20 % | 0.07-0.58 % |
| d `--no-flush-wait` (rolling arena, no wait) | 3/1500 | 0.20 % | 0.07-0.58 % |
| a default (wait + rolling arena), 3 runs | 0/7500 | 0 | 0-0.05 % |

Conclusion: neither the head-flush WAIT alone nor the rolling arena alone removes the bug (each still ~0.2 %, same as old); only both together gave 0/7500
(old rate predicts ~17, P(0) ~ 4e-8). So each is necessary, neither sufficient. This contradicts H1 as the single cause (WAIT alone would have cured it) and
H2 as the single cause (new VA alone would have cured it); it fits two independent races that both end in a stale/empty tiler ctx, or a VA-reuse hazard that
the wait only masks when the VA also changes. Keep the fix ON by default (harmless, PanVK-conformant). Not tested here: game in background (menu was enough), t_uniform/t_depth/t_texture in guest.
