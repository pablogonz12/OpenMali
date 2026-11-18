#!/usr/bin/env python3 -I
# SPDX-License-Identifier: MIT
# Regenerates the tracked NON-GATING candidate case lists from the pinned CTS mustpass:
#   python3 -I gen-caselists.py <mustpass vk-default dir>      (e.g. third_party/cts-src/external/vulkancts/mustpass/main/vk-default)
# Selection is by group file; "1/N" takes every Nth case of the group in mustpass order (deterministic, same input -> same output).
#   cases-small.txt  groups proposed for promotion to the gated daily (small, cheap, deterministic)
#   cases-deep.txt   sampled larger groups, run on demand / nightly, never a gate
# Run on a phone with: farm submit --suite cases --env "" $(sed 's/^/--case /' cases-small.txt) (prio 20); lists/daily-sample.txt is untouched.
import os, sys
M = sys.argv[1]
HERE = os.path.dirname(os.path.abspath(__file__))
def load(f): return [l.strip() for l in open(os.path.join(M, f)) if l.strip()]
def pick(src, prefix, n):
    g = [c for c in src if c.startswith(prefix)]
    return g[::n]
SMALL = [("draw.txt", "dEQP-VK.draw.renderpass.", 130), ("draw.txt", "dEQP-VK.draw.dynamic_rendering.", 60),
         ("dynamic-state.txt", "", 1), ("clipping.txt", "", 1), ("fragment-operations.txt", "", 1), ("depth.txt", "", 1)]
DEEP = [("rasterization.txt", "", 20), ("compute.txt", "", 40), ("ssbo.txt", "", 20), ("ubo.txt", "", 20), ("texture.txt", "", 20),
        ("query-pool.txt", "", 20), ("memory.txt", "", 20), ("conditional-rendering.txt", "", 1), ("multiview.txt", "", 1),
        ("descriptor-indexing.txt", "", 1), ("binding-model.txt", "", 200)]
for name, rules in (("cases-small.txt", SMALL), ("cases-deep.txt", DEEP)):
    out, seen = [], set()
    for f, pre, n in rules:
        for c in pick(load(f), pre, n):
            if c not in seen:
                seen.add(c); out.append(c)
    open(os.path.join(HERE, name), "w").write("\n".join(out) + "\n")
    print(name, len(out))
