# SPDX-License-Identifier: MIT
"""Route choice of the smoke suite on a guest-route phone (devices.toml `smoke_fallback = "shell"`). Pure helpers, no phone access.

The guest route (valrun.sh -> qdrv -> t_vk_* in the MediaDeck app's Linux guest, glibc build) needs a READY app session. With the
screen off or the keyguard showing the session stays SUSPENDED. The fallback runs the same t_vk_* binaries as the bionic build under the
adb shell user (tools/farm/shellroute.py smoke), with this job's own driver build, and says so in the job log.
dEQP (qdeqp.py) runs in the same guest session, so it has no such route: it does NOT run on a locked guest-route phone."""
import re

# tests of the guest smoke that need a live session / display; the shell route cannot run them (none is part of the smoke binaries today:
# t_vk_draw/compute/meta/feat render offscreen), listed so the job log says so instead of implying coverage
SKIPPED = ("present/swapchain (needs the session's display)", "gamescope (needs the session)")


def blocked_reason(text):
    """text = `dumpsys power | grep mWakefulness` + keyguard flag of `dumpsys window` -> why the session cannot be READY ('' = no reason known)"""
    m = re.search(r"mWakefulness=(\w+)", text or "")
    if m and m.group(1) != "Awake":
        return f"screen {m.group(1)} (wakefulness)"
    if re.search(r"(mShowingLockscreen|isKeyguardShowing)=true", text or ""):
        return "keyguard showing"
    return ""


def guest_unusable(rc, out):
    """valrun's rc 74 (qdrv wrote no result for THIS run) or the qdrv failure line: the guest route did not run the tests"""
    return rc == 74 or "session not READY" in (out or "") or "NO RESULT" in (out or "")


def header(route, serial, sha, reason="", skipped=SKIPPED):
    """first line of the kept job log: which route produced the results, on which phone, with which driver build"""
    s = f"SMOKE-ROUTE route={route} serial={serial} so_sha256={sha}"
    if reason:
        s += f" reason={reason}"
    if route == "shell-fallback":
        s += " skipped=" + ";".join(skipped)
    return s
