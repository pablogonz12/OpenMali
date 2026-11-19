#!/usr/bin/env python3
"""compare.py OLD.tsv NEW.tsv [-v] [--time-threshold PCT]
Diff two shaderdb TSVs. Per metric (instr, regs, spills, wls) a shader is `helped` if the value dropped, `hurt` if it rose.
Totals are over shaders that compiled OK in both. Compile time is reported as total and median change (noisy: informational only)."""
import csv, sys, statistics, argparse

def load(p):
    d = {}
    with open(p) as f:
        for r in csv.DictReader(f, delimiter="\t"):
            d[(r["file"], r["arch"])] = r
    return d

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("old"); ap.add_argument("new")
    ap.add_argument("-v", action="store_true", help="list every changed shader")
    a = ap.parse_args()
    o, n = load(a.old), load(a.new)
    keys = sorted(set(o) & set(n))
    ok = [k for k in keys if o[k]["status"] == "OK" and n[k]["status"] == "OK"]
    broke = [k for k in keys if o[k]["status"] == "OK" and n[k]["status"] != "OK"]
    fixed = [k for k in keys if o[k]["status"] != "OK" and n[k]["status"] == "OK"]
    print(f"shaders: {len(keys)} common, {len(ok)} OK in both, {len(set(o) ^ set(n))} only in one file")
    if broke: print("NOW FAILING:", ", ".join(f"{k[0]}/v{k[1]}" for k in broke))
    if fixed: print("NOW COMPILING:", ", ".join(f"{k[0]}/v{k[1]}" for k in fixed))
    for m in ("instr", "regs", "spills", "wls", "bytes"):
        helped = hurt = same = 0; to = tn = 0
        for k in ok:
            x, y = int(o[k][m]), int(n[k][m]); to += x; tn += y
            if y < x: helped += 1
            elif y > x: hurt += 1
            else: same += 1
        pct = (tn - to) / to * 100 if to else 0.0
        print(f"{m:7s} total {to} -> {tn} ({pct:+.2f}%)  helped {helped}  hurt {hurt}  unchanged {same}")
        if a.v:
            for k in ok:
                x, y = int(o[k][m]), int(n[k][m])
                if x != y: print(f"    {k[0]}/v{k[1]}: {x} -> {y}")
    if ok:
        to = [float(o[k]["time_us"]) for k in ok]; tn = [float(n[k]["time_us"]) for k in ok]
        ratios = [y / x for x, y in zip(to, tn) if x > 0]
        tot = f"{(sum(tn)/sum(to)-1)*100:+.1f}%" if sum(to) > 0 else "n/a"
        med = f"{statistics.median(ratios):.3f}" if ratios else "n/a"
        print(f"time_us total {sum(to):.0f} -> {sum(tn):.0f} ({tot}), median per-shader ratio {med} (noisy)")

main()
