# Review checklist (lessons from automated review)

Recurring findings from CodeRabbit reviews of this repo, grouped by theme. Run through it before opening a PR.
Nothing here is enforced by `check.sh`; a grep for any of these would have false positives on current main.

## Vulkan driver
- **Limits and features.** Before raising a limit or advertising a feature, grep the tests and `R*-PLAN.md` for its ceiling. Use a value proven on this hardware (PanVK oracle) and add a bindability invariant (UBO range <= storage range, `maxBufferSize`).
- **Name-only extensions.** Read every core entry point the extension exposes for silent skips (a `VK_SUCCESS` that did nothing). Return an error for what is unimplemented, and pin extension name == feature bit in `t_steam_caps`.
- **Format tables.** When adding a render-target format row, grep every per-format switch (preload/`loadOp LOAD`, resolve, clear) for the sibling formats. Assert `FormatProperties3` bits against feature bits in a sweep over all formats.
- **Second bind point, queue or stage.** Check whether existing state is per-point (e.g. `dyn_desc`/`set_va` shared by graphics and compute). Open a task instead of copying a shared array.
- **Tests.** Zero the whole buffer when a test draws or fetches more elements than it initialises, and say in the comment what the extra reads see. Tests must fail without the fix (mutation-check).
- **C helpers.** Guard `memcpy`/`memset` with `if (bytes)` when the pointer may be NULL for a zero-size layout.
- **Spec docs.** Classify each "required" item as unconditional or conditional on an extension/feature. Grep the code before calling anything a no-op.

## Tools and farm scripts
- **Untrusted text.** Sanitise control characters from guest/device output before printing. Normalise `/proc` paths (` (deleted)` suffix).
- **"Nothing" vs "unavailable".** A failed query is not an empty result: exit nonzero. Reports that swallow failures (`|| true`) must count them and print INCOMPLETE.
- **CLI inputs.** Resolve optional arguments once and guard each consumer. Validate numeric options (reject `<= 0`). Check every external call (adb, guest) and delete stale outputs at fixed names first.
- **Docs vs CLI.** Check every command and flag in a doc against the checked-out CLI's `--help` and parser. A restore/enable step must undo only what the recipe itself changed.
- **Placeholders.** Use `<ip:port>` in docs and examples, and run `tools/private-lint.sh` before pushing.
- **Time.** Parse ISO timestamps with `fromisoformat`, never by slicing and `mktime`.
- **Re-entrant acquire.** A second call must reuse a resource the object already holds. Never use an env marker you set yourself to decide that someone else holds the lock.

## Workers, leases and schedulers
- **Races.** Write the liveness marker before the record. Add an explicit "ending" state before teardown. Snapshot config for a long-lived worker.
- **Exit paths.** List every exit path (launch error, restart, disable, cancel, early return, thermal stop) and test it or note it. Clean registrations in `finally`. Discard partial output of an invalid run before a retry.
- **Dedupe markers.** Clear "seen/queued" markers on every cancel or skip path. Re-read persisted state before time-gated or irreversible decisions. Every path that finishes a job must run its cleanup.
- **Memoisation.** Key reuse on everything that changes the verdict (baseline, app version, class set, commit SHA) and limit it to safe suites. Name artifacts by the commit that produced them, not "current tip".
- **A/B measurement.** Filter at the unit compared (round), not the single run. Reset parser state at every record boundary. Sample the gating quantity (temperature) after the run too. Chunk remote calls under the 600 s cap. Validate names as unique identifiers.
- **Fix coverage.** Trace the fix's input to every caller. Build reference sets from post-merge state. Record the hash of the bytes actually used.

## Automation safety
- Match the exact component you are allowed to touch (no broad regex such as "permission" that could act on the owner's unrelated screen). Order checks so the specific classification runs before generic verification.
- Close or terminate open blocks (`parse_qpa` unfinished) before concatenating partial logs.
- Run `check.sh` before opening a PR that changes the driver.
