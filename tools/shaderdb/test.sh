#!/bin/bash
# SPDX-License-Identifier: MIT
# shaderdb harness self-test (needs spirv-as; run after vk/build.sh): tools/shaderdb/test.sh
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
# 1. extract_spv: a carve candidate truncated right after an OpEntryPoint header must not crash
python3 -I - "$D/tools/shaderdb/extract_spv.py" <<'P' || fail "extract_spv truncated OpEntryPoint"
import importlib.util, struct, sys
s = importlib.util.spec_from_file_location("e", sys.argv[1]); m = importlib.util.module_from_spec(s); s.loader.exec_module(m)
d = struct.pack("<5I", 0x07230203, 0x10000, 0, 8, 0) + struct.pack("<I", (1 << 16) | 15)
assert m.carve(d, 0) == (None, None)
P
# 2. harness: fixed-size descriptor array keeps its length; same basename in two dirs stays distinct; space in path
mkdir -p "$T/a b" "$T/c"
cat > "$T/t.spvasm" <<'S'
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" 
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %u DescriptorSet 0
OpDecorate %u Binding 0
OpDecorate %blk Block
OpMemberDecorate %blk 0 Offset 0
OpDecorate %v DescriptorSet 0
OpDecorate %v Binding 1
OpDecorate %blk2 Block
OpMemberDecorate %blk2 0 Offset 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%int = OpTypeInt 32 0
%c4 = OpConstant %int 4
%blk = OpTypeStruct %int
%arr = OpTypeArray %blk %c4
%pa = OpTypePointer Uniform %arr
%u = OpVariable %pa Uniform
%blk2 = OpTypeStruct %int
%p2 = OpTypePointer Uniform %blk2
%v = OpVariable %p2 Uniform
%main = OpFunction %void None %fn
%l = OpLabel
OpReturn
OpFunctionEnd
S
spirv-as "$T/t.spvasm" -o "$T/a b/t.spv" && cp "$T/a b/t.spv" "$T/c/t.spv"
OUT=$T/o.tsv
SHADERDB_NO_DXVK=1 SHADERDB_CACHE=$T/cache MALI_SHADERDB_REPS=1 "$D/tools/shaderdb/run.sh" -n -a 11 -o "$OUT" "$T/a b" "$T/c/t.spv" 2>/dev/null
grep -qF "$T/a b/t.spv" "$OUT" || fail "path with space not compiled/identified"
grep -qF "$T/c/t.spv" "$OUT" || fail "same-basename shader collapsed"
[ "$(cut -f1 "$OUT" | grep -c "/t\.spv$")" = 2 ] || fail "expected 2 distinct t.spv rows"
# 3. a missing corpus path is an error, not a silent skip
if SHADERDB_NO_DXVK=1 SHADERDB_CACHE=$T/cache "$D/tools/shaderdb/run.sh" -n -a 11 -o "$T/x.tsv" "$T/nope.spv" 2>/dev/null; then fail "missing path accepted"; fi
# 4. reflection: entry point not named "main", binding above 31, spec-constant array length (default value), 32-bit set/binding ids
cat > "$T/r.spvasm" <<'S'
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "cs_entry" 
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %blk BufferBlock
OpMemberDecorate %blk 0 Offset 0
OpDecorate %hi DescriptorSet 0
OpDecorate %hi Binding 40
OpDecorate %arr_v DescriptorSet 0
OpDecorate %arr_v Binding 300
OpDecorate %len SpecId 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%int = OpTypeInt 32 0
%c0 = OpConstant %int 0
%c3 = OpConstant %int 3
%c7 = OpConstant %int 7
%len = OpSpecConstant %int 4
%blk = OpTypeStruct %int
%pb = OpTypePointer Uniform %blk
%pi = OpTypePointer Uniform %int
%arr = OpTypeArray %blk %len
%parr = OpTypePointer Uniform %arr
%hi = OpVariable %pb Uniform
%arr_v = OpVariable %parr Uniform
%main = OpFunction %void None %fn
%l = OpLabel
%p1 = OpAccessChain %pi %hi %c0
OpStore %p1 %c7
%p2 = OpAccessChain %pi %arr_v %c3 %c0
OpStore %p2 %c7
OpReturn
OpFunctionEnd
S
if spirv-as --target-env spv1.3 "$T/r.spvasm" -o "$T/r.spv" 2>"$T/r.err"; then
   SHADERDB_NO_DXVK=1 SHADERDB_CACHE=$T/cache MALI_SHADERDB_REPS=1 "$D/tools/shaderdb/run.sh" -n -a 11 -o "$T/r.tsv" "$T/r.spv" 2>/dev/null
   grep -F "/r.spv" "$T/r.tsv" | grep -q "	OK	" || { cat "$T/r.tsv"; fail "reflection shader (entry cs_entry, binding 40/300, spec-length array) did not compile"; }
else cat "$T/r.err"; fail "spirv-as r.spvasm"; fi
# 5. the corpus travels as a list file (no argv limit): 20000 paths in a -l list parse without E2BIG and give one header
for i in $(seq 1 20000); do printf '%s\0' "$T/c/t.spv"; done > "$T/big.list"
"$D/vk/out/native/shaderdb" -a 11 -l "$T/big.list" 2>/dev/null | awk 'END{ if (NR != 20001) exit 1 }' || fail "-l list of 20000 paths"
echo "shaderdb test OK"
