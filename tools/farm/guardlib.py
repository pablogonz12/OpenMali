# SPDX-License-Identifier: MIT
"""runaway-app guard: an idle phone (no job, no hold) whose foreground app burns CPU for a long time is force-stopped.
Origin: MediaDeck sat 5 h at 128% CPU on the X4 and held it at 52C. Pure helpers; farmd does the adb I/O (`dumpsys cpuinfo`, `am force-stop`)."""
import re

import menulib as M

CPU_DEFAULT = 90.0        # % of one core (dumpsys cpuinfo scale: 128% is possible)
SECS_DEFAULT = 900        # sustained this long before the kill
SAFE_PKGS = ("com.android.systemui", "android", "com.android.settings")


def cpu_of(cpuinfo_out, pkg):
    """highest '%' for a process of pkg in `dumpsys cpuinfo` lines like '  128% 1234/com.x: 100% user + 28% kernel' -> float | None"""
    best = None
    for m in re.finditer(r"([\d.]+)%\s+\d+/([\w.]+)(?::[\w.]+)?:", cpuinfo_out or ""):
        if m.group(2) == pkg:
            best = max(best or 0.0, float(m.group(1)))
    return best


def candidate(activity_out):
    """foreground package that may be stopped, or None (unreadable, launcher and system UI are never touched)"""
    fg = M.foreground_pkg(activity_out)
    if not fg or M.is_launcher(fg) or fg in SAFE_PKGS:
        return None
    return fg


def verdict(hot, pkg, cpu, now, cpu_max, secs):
    """hot: dict pkg -> first time seen above the limit (mutated). -> seconds above the limit when it must be stopped, else None"""
    if pkg is None or cpu is None or cpu < cpu_max:
        hot.clear()
        return None
    for k in [k for k in hot if k != pkg]:
        del hot[k]
    t0 = hot.setdefault(pkg, now)
    return now - t0 if now - t0 >= secs else None
