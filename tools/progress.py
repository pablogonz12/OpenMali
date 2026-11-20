#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Deprecated: the checklist and the dashboard are one page now.

This used to write docs/progress.svg and docs/progress.md from docs/progress.toml. Those files were a
second, competing status page, so they are gone: tools/status/status.py reads the same
docs/progress.toml and writes docs/STATUS.md (per-item tables, evidence, history) plus the summary
table in README.md. This wrapper stays so `tools/progress.py [--check]` (and vk/check.sh) keep working.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.exit(subprocess.call([sys.executable, str(ROOT / "tools/status/status.py"), *sys.argv[1:]]))
