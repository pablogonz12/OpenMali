# lib.sh — shared by every tool in M/bin. Source it, never run it.
M=${M:-${MALI_LAB_ROOT:-$HOME/.local/share/mali-phone-lab}}
ADB=${ADB:-$HOME/Android/Sdk/platform-tools/adb}; export ADB   # droiddeckctl reads $ADB; workers have no adb on PATH
CTL=${CTL:-${MALI_DROIDDECKCTL:-droiddeckctl}}
REPO=${REPO:-${MALI_REPO:-$HOME/src/OpenMali}}
DEPLOY=${DEPLOY:-${MALI_DEPLOY_DIR:-$HOME/.local/share/mali-deploy}}     # one directory per artefact sha (bin/stash-artefact)
LANES=${LANES:-${MALI_LANES_DIR:-$HOME/.local/share/mali-lanes}}    # lane worktrees live on disk, never on the /tmp tmpfs
CACHE=${CACHE:-$HOME/.cache/mali-orchestrator}            # screenshots, smoke runs, logs
die() { echo "ERROR: $1" >&2; exit "${2:-2}"; }
phone_lock() {
  local n=$1 f owner cmd
  [[ "$n" =~ ^[a-zA-Z0-9_-]+$ ]] || { echo "REFUSED: invalid phone name" >&2; return 4; }
  mkdir -p "$M/locks" || return 4
  f="$M/locks/$n.lock"
  exec {MALI_LOCK_FD}>>"$f" || return 4
  if ! flock -n "$MALI_LOCK_FD"; then
    owner=$(cat "$f" 2>/dev/null)
    PHONE_LOCK_REASON="PHONE_LOCKED by ${owner:-unknown}"
    echo "REFUSED: $PHONE_LOCK_REASON" >&2
    return 4
  fi
  cmd=$(tr '\0' ' ' < "/proc/$$/cmdline" 2>/dev/null)
  printf '%s %s since %s\n' "$$" "$cmd" "$(date -Is)" > "$f"
  MALI_LOCK_HELD=$n
  export MALI_LOCK_HELD
}
phones() { grep -v -e '^#' -e '^$' "$M/phones.conf" | cut -d'|' -f1 | tr '\n' ' '; }
# farm_state -> "desktop=inactive laptop=inactive". The farm unit exists on every host that shares ~/.config (Syncthing);
# the laptop's ran unnoticed 2026-11-19..21 and panicked the CMF. FARM_HOSTS: ssh aliases of the other hosts.
FARM_HOSTS=${FARM_HOSTS-}   # the OTHER hosts; the fleet host is the one named in M/HOST
farm_state() {
  local h out="$(hostname | tr A-Z a-z | sed s/^arch-//)=$(systemctl --user is-active mali-farm.service 2>/dev/null)"
  for h in $FARM_HOSTS; do
    out="$out $h=$(timeout 8 ssh -o BatchMode=yes -o ConnectTimeout=5 "$h" systemctl --user is-active mali-farm.service 2>/dev/null || true)"
    case $out in *=) out="${out}unreachable";; esac
  done
  echo "$out"
}
# mdns_adb "<candidate serials>" -> prints ip:port of a wireless-debugging endpoint whose mDNS name carries one of
# the hardware serials (adb-<serial>-xxxx _adb-tls-connect), after connecting to it. The port changes every time
# wireless debugging is switched on, so it is never stored in phones.conf. Needs an earlier pairing with this host.
mdns_adb() {
  local c ep
  for c in $1; do
    case $c in *:*) continue;; esac
    # 1) an endpoint adb is ALREADY connected to whose ro.serialno is this phone (mDNS adverts come and go; a live
    #    connection does not need one — on 2026-11-21 the CMF was "device" in `adb devices` and still reported OFFLINE)
    for ep in $(timeout 8 "$ADB" devices 2>/dev/null | awk '$2=="device" && $1 ~ /:/ {print $1}'); do
      [ "$(timeout 6 "$ADB" -s "$ep" shell getprop ro.serialno 2>/dev/null | tr -d '\r\n')" = "$c" ] && { echo "$ep"; return 0; }
    done
    # 2) mDNS wireless debugging
    ep=$(timeout 10 "$ADB" mdns services 2>/dev/null | awk -v s="adb-$c-" 'index($1,s)==1 && $2=="_adb-tls-connect._tcp"{print $3; exit}')
    [ -n "$ep" ] || continue
    timeout 10 "$ADB" connect "$ep" >/dev/null 2>&1
    [ "$(timeout 6 "$ADB" -s "$ep" get-state 2>/dev/null)" = device ] && { echo "$ep"; return 0; }
  done
  return 1
}
# phone <name> -> sets P_NAME P_SERIAL P_PKG P_HZ; returns 2 unknown, 3 offline
phone() {
  local want=${1:-} n ss p h c
  while IFS='|' read -r n ss p h; do
    [ "$n" = "$want" ] || continue
    for c in $ss; do
      if [ "$(timeout 6 "$ADB" -s "$c" get-state 2>/dev/null)" = device ]; then
        P_NAME=$n; P_SERIAL=$c; P_PKG=$p; P_HZ=$h; return 0
      fi
    done
    c=$(mdns_adb "$ss") && { P_NAME=$n; P_SERIAL=$c; P_PKG=$p; P_HZ=$h; return 0; }
    echo "ERROR: phone '$want' is OFFLINE (tried: $ss, and mDNS wireless debugging). Owner: plug in USB, or Settings > Developer options > Wireless debugging ON (same Wi-Fi)" >&2; return 3
  done < <(grep -v -e '^#' -e '^$' "$M/phones.conf")
  echo "ERROR: unknown phone '$want' (known: $(phones))" >&2; return 2
}
ctl()  { DROIDDECK_PACKAGE=$P_PKG ADB_SERIAL=$P_SERIAL ANDROID_SERIAL=$P_SERIAL "$CTL" --serial "$P_SERIAL" "$@"; }
adbs() { "$ADB" -s "$P_SERIAL" "$@"; }
fg_pkg() { adbs shell 'dumpsys activity activities 2>/dev/null | grep -m1 topResumedActivity' 2>/dev/null | sed -E 's/.* ([a-zA-Z0-9_.]+)\/.*/\1/' | tr -d '\r'; }
awake()  { adbs shell 'dumpsys power 2>/dev/null | grep -m1 mWakefulness=' 2>/dev/null | grep -q Awake; }
# owner_busy: the screen is on and the foreground app is neither DroidDeck nor a home launcher
owner_busy() {
  local fg; fg=$(fg_pkg)
  [ "$fg" = "$P_PKG" ] && return 1
  case "$fg" in *launcher*|"") return 1;; esac
  awake || return 1
  echo "$fg"; return 0
}
# jget <dotted.key>   reads droiddeckctl output on stdin (skips the "Resolved ADB serial" line), prints JSON scalars bare
jget() { python3 -c '
import sys, json
t = sys.stdin.read(); i = t.find("{")
try: d = json.loads(t[i:]) if i >= 0 else None
except Exception: d = None
for k in sys.argv[1].split("."):
    d = d.get(k) if isinstance(d, dict) else None
print("" if d is None else (d if isinstance(d, str) else json.dumps(d)))' "$1"; }
bat_temp()  { adbs shell 'dumpsys battery' 2>/dev/null | awk '/temperature/{print $2/10; exit}'; }
bat_level() { adbs shell 'dumpsys battery' 2>/dev/null | awk '/^ *level:/{print $2; exit}' | tr -d '\r'; }
# tctl <seconds> <droiddeckctl args…>   ctl with a hard time limit (ctl is a shell function, so `timeout ctl` cannot work)
tctl() { local s=$1; shift; timeout "$s" env DROIDDECK_PACKAGE=$P_PKG ADB_SERIAL=$P_SERIAL ANDROID_SERIAL=$P_SERIAL "$CTL" --serial "$P_SERIAL" "$@"; }
