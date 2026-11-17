#!/bin/bash
# SPDX-License-Identifier: MIT
# tools/private-lint.sh   fail if any tracked file holds a private identifier:
# adb serials of devices.toml, /home/<user> paths, private IPs, MACs, non-noreply emails, key/token material.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); cd "$root"
dev=${MALI_FARM_DEVICES:-$HOME/.config/mali-farm/devices.toml}
fail=0
hit() { echo "FAIL: $1"; fail=1; }
files() { git ls-files -z; }
# 1. serials from the (untracked) device table
if [ -f "$dev" ]; then
  while IFS= read -r s; do
    [ -n "$s" ] || continue
    files | xargs -0 grep -IlF -- "$s" 2>/dev/null | sed 's/^/  /' | grep -q . && { hit "adb serial from devices.toml in:"; files | xargs -0 grep -IlF -- "$s" | sed 's/^/  /'; }
  done < <(sed -n 's/^[[:space:]]*serial[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "$dev")
fi
chk() { # label, ERE
  local out; out=$(files | xargs -0 grep -IEn -- "$2" 2>/dev/null | grep -v '^tools/private-lint.sh:' | grep -v 'users\.noreply\.github\.com' | cut -d: -f1,2)
  [ -z "$out" ] || { hit "$1:"; echo "$out" | sed 's/^/  /'; }
}
chk "home path"        '/home/[A-Za-z][A-Za-z0-9_-]*/'
chk "private IP"       '(^|[^0-9.])(192\.168|10\.[0-9]{1,3}|172\.(1[6-9]|2[0-9]|3[01]))\.[0-9]{1,3}\.[0-9]{1,3}([^0-9]|$)'
chk "MAC address"      '(^|[^0-9A-Fa-f:])([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}([^0-9A-Fa-f:]|$)'
chk "email address"    '[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+\.[A-Za-z]{2,}'
chk "key/token"        '(ghp_[A-Za-z0-9]{20}|github_pat_|sk-[A-Za-z0-9]{20}|AKIA[0-9A-Z]{16}|BEGIN [A-Z ]*PRIVATE KEY)'
[ $fail = 0 ] && echo "private-lint: OK"
exit $fail
