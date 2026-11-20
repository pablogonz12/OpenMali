#!/usr/bin/env python3
"""Carve embedded SPIR-V modules out of binaries (e.g. DXVK d3d9/d3d11/dxgi DLLs).
usage: extract_spv.py OUTDIR PREFIX FILE...   Modules are validated with spirv-val; stage goes into the file name."""
import struct, subprocess, sys, os, hashlib
MAGIC = 0x07230203
STAGE = {0: "vert", 4: "frag", 5: "comp"}
def carve(data, off):
    n = len(data) // 4
    i = off // 4 + 5
    last_end = None
    entry = None
    while i < n:
        w = struct.unpack_from("<I", data, i * 4)[0]
        wc, op = w >> 16, w & 0xFFFF
        if wc == 0 or op > 5000 or i + wc > n:
            break
        if op == 15 and entry is None:  # OpEntryPoint
            if wc < 4:
                return None, None  # truncated candidate
            entry = struct.unpack_from("<I", data, (i + 1) * 4)[0]
        i += wc
        if op == 56:  # OpFunctionEnd
            last_end = i
    if last_end is None:
        return None, None
    return data[off:last_end * 4], entry
def main():
    out, prefix, files = sys.argv[1], sys.argv[2], sys.argv[3:]
    os.makedirs(out, exist_ok=True)
    seen = set()
    for f in files:
        data = open(f, "rb").read()
        pos = 0
        while True:
            pos = data.find(struct.pack("<I", MAGIC), pos)
            if pos < 0: break
            if pos % 4 == 0:
                mod, em = carve(data, pos)
                if mod and em in STAGE:
                    h = hashlib.sha1(mod).hexdigest()[:10]
                    if h not in seen:
                        p = f"{out}/{prefix}_{h}.{STAGE[em]}.spv"
                        open(p, "wb").write(mod)
                        r = subprocess.run(["spirv-val", "--target-env", "vulkan1.3", p], capture_output=True)
                        if r.returncode: os.remove(p)
                        else: seen.add(h)
            pos += 4
    print(len(seen), "modules")
if __name__ == "__main__":
    main()
