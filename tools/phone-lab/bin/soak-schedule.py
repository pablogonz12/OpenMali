#!/usr/bin/env python3
"""Monotonic, absolute-deadline event planning for the supervised soak."""
import time


def events(minutes):
    """Yield (seconds, action, index); no sleeping and no phone operations.

    Right is sent on every input tick, plus down on fourth and b on eighth.
    A missed deadline remains observable rather than silently shifting later
    deadlines or dropping an input.
    """
    if not isinstance(minutes, int) or isinstance(minutes, bool) or minutes < 1:
        raise ValueError("minutes must be a positive integer")
    end = minutes * 60
    plan = []
    for timestamp, label in ((0, "start"), (end / 2, "mid"), (end, "end")):
        plan.append((timestamp, 3, "screenshot", label))
    for seconds in range(0, end + 1, 60):
        plan.append((seconds, 4, "sample", seconds // 60))
    for index, seconds in enumerate(range(30, end + 1, 30), 1):
        plan.append((seconds, 0, "right", index))
        if index % 4 == 0:
            plan.append((seconds, 1, "down", index))
        if index % 8 == 0:
            plan.append((seconds, 2, "b", index))
    for timestamp, _, action, index in sorted(plan):
        yield timestamp, action, index


def run(minutes, callback, clock=time.monotonic, sleep=time.sleep):
    """Invoke callback(deadline offset, actual offset, action, index).

    Monotonic origin is captured once. A slow callback may make subsequent
    events late, but it cannot move their deadlines. Callback exceptions are
    propagated to the caller, which must run phone teardown in a finally block.
    """
    start = clock()
    for due, action, index in events(minutes):
        delay = start + due - clock()
        if delay > 0:
            sleep(delay)
        callback(due, clock() - start, action, index)
