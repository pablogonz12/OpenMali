#!/usr/bin/env python3 -I
# SPDX-License-Identifier: MIT
# Regenerates the tracked NON-GATING candidate case lists from the pinned CTS mustpass:
#   python3 -I gen-caselists.py <mustpass vk-default dir>      (e.g. third_party/cts-src/external/vulkancts/mustpass/main/vk-default)
# Selection is by group file; "1/N" takes every Nth case of the group in mustpass order (deterministic, same input -> same output).
#   cases-small.txt        groups proposed for promotion to the gated daily (small, cheap, deterministic)
#   cases-deep.txt         sampled larger groups, run on demand / nightly, never a gate
#   cases-texture-all.txt  the whole dEQP-VK.texture.* group (17 070 cases)
#   cases-texture-2k.txt   ~2 000 cases stratified across every texture sub-tree (shadow/swizzle/filtering/compressed/
#                          mipmap/explicit_lod/anisotropy/...), so a slice is not the first 2 000 alphabetically
#   cases-texture-1in8.txt every 8th texture case (2 134): the second, wider slice that extends the 2k one
# Run on a phone with: farm submit --suite cases --env "" $(sed 's/^/--case /' cases-small.txt) (prio 20); lists/daily-sample.txt is untouched.
# The texture lists are run by the shell-user route: python3 -I shell-deqp.py x4 cases-texture-2k.txt <out.tsv>.
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
# One rule per sub-tree, sized to its share: a case in a 6 000-case sub-tree is not worth less than one in a 3-case sub-tree,
# and every sub-tree with fewer cases than its share is taken whole (n = 1).
TEXTURE_2K = [("texture.txt", "dEQP-VK.texture.shadow.", 12), ("texture.txt", "dEQP-VK.texture.swizzle.", 12),
              ("texture.txt", "dEQP-VK.texture.filtering.", 8), ("texture.txt", "dEQP-VK.texture.compressed.", 4),
              ("texture.txt", "dEQP-VK.texture.compressed_3D.", 3), ("texture.txt", "dEQP-VK.texture.mipmap.", 3),
              ("texture.txt", "dEQP-VK.texture.explicit_lod.", 3), ("texture.txt", "dEQP-VK.texture.filtering_anisotropy.", 1),
              ("texture.txt", "dEQP-VK.texture.conversion.", 1), ("texture.txt", "dEQP-VK.texture.texel_buffer.", 1),
              ("texture.txt", "dEQP-VK.texture.multisample.", 1), ("texture.txt", "dEQP-VK.texture.subgroup_lod.", 1)]
for name, rules in (("cases-small.txt", SMALL), ("cases-deep.txt", DEEP),
                    ("cases-texture-all.txt", [("texture.txt", "dEQP-VK.texture.", 1)]),
                    ("cases-texture-1in8.txt", [("texture.txt", "dEQP-VK.texture.", 8)]),
                    ("cases-texture-2k.txt", TEXTURE_2K)):
    out, seen = [], set()
    for f, pre, n in rules:
        for c in pick(load(f), pre, n):
            if c not in seen:
                seen.add(c); out.append(c)
    open(os.path.join(HERE, name), "w").write("\n".join(out) + "\n")
    print(name, len(out))
