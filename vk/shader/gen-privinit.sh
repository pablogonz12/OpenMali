#!/bin/bash
# SPDX-License-Identifier: MIT
# privinit.comp.spv: glslang emits Private globals without initializers (it stores in main), but DXVK/dxc emit OpVariable ... Private <const>.
# Assemble the real thing: move the two variable declarations behind the constants and give them initializers.
set -euo pipefail
cd "$(dirname "$0")"
glslangValidator -V --target-env vulkan1.1 privinit.comp -o /tmp/privinit.glsl.spv >/dev/null
spirv-dis /tmp/privinit.glsl.spv | python3 -c '
import sys
L=sys.stdin.read().split("\n")
v=[l for l in L if "OpVariable %_ptr_Private" in l]
L=[l for l in L if "OpVariable %_ptr_Private" not in l and not l.strip().startswith(("OpStore %g_base %uint_5","OpStore %g_tab"))]
i=max(n for n,l in enumerate(L) if "OpConstantComposite %_arr_uint_uint_4" in l)
v=[v[0].rstrip()+" %uint_5", v[1].rstrip()+" %"+L[i].split("=")[0].strip().lstrip("%")]
print("\n".join(L[:i+1]+v+L[i+1:]))' | spirv-as --target-env spv1.3 - -o privinit.comp.spv
spirv-val --target-env vulkan1.1 privinit.comp.spv
