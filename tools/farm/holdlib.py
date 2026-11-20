# SPDX-License-Identifier: MIT
"""farm hold: a server-side FIFO queue of exclusive phone holds for agents, ordered with farm jobs by priority.
Files in <state>/holds/: <id>.json (written by farmd only: state queued|active|done), <id>.hb (the caller touches it: heartbeat),
<id>.rel (the caller creates it to release / cancel). farmd takes the same flock (/tmp/qsafe-<lock>.lock) as every qsafe script
for the whole hold, so old scripts stay safe. Pure helpers here; farmd and `farm` do the I/O around them."""
import json, os, time

import farmlib as L

HB_STALE = float(os.environ.get("MALI_FARM_HB_STALE", 45))   # s without a heartbeat -> the caller is dead, the hold is released
HB_EVERY = float(os.environ.get("MALI_FARM_HB_EVERY", 10))
DEFAULT_PRIO = 0          # same as gates: an agent that blocks on a hold is waiting on the critical path
BAD_PROCS = ("steam", "steamwebhelper", "gamescope-wl")


def holds_dir():
    d = os.path.join(L.state_dir(), "holds")
    os.makedirs(d, exist_ok=True)
    return d


def path(hid, ext="json"):
    return os.path.join(holds_dir(), f"{hid}.{ext}")


def new_hold(alias, max_s, prio=DEFAULT_PRIO, lease=False, who=""):
    hid = time.strftime("h%Y%m%d-%H%M%S") + f"-{os.getpid()}-{time.time_ns() % 10**9:09d}"
    h = {"id": hid, "device": alias, "prio": int(prio), "max_s": int(max_s), "lease": bool(lease), "who": who, "state": "queued",
         "created": L.now(), "seq": time.time_ns(), "auto": 0}
    touch(hid, "hb")                                  # first the heartbeat: farmd must never see a queued hold without one and cancel a live caller
    try:
        L.write_json(path(hid), h)
    except BaseException:
        os.unlink(path(hid, "hb"))
        raise
    return h


def touch(hid, ext):
    with open(path(hid, ext), "a"):
        pass
    os.utime(path(hid, ext))


def age(hid, ext="hb", now=None):
    """seconds since the file was touched; huge when missing"""
    try:
        return (time.time() if now is None else now) - os.path.getmtime(path(hid, ext))
    except OSError:
        return 1e9


def released(hid):
    return os.path.exists(path(hid, "rel"))


def list_holds():
    out = []
    for n in sorted(os.listdir(holds_dir())):
        if n.endswith(".json"):
            try:
                out.append(L.read_json(os.path.join(holds_dir(), n)))
            except (ValueError, OSError):
                pass
    return sorted(out, key=lambda h: h.get("seq", 0))


def key(x):
    """the scheduling key shared by jobs and holds: priority, explicit before auto, submission order (seq is time_ns for both)"""
    return (L.job_prio(x), 1 if x.get("auto") else 0, x.get("seq", 0))


def queued(holds, alias):
    return sorted((h for h in holds if h["state"] == "queued" and h["device"] == alias), key=key)


def end_reason(h, hb_age, rel, now=None, disabled=False):
    """why an active hold must end now ('' = keep going)"""
    now = time.time() if now is None else now
    if rel:
        return "released"
    if disabled:
        return "disabled by owner"
    if now > h.get("deadline", now + 1):
        return "max time reached"
    if hb_age > HB_STALE:
        return "caller lost (no heartbeat)"
    return ""


def gc(keep_s=86400, orphan_s=600):
    """drop finished hold files older than a day, and orphans: .hb/.rel with no .json (a crashed new_hold or a gc cut short) and
    leftover .tmp-* files of an interrupted write_json, once older than orphan_s (new_hold writes the .hb first: leave it a grace period)"""
    now = time.time()
    for h in list_holds():
        if h["state"] == "done" and now - os.path.getmtime(path(h["id"])) > keep_s:
            for e in ("json", "hb", "rel"):
                try:
                    os.unlink(path(h["id"], e))
                except OSError:
                    pass
    d = holds_dir()
    for n in os.listdir(d):
        p = os.path.join(d, n)
        orphan = n.startswith(".tmp-") or (n.rsplit(".", 1)[-1] in ("hb", "rel") and not os.path.exists(p.rsplit(".", 1)[0] + ".json"))
        try:
            if orphan and now - os.path.getmtime(p) > orphan_s:
                os.unlink(p)
        except OSError:
            pass


# ---- restore-pending marks: a phone whose hold/job was cut short by a farmd restart must be restored before its next use; the mark is a
# file so it survives further restarts (an in-memory set was lost by the very restart it was meant to cover)
def restore_dir():
    d = os.path.join(L.state_dir(), "restore-pending")
    os.makedirs(d, exist_ok=True)
    return d


def mark_restore(alias):
    with open(os.path.join(restore_dir(), alias), "a"):
        pass


def clear_restore(alias):
    try:
        os.unlink(os.path.join(restore_dir(), alias))
    except OSError:
        pass


def load_restore():
    return set(os.listdir(restore_dir()))
