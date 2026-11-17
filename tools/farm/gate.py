# SPDX-License-Identifier: MIT
"""Phone health gates (battery + thermal) shared by farmd and the hand-driven quick.py/qsafe path.
Single source: tools/farm/gate.py; a copy is installed as scripts/quick/mgate.py (keep identical). Python 3.11+, stdlib only.

States: on_battery (require_charging devices only: nothing runs unless on a charger), ok -> charging (battery < batt_stop %) -> ok again only at >= batt_resume %
        ok -> cooling  (battery temp >= temp_stop C, or Android thermal status >= thermal_stop) -> ok again only at
        temp <= temp_resume C and status <= thermal_resume (charging has priority). Hysteresis state is a shared file per
        phone, so the farm and agents see the same state. Each state change appends one line to alerts.log.
Defaults (override per device in devices.toml: batt_stop, batt_resume, temp_stop, temp_resume, thermal_stop, thermal_resume):
        battery 20 -> 50 %, 43 C -> 38 C, thermal status SEVERE (3) stop / LIGHT (1) resume (0 NONE 1 LIGHT 2 MODERATE 3 SEVERE ...).
CLI (used by qsafe): python3 -I gate.py check <lockname> <adb-transport> [serial-for-config]  -> exit 0 ok, 3 gated, prints one line."""
import json, os, re, subprocess, sys, time, tomllib

DEFAULTS = {"batt_stop": 20, "batt_resume": 50, "temp_stop": 43.0, "temp_resume": 38.0, "thermal_stop": 3, "thermal_resume": 1,
            "require_charging": False}
HEALTH_CMD = ("dumpsys battery | grep -E '^ *(level|temperature|AC powered|USB powered|Wireless powered|Dock powered):'; "
              "dumpsys thermalservice | grep -E 'Thermal Status:|^Cached temperatures|^Current temperatures from HAL|mName=(BATTERY|SKIN),'")


def state_dir():
    return os.environ.get("MALI_FARM_STATE") or os.path.expanduser("~/.local/state/mali-farm")


def parse(text):
    """-> {'level': %, 'temp_c': C, 'thermal': int} (None for what is missing)
    temp_c = the HIGHEST of `dumpsys battery` temperature and the BATTERY/SKIN sensors under "Current temperatures from HAL".
    The "Cached temperatures" block of dumpsys thermalservice (also what `cmd thermalservice` shows) is ignored: on the Poco X4 it
    stayed frozen (BATTERY 33.1 C for 3+ minutes while the real value moved 35.2 -> 34.1 and another reading said 47 C): stale, never trusted.
    CPU/GPU die sensors are not used (they idle above 43 C by design)."""
    h = {"level": None, "temp_c": None, "thermal": None, "powered": None}
    sect = None
    temps = []
    for l in text.splitlines():
        if l.startswith("Cached temperatures"):
            sect = "cached"
        elif l.startswith("Current temperatures from HAL"):
            sect = "hal"
        m = re.search(r"Temperature\{mValue=(-?[\d.]+),.*mName=(BATTERY|SKIN),", l)
        if m and sect == "hal":
            temps.append(float(m.group(1)))
        m = re.match(r"\s*level:\s*(\d+)", l)
        if m:
            h["level"] = int(m.group(1))
        m = re.match(r"\s*temperature:\s*(-?\d+)", l)
        if m:
            temps.append(int(m.group(1)) / 10)
        m = re.match(r"\s*(AC|USB|Wireless|Dock) powered:\s*(true|false)", l)
        if m:
            h["powered"] = bool(h["powered"]) or m.group(2) == "true"
        m = re.search(r"Thermal Status:\s*(\d+)", l)
        if m:
            h["thermal"] = int(m.group(1))
    h["temp_c"] = max(temps) if temps else None
    return h


def device_address(alias_or_serial):
    """plain-tcpip address ('ip:port') of a phone from devices.toml (matches alias, lock or serial); None if it has none.
    quick.py uses it when the adb transport of a wireless-only phone is not its serial (qrestore failed with 'adb missing' without it)."""
    p = os.environ.get("MALI_FARM_DEVICES") or os.path.expanduser("~/.config/mali-farm/devices.toml")
    try:
        for d in tomllib.load(open(p, "rb")).get("device", []):
            if alias_or_serial in (d.get("alias"), d.get("lock"), d.get("serial")) and d.get("address"):
                return d["address"]
    except (OSError, ValueError):
        pass
    return None


def thresholds(serial=None):
    """defaults + the [[device]] entry of devices.toml whose serial matches (per-device overrides)"""
    th = dict(DEFAULTS)
    p = os.environ.get("MALI_FARM_DEVICES") or os.path.expanduser("~/.config/mali-farm/devices.toml")
    try:
        for d in tomllib.load(open(p, "rb")).get("device", []):
            if serial and d.get("serial") == serial:
                th.update({k: d[k] for k in DEFAULTS if k in d})
                th["_alias"] = d.get("alias")
    except (OSError, ValueError):
        pass
    return th


def next_state(prev, h, th):
    """pure: (state, reason). Unreadable values never gate (the job itself will fail loudly if the phone is gone)."""
    if th.get("require_charging") and h.get("powered") is not True:      # owner's daily phone: only while on a charger
        return "on_battery", "on battery: paused (require_charging)"
    if prev == "on_battery":
        prev = "ok"
    lvl, t, ts = h.get("level"), h.get("temp_c"), h.get("thermal")
    hot = (t is not None and t >= th["temp_stop"]) or (ts is not None and ts >= th["thermal_stop"])
    cool = (t is None or t <= th["temp_resume"]) and (ts is None or ts <= th["thermal_resume"])
    low = lvl is not None and lvl < th["batt_stop"]
    if prev == "charging":
        if lvl is not None and lvl < th["batt_resume"]:
            return "charging", f"battery {lvl}% (resume at {th['batt_resume']}%)"
        prev = "ok"                                        # recovered: re-evaluate thermal below
    if prev == "cooling":
        if not cool:
            return "cooling", f"temp {t}C thermal {ts} (resume at <= {th['temp_resume']}C and status <= {th['thermal_resume']})"
        prev = "ok"
    if low:
        return "charging", f"battery {lvl}% < {th['batt_stop']}% (resume at {th['batt_resume']}%)"
    if hot:
        return "cooling", f"temp {t}C thermal {ts} (stop at >= {th['temp_stop']}C or status >= {th['thermal_stop']})"
    return "ok", ""


def disabled(key, alias=None):
    """owner kill switch: file <state>/disabled/<name> where name is the phone's lock name or its devices.toml alias"""
    d = os.path.join(state_dir(), "disabled")
    names = {key, alias}
    p = os.environ.get("MALI_FARM_DEVICES") or os.path.expanduser("~/.config/mali-farm/devices.toml")
    try:
        for dev in tomllib.load(open(p, "rb")).get("device", []):
            if dev.get("lock", dev.get("alias")) == key or dev.get("alias") == key:
                names |= {dev.get("alias"), dev.get("lock")}
    except (OSError, ValueError):
        pass
    return any(n and os.path.exists(os.path.join(d, n)) for n in names)


def alert(msg):
    d = state_dir()
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "alerts.log"), "a") as f:
        f.write(time.strftime("%Y-%m-%dT%H:%M:%S%z ") + msg + "\n")


def check(key, health, th):
    """key: phone lock name (shared by farm and quick). health: dict from parse()/fake. -> (state, reason). Persists + alerts on change."""
    d = state_dir()
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, f"gate-{key}.json")
    try:
        prev = json.load(open(path)).get("state", "ok")
    except (OSError, ValueError):
        prev = "ok"
    st, why = next_state(prev, health, th)
    if st != prev:
        alert(f"GATE {key} {prev}->{st}: {why or 'recovered'} [batt {health.get('level')}% temp {health.get('temp_c')}C thermal {health.get('thermal')}]")
    json.dump({"state": st, "reason": why, "health": health, "ts": time.time()}, open(path, "w"))
    return st, why


def read_adb(transport, timeout=20):
    try:
        p = subprocess.run(["adb", "-s", transport, "shell", HEALTH_CMD], capture_output=True, timeout=timeout)
        return parse(p.stdout.decode(errors="replace"))
    except (subprocess.TimeoutExpired, OSError):
        return {"level": None, "temp_c": None, "thermal": None, "powered": None}


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "check":
        key, tr = sys.argv[2], sys.argv[3]
        if disabled(key):                                   # before any adb traffic
            print("PHONE DISABLED by owner (farm enable <device> to lift)")
            sys.exit(3)
        h = read_adb(tr)
        st, why = check(key, h, thresholds(sys.argv[4] if len(sys.argv) > 4 else None))
        if st != "ok":
            print(f"PHONE GATED ({st}): {why}")
            sys.exit(3)
        sys.exit(0)
    print(__doc__)
    sys.exit(2)
