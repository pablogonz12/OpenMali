# featuremap — the requirements chart, generated from public sources

`tools/featuremap.py` builds `docs/featuremap.md` + `docs/featuremap.json`: one row per capability a
consumer needs, where the requirement comes from (public source, file:line, pinned revision), what our
driver reports (`M/phones/<phone>/caps-driver.txt`) and what the bridge reports (`caps-bridge.txt`),
whether the hardware can do it honestly / only emulated / not at all, the lane-day cost, the test that
covers it, and a status.

    python3 tools/featuremap.py                 # write docs/featuremap.md + .json, print a summary
    python3 tools/featuremap.py --missing dxvk  # numbered list of what dxvk needs and we lack
    python3 tools/featuremap.py --freeze        # re-pin file:line in consumers/*.json after an upstream bump
    python3 tools/featuremap.py --audit         # what upstream added/removed since our pin (no write)
    python3 tools/featuremap.py --no-fetch      # offline: use only ~/.cache/featuremap/src

Data files (all in `tools/featuremap/`):

| file | what it is |
|---|---|
| `sources.lock.json` | every source: URL, pinned revision, licence, sha256 of the bytes we read, fetch policy (`local` first, else fetch). Nothing is vendored. |
| `consumers/*.json` | one per consumer: the extract spec (regex or explicit rows) + annotations for each requirement |
| `annotations.json` | per vk member / gl ext: hardware class (`honest`/`emulated`/`impossible`), lane-days, what it unlocks, which D3D level it gates — every entry carries the note it comes from |
| `tests.json` | requirement id → test ids, and the check.sh case each test id names (verified to exist) |
| `games.json` | the games in the owner's library we have evidence for → the consumer they run and the requirement that blocks them |
| `falsify.json` | five rows whose measured fact is in `M/notes/*`, re-checked on every run |

A row is only printed when its `match` string is found, at the pinned line or (with a `DRIFT` warning)
elsewhere in the pinned file. A requirement whose source line is gone is dropped and listed in
"Gaps" — the generator never guesses.
