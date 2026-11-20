# 2026-11-20-invalid — archived benchmark store (not deleted)

This directory is the previous benchmark store, kept as evidence after the owner's verdict:
"steam menu never reached 421 fps, that's fake. Reset all findings and measurements and start
from scratch." It is invalid as a whole because `fps` was filled from a *render-side* or
*producer-side* rate instead of the fps a user can see on the panel. The clearest row: the store
entry `ts 2026-11-18T11:26:38`, `device x8`, `stack bridge`, `version v25`, `fps 421.1` was taken
from a menubench run whose raw row has `profile=uncapped`, `render.avg_fps 421.1`, but
`screen.fps 87.8` on a `panel_hz 120` panel — 421.1 is therefore a rate at which frames were
*submitted*, not displayed, and is 3.5× the panel's physical ceiling. The second example,
`fps 850.613` for `triangles`/`x8`/`mali-open`, came from `bench/results.jsonl`
`presented.fps` for the `triangle-3k` bench (raw row: `presented.fps 850.613`,
`offscreen.fps 663.727`) — an offscreen/present producer rate for a test that never presented a
visible scene at that rate. Every chart in `BENCH.md` (archived here unchanged) inherited those
numbers, so no row in this store is treated as a valid displayed-fps measurement. The files are
frozen: `bench.jsonl` (244 rows), `bench-seed.jsonl` (14 hand-typed rows), `bench-gaps.json` and
the generated `BENCH.md`.

## What happened to the 244 rows

Re-imported under `docs/MEASURE.md` on 2026-11-20, **22 survived** as displayed-fps measurements
(all from menubench `screen.fps`, all `steam-menu`; 21 `visible`, 1 `black`). The other **222 were
rejected**:

| reason | rows |
|---|---|
| `presented_fps_not_displayed` (bench/results.jsonl `presented.fps`) | 153 |
| `not_ok` (menubench session not ready / no frames / selfcheck failed) | 43 |
| `doc_claim_not_measurable` (FINDINGS.md, MENUBENCH.md and owner notes: no displayed-fps source, no `panel_hz`, no `reps`) | 14 |
| `uncapped_profile` (the 421.1 row is one of these) | 10 |
| `render_avg_not_displayed` (bench/menu.jsonl `avg_fps`) | 2 |
| **total rejected** | **222** |

Full per-row reasons: `docs/rejected.jsonl`. The surviving rows are in `docs/bench.jsonl` with
`metric: "displayed_fps"` and their panel ceiling; the new charts draw that ceiling so a point
above it is visibly impossible.
