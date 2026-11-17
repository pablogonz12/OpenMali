# Curating LESSONS.md

Trigger: LESSONS.md reaches 15 lines, or once a week, whichever comes first. Run by one curator (a worker or the owner), in a worktree.

For each lesson line, choose exactly one:
1. **Script check (preferred).** Turn it into an automatic check: a case in `vk/check.sh`, a guard in a build/test script,
   a farm assertion or a lint rule. Add the check in the same commit, and show that it fails without the fix.
2. **Pitfall.** Add one line to "Pitfalls" in SKILL.md, only if it was hit at least twice or cost over 30 minutes.
   Merge it into an existing entry when it fits. SKILL.md keeps at most 20 pitfalls and 120 lines: drop the least useful
   entry (or one now enforced by a script) to make room.
3. **Drop.** One-off, already fixed for good, or not reproducible. Say why in the commit message.

Then:
- Empty LESSONS.md (keep the file, zero lines).
- Run `tools/skill-lint.sh`; it must pass (no serials, IPs, emails or home-directory paths).
- Commit as `skill: curate N lessons (x check, y pitfall, z drop)`, listing each lesson and its fate in the body.
- The owner reviews the diff before merge. The curator never pushes.

Evidence on each lesson line (commit, file:line or run id) is what lets the curator verify it. No evidence, no promotion.
