# Compiler spike: SPIR-V -> NIR -> Mali Valhall binary, offline (2026-11-15)
Mesa mesa-26.3.0 @ 0a6fa60abb73 (third_party/MESA-SOURCE.txt). Works: v11 (G615) and v13 (G725/G925), VS + FS.

## Build (native x86-64, no Vulkan driver, no Docker; llvm+libclc needed only for mesa-clc)
    export PATH=third_party/venv/bin:$PATH          # meson, ninja, mako, pyyaml
    meson setup build mesa -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dtools=panfrost -Dglx=disabled -Dplatforms= \
      -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dllvm=enabled \
      -Dvalgrind=disabled -Dlibunwind=disabled -Dbuildtype=release -Dpanfrost-rust=false -Dmesa-clc=enabled
    ninja -C build src/panfrost/clc/panfrost_compile src/panfrost/bifrost_compiler \
      src/panfrost/compiler/libpanfrost_compiler.a src/compiler/spirv/libvtn.a
Configure 18 s; build 681 steps = 9 min 1 s on 4 cores. System tools all present (bison flex glslang spirv-tools llvm 23).
`-Dllvm=disabled` fails (precomp compiler "CLC requires LLVM"); no patches to Mesa were needed.
Reproduce spike: `compiler-spike/build.sh` (re-uses panfrost_compile's compile+link lines), then
`./spike <11|13> <vert|frag> x.spv out.bin`; `BIFROST_MESA_DEBUG=shaders` dumps IR + disasm; GLSL->SPIR-V: glslangValidator -V --target-env vulkan1.1.

## Backends in this tag
- Old `bi_*` Bifrost/Valhall (src/panfrost/compiler/bifrost, bifrost_compile.c) - built, default. Midgard also present (arch<6).
- Kraid (Rust, src/panfrost/compiler/kraid, isa-v9-v14.xml + isa-v15.xml): present but NOT built here. Needs
  `-Dpanfrost-rust=true` (meson.build:920, adds -DWITH_PANFROST_RUST) + bindgen>=0.71.1 (not installed; `cargo install bindgen-cli`) + rustc>=1.85 (have 1.100).
  Runtime select: env PAN_USE_KRAID=cs,fs,vs,internal|all (pan_compiler.c:46-77, pan_use_kraid at :60); default off in 26.3.
  NIR options differ: pan_get_nir_shader_compiler_options() returns Kraid's only if Kraid is on for the stage AND internal (:116).
  => our interface must take nir_options from that function and set PAN_USE_KRAID before first compile (debug_get_option is cached once).
- Old bi_* backend "knobs": BIFROST_MESA_DEBUG=nosched,noopt,shaders,verbose,statsfull... (bi_debug.c:36).

## Results (inputs: const.frag = const colour out; pos.vert = position + 1 varying passthrough; outputs out_v*.{txt,bin,disasm.txt})
| shader | arch | binary | disasm lines | work regs | FAU words (reserved 8) | notes |
| FS const | v11 | 128 B | 10 | 32* | 3 | ATEST+BLEND, blend_descriptor in FAU; can_early_z,can_fpk |
| FS const | v13 | 128 B | 10 | 32* | 3 | same |
| VS pos+varying | v11 | 384 B | 16 | 32* | 11 | IDVS: position part + secondary (varying) shader at offset 256 |
| VS pos+varying | v13 | 256 B | 33 | 32* | 13 | no secondary shader; clip/cull + viewport transform in-shader |
*work_reg_count reports 32 on Valhall (granularity of allocation bucket), preload FS 0x3000000000000000, VS 0x3800000000000000.
Disassembly round-trips through `bifrost_compiler --id=<gpu_id>>16 disasm bin` (v11: --id=47107, v13: --id=55296).
Metadata the driver needs = struct pan_shader_info (pan_compiler.h:427): work_reg_count, tls/wls_size, preload, fau{count,words[],reserved},
ubo_count, attribute_count, texture/sampler_count, varyings.formats, bifrost.{uses_ld_var,wait_6,wait_7}, fs.*, vs.secondary_{enable,offset,work_reg_count,preload}.
Spike prints them; packing into RSD/SPD descriptors is pan_shader.h (genxml-dependent) - our own code must replicate this.

## Library entry points (src/panfrost/compiler/pan_compiler.h; contract comment at :60-75)
1. spirv_to_nir() (compiler/spirv/nir_spirv.h:176; NIR_SPIRV_VULKAN env) with nir options from pan_get_nir_shader_compiler_options(arch,stage,merge_wg) (pan_compiler.c:116).
2. Driver early lowering (spike.c: init-lowering, inline, vars_to_ssa, io_vars_to_temporaries, system_values) - see PanVK flow
   panvk_vX_shader.c:431-500 (oracle only).
3. pan_preprocess_nir(nir, arch) (pan_compiler.c:156, .h:78)
4. Driver: descriptor lowering, nir_assign_io_var_locations, nir_lower_io + pan_nir_lower_mediump_io, VS: pan_varying_collect_formats +
   pan_build_varying_layout_compact (.h:358,355), FS: pan_nir_lower_fs_outputs, VS attrib driver_location = location-GENERIC0 (panvk_vX_shader.c:984-1000,1480-1520,1562).
5. pan_postprocess_nir(nir, &inputs, &info) (pan_compiler.c:180)
6. ONLY sysval lowering to load_push_constant -> FAU (spike: load_viewport_scale/offset; without it bifrost_compile crashes in bi_alu_src_index);
   then set inputs.fau.reserved (words) + promote_immediates.
7. pan_shader_compile(nir, &inputs, &util_dynarray bin, &info) (pan_compiler.c:371).
Inputs struct pan_compile_inputs (.h:35): arch, model (pan_get_model(gpu_id,variant,cores,features); G615 id 0xB8030000, G725 0xD8000000), varying_layout, fau, robust_*.

## Dependency surface (link line of panfrost_compile): libpanfrost_compiler + _bifrost(+_disasm, valhall_disasm, midgard) + libnir + libcompiler (glsl_types, shader_enums)
+ libvtn + libmesa_util(+c11, simd, blake3, clflush) + libpanfrost_model_lib + libpanfrost_lib (pan_props/format) + libpan-format-v*/pan-arch-v* + libpanfrost_decode/pandecode
+ pankmod_lib (all three only for pan_get_model/format tables and libpanfrost_lib deps; trimmable) + system: SPIRV-Tools(opt) (vtn), zlib, zstd, libdrm, pthread.
Generated headers needed: nir_opcodes.h, nir_builder_opcodes.h, bi_opcodes.h, genxml gen_pack, nir_intrinsics*. Include roots: include/, src/, src/compiler/{nir,spirv}, src/panfrost/*, src/util.
Not needed for the compiler: gallium, Vulkan runtime, LLVM (only for mesa-clc at build time).
Unresolved: Kraid not built; trimming libpanfrost_lib/pankmod; descriptor lowering, blend shaders, FAU->push mapping, stats (pan_stats not printed).
