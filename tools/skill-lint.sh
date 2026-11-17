#!/bin/bash
# SPDX-License-Identifier: MIT
# tools/skill-lint.sh            lint .opencode/skills/ of this checkout
# tools/skill-lint.sh --check-text "<text>"   lint one string (used by tools/lesson)
set -uo pipefail
dev=${MALI_FARM_DEVICES:-$HOME/.config/mali-farm/devices.toml}
serials=()
[ -f "$dev" ] && while IFS= read -r s; do [ -n "$s" ] && serials+=("$s"); done < <(sed -n 's/^[[:space:]]*serial[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "$dev")

# prints a reason and returns 1 if stdin contains private data
scan() {
   local t; t=$(cat)
   for s in "${serials[@]}"; do grep -qF -- "$s" <<<"$t" && { echo "adb serial from devices.toml"; return 1; }; done
   grep -qE '(^|[^0-9A-Za-z])[0-9]{1,3}(\.[0-9]{1,3}){3}([^0-9]|$)' <<<"$t" && { echo "IP address"; return 1; }
   grep -qE '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}' <<<"$t" && { echo "email address"; return 1; }
   grep -qE '/home/' <<<"$t" && { echo "/home/ path"; return 1; }
   grep -qE '(^|[^A-Za-z0-9])(([0-9]+[A-Z]|[A-Z]+[0-9])[A-Z0-9]{9,}|[0-9a-f]{12,})([^A-Za-z0-9]|$)' <<<"$t" && { echo "long hex/alphanumeric id (adb serial?)"; return 1; }
   return 0
}

if [ "${1:-}" = "--check-text" ]; then printf '%s\n' "${2:-}" | scan; exit $?; fi

root=$(cd "$(dirname "$0")/.." && pwd)
d=$root/.opencode/skills
fail=0
err() { echo "FAIL: $*"; fail=1; }
skill=$d/mali-open/SKILL.md
if [ -f "$skill" ]; then
   n=$(wc -l <"$skill"); [ "$n" -le 120 ] || err "SKILL.md has $n lines (max 120)"
   p=$(sed -n '/^## Pitfalls/,$p' "$skill" | grep -cE '^[0-9]+\.'); [ "$p" -le 20 ] || err "$p pitfalls (max 20)"
else err "missing $skill"; fi
[ -f "$d/mali-open/CURATE.md" ] && { c=$(wc -l <"$d/mali-open/CURATE.md"); [ "$c" -lt 25 ] || err "CURATE.md has $c lines (max 24)"; }
while IFS= read -r f; do
   r=$(scan <"$f") || err "$f: $r"
done < <(find "$d" -type f)
[ $fail = 0 ] && echo "skill-lint: OK"
exit $fail
