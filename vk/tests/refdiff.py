#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare two reference-scene frames (P6 PPM) block by block.

The two frames are the same scene (vk/tests/ref_scene.h) executed by two different Vulkan implementations: our ICD on a
phone and a desktop GPU (RADV) for the reference. Differences are expected wherever the spec allows a driver choice
(filtering precision, blend rounding, sub-pixel rasterisation), so the comparison is tolerance based and reports *where*
a difference is, not just that one exists.

Blocks are 64x64 pixels and the report names the 64-row band, because attachment-store bugs on Mali come in 64-row
granularity (AFBC row blocks): "rows 64-127 wrong" is one line here and one row of the block map.

  tools/refdiff.py phone.ppm ref.ppm                 # human report, exit 1 on mismatch
  tools/refdiff.py phone.ppm ref.ppm --json          # one JSON object (farm/bless)
  tools/refdiff.py phone.ppm ref.ppm --diff d.ppm    # amplified difference image
  tools/refdiff.py --selftest                        # synthetic cases, no GPU, used by vk/check.sh

Tolerances (defaults, see docs/desktop-gpu.md):
  --tol 4            a channel may differ by up to 4/255 before the pixel counts as bad
  --block-bad 0.5    a block fails when more than 0.5% of its pixels are bad (20 of 4096: 1-pixel edges pass)
  --max-bad 0.25     the whole frame fails when more than 0.25% of its pixels are bad
"""

import argparse
import json
import os
import sys

BLOCK = 64


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    fields, i = [], 0
    while len(fields) < 4:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":
            while i < len(data) and data[i : i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(data) and not data[j : j + 1].isspace():
            j += 1
        if j == i:
            raise SystemExit("refdiff: %s: bad PPM header" % path)
        fields.append(data[i:j])
        i = j
    if fields[0] != b"P6":
        raise SystemExit("refdiff: %s: not a P6 (binary RGB) PPM" % path)
    w, h, maxv = int(fields[1]), int(fields[2]), int(fields[3])
    if maxv != 255:
        raise SystemExit("refdiff: %s: maxval %d, only 255 is supported" % (path, maxv))
    while i < len(data) and data[i : i + 1].isspace():
        i += 1
    px = data[i:]
    if len(px) < w * h * 3:
        raise SystemExit("refdiff: %s: %d bytes of pixels, want %d" % (path, len(px), w * h * 3))
    return w, h, px


def write_ppm(path, w, h, px):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(px)


def compare(a, b, w, h, tol):
    """Per-block statistics. A pixel's delta is the max over R,G,B of |a-b| (0..255)."""
    nbx, nby = (w + BLOCK - 1) // BLOCK, (h + BLOCK - 1) // BLOCK
    blocks = [[{"max": 0, "sum": 0, "bad": 0, "n": 0} for _ in range(nbx)] for _ in range(nby)]
    bad_total = 0
    for y in range(h):
        row = blocks[y // BLOCK]
        base = y * w * 3
        for x in range(w):
            o = base + x * 3
            d = a[o] - b[o]
            if d < 0:
                d = -d
            e = a[o + 1] - b[o + 1]
            if e < 0:
                e = -e
            if e > d:
                d = e
            e = a[o + 2] - b[o + 2]
            if e < 0:
                e = -e
            if e > d:
                d = e
            s = row[x // BLOCK]
            if d > s["max"]:
                s["max"] = d
            s["sum"] += d
            s["n"] += 1
            if d > tol:
                s["bad"] += 1
                bad_total += 1
    return blocks, bad_total


def diff_image(a, b, w, h, gain=8):
    out = bytearray(w * h * 3)
    for i in range(0, w * h * 3):
        d = abs(a[i] - b[i]) * gain
        out[i] = 255 if d > 255 else d
    return bytes(out)


def report(name_a, name_b, w, h, blocks, bad_total, args):
    nbx, nby = len(blocks[0]), len(blocks)
    per_block_budget = max(1, int(round(args.block_bad / 100.0 * BLOCK * BLOCK)))
    total_budget = int(round(args.max_bad / 100.0 * w * h))
    worst, failing = None, []
    for by in range(nby):
        for bx in range(nbx):
            s = blocks[by][bx]
            mean = s["sum"] / s["n"] if s["n"] else 0.0
            over = s["bad"] > per_block_budget
            if over:
                failing.append((bx, by, s["max"], mean, s["bad"]))
            if worst is None or s["max"] > worst[3] or (s["max"] == worst[3] and mean > worst[4]):
                worst = (bx, by, w, s["max"], mean)
    ok = not failing and bad_total <= total_budget
    bands = sorted({by for _, by, _, _, _ in failing})
    if args.json:
        print(
            json.dumps(
                {
                    "ok": ok,
                    "a": name_a,
                    "b": name_b,
                    "size": [w, h],
                    "block": BLOCK,
                    "tol": args.tol,
                    "block_bad_pct": args.block_bad,
                    "max_bad_pct": args.max_bad,
                    "bad_pixels": bad_total,
                    "bad_budget_pixels": total_budget,
                    "failing_blocks": [
                        {"block": [bx, by], "rows": [by * BLOCK, min(h, (by + 1) * BLOCK) - 1],
                         "cols": [bx * BLOCK, min(w, (bx + 1) * BLOCK) - 1], "max": m, "mean": round(v, 3),
                         "bad": nb}
                        for bx, by, m, v, nb in failing
                    ],
                    "row_bands": [{"band": b, "rows": [b * BLOCK, min(h, (b + 1) * BLOCK) - 1]} for b in bands],
                    "worst_block": None if worst is None else {"block": [worst[0], worst[1]], "max": worst[3],
                                                              "mean": round(worst[4], 3)},
                },
                sort_keys=True,
            )
        )
        return 0 if ok else 1

    verdict = "ok  " if ok else "FAIL"
    print("refdiff: %s %dx%d, tol %d/255, block budget %d px (>%.2f%%), frame budget %d px (>%.2f%%)"
          % (verdict, w, h, args.tol, per_block_budget, args.block_bad, total_budget, args.max_bad))
    if worst:
        print("  worst block (%d,%d): max %d  mean %.2f" % (worst[0], worst[1], worst[3], worst[4]))
    if bad_total > total_budget:
        print("  %d pixels over tolerance (budget %d)" % (bad_total, total_budget))
    if failing:
        print("  %d of %d blocks over budget:" % (len(failing), nbx * nby))
        for bx, by, m, v, nb in failing:
            print("    block (%d,%d)  rows %3d-%3d  cols %3d-%3d  max %3d  mean %6.2f  bad %5d (%5.1f%%)"
                  % (bx, by, by * BLOCK, min(h, (by + 1) * BLOCK) - 1, bx * BLOCK, min(w, (bx + 1) * BLOCK) - 1,
                     m, v, nb, 100.0 * nb / (BLOCK * BLOCK)))
        print("  row bands with failing blocks: %s" % ", ".join("%d (rows %d-%d)" % (b, b * BLOCK, min(h, (b + 1) * BLOCK) - 1) for b in bands))
    print("  block map (columns = x/64, '.' ok, '-' over budget, '#' mostly wrong):")
    print("    band    rows   %s" % "".join("%-1d" % bx for bx in range(nbx)))
    for by in range(nby):
        cells = []
        for bx in range(nbx):
            s = blocks[by][bx]
            frac = s["bad"] / s["n"] if s["n"] else 0.0
            cells.append("#" if frac > 0.5 else "-" if s["bad"] > per_block_budget else ".")
        print("    %4d %7d   %s" % (by, by * BLOCK, "".join(cells)))
    return 0 if ok else 1


def selftest():
    """Synthetic cases: the harness must pass what it should and fail where it should, and name the right band."""
    w = h = 128
    base = bytearray()
    for y in range(h):
        for x in range(w):
            base += bytes((x * 2 & 255, y * 2 & 255, (x ^ y) & 255))
    base = bytes(base)
    cases = []

    def run(name, other, expect_ok, expect_band=None, **kw):
        blocks, bad = compare(base, other, w, h, kw.get("tol", 4))
        args = argparse.Namespace(tol=kw.get("tol", 4), block_bad=kw.get("block_bad", 0.5),
                                  max_bad=kw.get("max_bad", 0.25), json=True)
        import io
        import contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = report("a", "b", w, h, blocks, bad, args)
        got = json.loads(buf.getvalue())
        ok = (rc == 0) == expect_ok
        bands = [b["band"] for b in got["row_bands"]]
        if expect_band is not None:
            ok = ok and bands == [expect_band]
        print("%s %-32s rc=%d ok=%s bands=%s" % ("ok  " if ok else "FAIL", name, rc, got["ok"], bands))
        return ok

    good = True
    good &= run("identical", base, True)
    good &= run("all channels +1 (tol 4)", bytes(min(255, v + 1) for v in base), True)
    good &= run("all channels +3 (tol 4)", bytes(min(255, v + 3) for v in base), True)
    good &= run("+1 everywhere, tol 0", bytes(min(255, v + 1) for v in base), False, tol=0)
    # a sparse edge: 8 differing pixels in each of two blocks -> under the 0.5% block budget, under the frame budget
    edge = bytearray(base)
    for k in range(8):
        edge[(k * 8 * w + k) * 3] ^= 255
        edge[(k * 8 * w + 65 + k) * 3] ^= 255
    good &= run("sparse 1-px edge", bytes(edge), True)
    # a full 1-px diagonal is 64 pixels in a block: over the 0.5% block budget, so it must be reported
    diag = bytearray(base)
    for y in range(h):
        diag[(y * w + y) * 3] ^= 255
    good &= run("1-px diagonal (64/block)", bytes(diag), False)
    # rows 64..127 shifted by one row = the AFBC-shaped failure: band 1 must be named
    shifted = bytearray(base)
    for y in range(64, 128):
        for x in range(w):
            d = (y * w + x) * 3
            s = ((y - 1) * w + x) * 3
            shifted[d : d + 3] = base[s : s + 3]
    good &= run("rows 64-127 shifted by 1", bytes(shifted), False, expect_band=1)
    # 8 bad pixels scattered -> under every budget
    few = bytearray(base)
    for k in range(8):
        few[k * 3] ^= 255
    good &= run("8 isolated pixels", bytes(few), True)
    # a block of 40 bad pixels = ~1% of one block -> over the 0.5% block budget, under the frame budget
    one = bytearray(base)
    for k in range(40):
        one[(10 * w + 10 + k) * 3] ^= 255
    good &= run("40 px in one block", bytes(one), False, expect_band=0)
    return 0 if good else 1


def main():
    ap = argparse.ArgumentParser(description="compare two reference-scene frames block by block")
    ap.add_argument("a", nargs="?", help="phone frame (P6 PPM)")
    ap.add_argument("b", nargs="?", help="reference frame (P6 PPM)")
    ap.add_argument("--tol", type=int, default=4, help="per-channel tolerance in 0..255 (default 4)")
    ap.add_argument("--block-bad", type=float, default=0.5, help="%% of a block's pixels over tol that fails it")
    ap.add_argument("--max-bad", type=float, default=0.25, help="%% of the frame's pixels over tol that fails it")
    ap.add_argument("--json", action="store_true", help="one JSON object on stdout, no human text")
    ap.add_argument("--diff", metavar="PPM", help="write an amplified |a-b| image")
    ap.add_argument("--selftest", action="store_true", help="run the synthetic cases (no files needed)")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.a or not args.b:
        ap.error("two PPM files are required (or --selftest)")
    wa, ha, pa = read_ppm(args.a)
    wb, hb, pb = read_ppm(args.b)
    if (wa, ha) != (wb, hb):
        print("refdiff: size mismatch: %s is %dx%d, %s is %dx%d" % (args.a, wa, ha, args.b, wb, hb),
              file=sys.stderr)
        return 2
    blocks, bad = compare(pa, pb, wa, ha, args.tol)
    if args.diff:
        write_ppm(args.diff, wa, ha, diff_image(pa, pb, wa, ha))
    return report(args.a, args.b, wa, ha, blocks, bad, args)


if __name__ == "__main__":
    sys.exit(main())
