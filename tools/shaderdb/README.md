# shaderdb

Offline compiler regression/optimisation harness (like Mesa's shader-db). Compiles a SPIR-V corpus for v11 and v13 through `mali_compile()` and writes a TSV: `instr` (bin bytes / 8), `regs` (work registers), `spills` (TLS bytes), `wls`, `fau`, `imm`, `bytes`, `time_us` (median of 3).

```
mali-slot -- tools/shaderdb/run.sh -o /tmp/before.tsv     # builds vk/out/native/shaderdb, compiles the corpus
MALI_OPT_GCM=1 mali-slot -- tools/shaderdb/run.sh -n -o /tmp/after.tsv
tools/shaderdb/compare.py /tmp/before.tsv /tmp/after.tsv [-v]   # helped / hurt / totals per metric
```

Corpus: `vk/shader/*.spv`, `bench/mdbench/shaders`, `compiler-spike`, `$SHADERDB_EXTRA` (colon-separated dirs/files, e.g. deqp output), plus SPIR-V carved from a local Proton DXVK install by `extract_spv.py` (cached in `$SHADERDB_CACHE`, default `$TMPDIR/shaderdb-corpus`; set `SHADERDB_NO_DXVK=1` to skip). Descriptor layouts are reflected from the SPIR-V; each compile runs in a child process so a compiler assert shows up as an `ERR` row. `SHADERDB_V=1` prints the error text. Output is deterministic except `time_us`.
