# SPDX-License-Identifier: MIT
# Benchmark charts: docs/bench.jsonl (normalised store), docs/BENCH.md (one SVG per workload) and
# docs/bench-gaps.json (coverage matrix). Host-only: no phone is touched, no dependency beyond python3.
#   make bench        import the results files, regenerate the charts and the coverage matrix
#   make bench-check  exit 1 if regenerating would change a committed file (CI gate)
# `make bench --check` is not a make flag; use `make bench-check` (same pattern as make status).
# The generated status page lives on the status-v2 branch; when it merges, these targets sit next to it.
.PHONY: bench bench-check

bench:
	python3 tools/bench/import.py
	python3 tools/bench/charts.py

bench-check:
	python3 tools/bench/import.py --check
	python3 tools/bench/charts.py --check
