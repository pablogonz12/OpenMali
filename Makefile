# SPDX-License-Identifier: MIT
# Generated status page for the mali-open Vulkan driver. One page: docs/STATUS.md (per-item tables,
# evidence, what is left, history) plus the summary table spliced into README.md.
#   make status        build the ICD, run the offline gate, regenerate docs/STATUS.md, docs/status.json,
#                      docs/status-history.jsonl and the README summary
#   make status-check  exit 1 if regenerating would change a committed file (CI gate; needs the same build
#                      and gate log, so it repeats them)
# Never edit docs/STATUS.md or the README status block by hand: they are generated from this tree.
.PHONY: status status-check

status:
	./vk/build.sh
	@mkdir -p vk/out/native
	-./vk/check.sh > vk/out/native/check.log 2>&1
	python3 tools/status/status.py

status-check:
	./vk/build.sh
	@mkdir -p vk/out/native
	-./vk/check.sh > vk/out/native/check.log 2>&1
	python3 tools/status/status.py --check
