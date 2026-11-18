#!/usr/bin/env python3
"""Interleaved GPU A/B on a phone, run INSIDE a hold:
  farm hold poco-x8 --max 20 -- tools/gpuab/gpuab.py --scenes cube,alu --rounds 8 \
      --variant ours=icd:libvulkan_mali.so --variant bridge=bridge --variant noX=icd:libvulkan_mali.so,MALI_FOO=1
Why: the GPU devfreq node is not readable from the adb shell (no root), so the clock cannot be pinned.  Run-to-run
drift (0.45-0.66 ms GPU on cube) is bigger than most single-flag effects, so every round runs all variants back to
back, in a ROTATED order, and we report per-variant median and IQR of the per-round GPU ms, plus the paired
median ratio against the first variant.  Rounds with battery temp > --stop-temp are dropped.
Variant syntax: NAME=bridge | NAME=icd:<local .so>[,ENV=VAL...].  mdbench / reaper default to the repo build outputs."""
import argparse, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gpuablib import rotate, parse, summarize
HOME = os.path.expanduser("~")
ap = argparse.ArgumentParser()
ap.add_argument("--scenes", default="cube,alu,overdraw,tex")
ap.add_argument("--rounds", type=int, default=8)
ap.add_argument("--variant", action="append", required=True)
ap.add_argument("--size", default="1280x720")
ap.add_argument("--seconds", type=int, default=5)
ap.add_argument("--mdbench", default=os.environ.get("GPUAB_MDBENCH", HOME + "/Documents/scripts/mali-open/bench/mdbench/out/mdbench-aarch64"))
ap.add_argument("--reaper", default=os.environ.get("GPUAB_REAPER", HOME + "/Projects/mali-gaming/driver/libkbase/out/reaper-aarch64"))
ap.add_argument("--stop-temp", type=float, default=43.0)
ap.add_argument("--out", default="/tmp/gpuab")
a = ap.parse_args()
serial = os.environ.get("ANDROID_SERIAL") or os.environ.get("ADB_SERIAL")
adb = [HOME + "/Android/Sdk/platform-tools/adb"] + (["-s", serial] if serial else [])
R = HOME + "/Projects/mali-gaming"; DL = "/storage/emulated/0/Download"
env = dict(os.environ, PATH=HOME + "/Android/Sdk/platform-tools:" + os.environ["PATH"], PYTHONPATH=R + "/tools", DROIDDECK_PACKAGE="com.ludashi.benchmark")
os.makedirs(a.out, exist_ok=True)
variants = []
for v in a.variant:
    name, spec = v.split("=", 1)
    if spec.startswith("bridge"):
        variants.append((name, None, spec.partition(":")[2].replace(",", " "))); continue
    kind, rest = spec.split(":", 1); assert kind == "icd"
    parts = rest.split(","); variants.append((name, parts[0], " ".join(parts[1:])))
def sh(c, **k): return subprocess.run(c, capture_output=True, text=True, **k)
sh(adb + ["push", a.mdbench, DL + "/mdbench"]); sh(adb + ["push", a.reaper, DL + "/reaper"])
for i, (n, so, _) in enumerate(variants):
    if so: sh(adb + ["push", so, f"{DL}/gab{i}.so"])
scenes = a.scenes.split(",")
cmd = f"mkdir -p /tmp/mdb; cd /tmp/mdb; cp {DL}/mdbench {DL}/reaper .; chmod +x mdbench reaper; "
for i, (n, so, _) in enumerate(variants):
    if so: cmd += f"cp {DL}/gab{i}.so /tmp/mdb/gab{i}.so; "
cmd += "T() { cat /sys/class/power_supply/battery/temp; }; "
for r in range(a.rounds):
    for sc in scenes:
        order = rotate(variants, r)
        for n, so, e in order:
            i = variants.index((n, so, e))
            icd = f"--icd /tmp/mdb/gab{i}.so" if so else ""
            toks = e.split(); mode = "mailbox"
            for t in list(toks):
                if t.startswith("MODE="): mode = t[5:]; toks.remove(t)   # present mode per variant (mailbox|fifo|immediate)
            e = " ".join(toks)
            tag = f"{n}_{sc}_{r}"
            cmd += (f"echo RUN {n} {sc} {r} temp=$(T); MALI_BACKEND= {e} /tmp/mdb/reaper SteamLaunch AppId=769 -- /tmp/mdb/mdbench --scene {sc} --size {a.size} --windowed {icd} "
                    f"--wsi wayland --mode {mode} --images 3 --frames-in-flight 2 --seconds {a.seconds} --warmup 60 --label {tag} --csv /tmp/mdb/{tag}.csv 2>/dev/null | grep -o '\"fps\":[0-9.]*' | head -1; "
                    f"awk -F, 'NR>70 && $9!=\"nan\" {{n++; g+=$9}} END{{printf \"SUM gpu=%.4f n=%d\\n\", g/n, n}}' /tmp/mdb/{tag}.csv; rm -f /tmp/mdb/{tag}.csv; ")
ctl = ["python3", "-I", R + "/tools/droiddeckctl"]
sh(ctl + ["start", "steam", "--wait", "--reuse"], env=dict(env, ANDROID_SERIAL=serial or ""))
p = sh(ctl + ["guest", "--timeout", str(60 + a.rounds * len(scenes) * len(variants) * (a.seconds + 6)), "--", "sh", "-c", cmd], env=dict(env, ANDROID_SERIAL=serial or ""))
sh(adb + ["shell", "am force-stop com.ludashi.benchmark"])
txt = p.stdout.replace("\\n", "\n").replace('\\"', '"')
open(a.out + "/raw.txt", "w").write(txt)
res = parse(txt, a.stop_temp)
base = variants[0][0]
S = summarize(res, scenes, [v[0] for v in variants])
print(f"{'scene':10}{'variant':10}{'n':>3}{'gpu_ms med':>11}{'IQR':>7}{'fps med':>9}{'paired ratio vs ' + base:>24}")
for sc in scenes:
    for n, _, _ in variants:
        s = S.get((sc, n))
        if not s: print(f"{sc:10}{n:10}  0  (no data)"); continue
        print(f"{sc:10}{n:10}{s['n']:>3}{s['med']:>11.3f}{s['iqr']:>7.3f}{s['fps']:>9.0f}{s['ratio']:>24.3f}")
