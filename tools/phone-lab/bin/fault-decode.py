#!/usr/bin/env python3
"""Decode the first Mali fatal fault in a collected log tree."""
import json
import os
import re
import sys

PATTERN = re.compile(
    r"mali_vk: group \d+(?: queue \d+)? FATAL:\s*(\w+)"
    r"\s*\(status (0x[0-9a-f]+), sideband (0x[0-9a-f]+)\)",
    re.IGNORECASE,
)


def decode(line):
    match = PATTERN.search(line)
    if not match:
        return None
    kind, raw, va = match.groups()
    status = int(raw, 16)
    return {"type": kind, "access": ("ATOMIC", "EXECUTE", "READ", "WRITE")[
        (status >> 8) & 3], "source_id": (status >> 16) & 0xffff, "va": va}


def scan(root):
    for parent, dirs, files in os.walk(root):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(parent, name)
            try:
                with open(path, encoding="utf-8", errors="replace") as stream:
                    for line in stream:
                        fault = decode(line)
                        if fault:
                            return fault
            except OSError:
                continue
    return None


if __name__ == "__main__":
    print(json.dumps(scan(sys.argv[1])))
