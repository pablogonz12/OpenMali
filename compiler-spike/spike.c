/* compiler-spike: SPIR-V (Vulkan) -> NIR -> Mali Valhall binary, offline.
 * Usage: spike <arch 11|13> <vert|frag> <in.spv> <out.bin> */
#include "compiler/glsl_types.h"
#include "compiler/spirv/nir_spirv.h"
#include "panfrost/compiler/bifrost/bifrost_compile.h"
#include "panfrost/compiler/pan_compiler.h"
#include "panfrost/compiler/pan_nir.h"
#include "panfrost/model/pan_model.h"
#include "nir.h"
#include "nir_builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t *slurp(const char *p, size_t *n){
   FILE *f=fopen(p,"rb"); if(!f){perror(p);exit(1);} fseek(f,0,SEEK_END); *n=ftell(f); fseek(f,0,SEEK_SET);
   void *b=malloc(*n); if(fread(b,1,*n,f)!=*n) exit(1); fclose(f); return b; }

/* Driver-owned sysval lowering: the ONLY thing allowed between pan_postprocess_nir and pan_shader_compile.
 * Our driver defines the push-constant/FAU layout; here viewport scale at byte 0, offset at byte 12. */
static bool lower_sysvals(nir_builder *b, nir_intrinsic_instr *i, void *d){
   unsigned off;
   switch (i->intrinsic) {
   case nir_intrinsic_load_viewport_scale:  off = 0;  break;
   case nir_intrinsic_load_viewport_offset: off = 12; break;
   default: return false; }
   b->cursor = nir_before_instr(&i->instr);
   nir_def *v = nir_load_push_constant(b, i->def.num_components, i->def.bit_size, nir_imm_int(b, off));
   nir_def_replace(&i->def, v);
   return true;
}

/* User push constants (SPIR-V push_constant block) sit at byte SPIKE_PUSH_BASE of the FAU table (default 0). */
static bool add_push_base(nir_builder *b, nir_intrinsic_instr *i, void *d){
   if (i->intrinsic != nir_intrinsic_load_push_constant) return false;
   unsigned base = *(unsigned *)d;
   if (!base) return false;
   b->cursor = nir_before_instr(&i->instr);
   nir_src_rewrite(&i->src[0], nir_iadd_imm(b, i->src[0].ssa, base));
   return true;
}

/* Texture/sampler derefs -> constant Valhall resource handles (what panvk_vX_nir_lower_descriptors.c does at :921-953 for PAN_ARCH >= 9).
 * Combined image sampler of user set 0: FS resource table entry SPIKE_TEX_TABLE (default 1; entry 0 is the driver set), texture at
 * descriptor SPIKE_TEX_IDX (default 0), sampler at SPIKE_SAMP_IDX (default 1: panvk puts the sampler after the texture, subdesc_idx). */
static bool lower_tex_handles(nir_builder *b, nir_instr *instr, void *d){
   if (instr->type != nir_instr_type_tex) return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   int ti = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
   int si = nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);
   if (ti < 0 || si < 0) return false;
   unsigned table = getenv("SPIKE_TEX_TABLE") ? atoi(getenv("SPIKE_TEX_TABLE")) : 1;
   unsigned ti_idx = getenv("SPIKE_TEX_IDX") ? atoi(getenv("SPIKE_TEX_IDX")) : 0;
   unsigned si_idx = getenv("SPIKE_SAMP_IDX") ? atoi(getenv("SPIKE_SAMP_IDX")) : 1;
   b->cursor = nir_before_instr(instr);
   nir_tex_instr_remove_src(tex, MAX2(ti, si));
   nir_tex_instr_remove_src(tex, MIN2(ti, si));
   nir_tex_instr_add_src(tex, nir_tex_src_texture_handle, nir_imm_int(b, pan_res_handle(table, ti_idx)));
   nir_tex_instr_add_src(tex, nir_tex_src_sampler_handle, nir_imm_int(b, pan_res_handle(table, si_idx)));
   tex->texture_index = 0;
   tex->sampler_index = 0;
   return true;
}

static unsigned type_size(const struct glsl_type *t, bool bindless){ return glsl_count_attribute_slots(t, false); }

int main(int argc, char **argv){
   if(argc<5) return 1;
   unsigned arch = atoi(argv[1]);
   mesa_shader_stage stage = !strcmp(argv[2],"vert") ? MESA_SHADER_VERTEX : MESA_SHADER_FRAGMENT;
   size_t n; const uint32_t *spv = slurp(argv[3], &n);

   /* GPU id: arch<<28 | arch_minor<<24 | prod_major<<16 (G615 = v11 0xB8030000? see notes) */
   uint64_t gpu_id = arch==11 ? 0xB8030000ull : 0xD8000000ull;
   const struct pan_model *model = NULL;
   for (unsigned v=0; v<16 && !model; v++) model = pan_get_model(gpu_id, v, 16, 0);
   fprintf(stderr, "model: %s\n", model ? model->name : "NULL");

   glsl_type_singleton_init_or_ref();
   const nir_shader_compiler_options *opts = pan_get_nir_shader_compiler_options(arch, stage, false);

   struct spirv_to_nir_options so = {
      .environment = NIR_SPIRV_VULKAN,
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_64bit_global_32bit_offset,
      .phys_ssbo_addr_format = nir_address_format_64bit_global,
      .shared_addr_format = nir_address_format_32bit_offset,
      .min_ubo_alignment = 16, .min_ssbo_alignment = 16,
      .push_const_addr_format = nir_address_format_32bit_offset,
   };
   unsigned fau_reserved = getenv("SPIKE_FAU_RESERVED") ? atoi(getenv("SPIKE_FAU_RESERVED")) : 8;
   unsigned push_base = getenv("SPIKE_PUSH_BASE") ? atoi(getenv("SPIKE_PUSH_BASE")) : 0;
   nir_shader *nir = spirv_to_nir(spv, n/4, NULL, stage, "main", &so, opts);
   if(!nir){fprintf(stderr,"spirv_to_nir failed\n");return 1;}

   /* generic early lowering (subset of vk_spirv_to_nir / panvk_preprocess_nir) */
   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   nir_remove_non_entrypoints(nir);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_lower_io_vars_to_temporaries, nir_shader_get_entrypoint(nir), nir_var_shader_out);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_lower_system_values);
   pan_preprocess_nir(nir, arch);
   /* API push constants -> load_push_constant with constant byte offsets (as panvk_vX_shader.c:884), shifted by push_base */
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const, nir_address_format_32bit_offset);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, add_push_base, nir_metadata_control_flow, &push_base);
   NIR_PASS(_, nir, nir_opt_constant_folding);

   struct pan_compile_inputs in = { .arch = arch, .model = model };
   if (stage == MESA_SHADER_VERTEX) {
      nir_foreach_shader_in_variable(var, nir)
         var->data.driver_location = var->data.location - VERT_ATTRIB_GENERIC0;
      nir_assign_io_var_locations(nir, nir_var_shader_out);
   } else {
      nir_assign_io_var_locations(nir, nir_var_shader_in);
      nir_assign_io_var_locations(nir, nir_var_shader_out);
   }
   NIR_PASS(_, nir, nir_lower_var_copies);
   NIR_PASS(_, nir, nir_lower_indirect_derefs_to_if_else_trees, nir_var_shader_in|nir_var_shader_out, UINT32_MAX);
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in|nir_var_shader_out, type_size,
            nir_lower_io_use_interpolated_input_intrinsics);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   pan_nir_lower_mediump_io(nir);

   struct pan_varying_layout vl;
   if (stage == MESA_SHADER_VERTEX) {
      pan_varying_collect_formats(&vl, nir, arch);
      pan_build_varying_layout_compact(&vl, nir, arch);
      in.varying_layout = &vl;
      if (getenv("SPIKE_LAYOUT_OUT")) { FILE *lo = fopen(getenv("SPIKE_LAYOUT_OUT"), "wb"); fwrite(&vl, sizeof vl, 1, lo); fclose(lo); }
   } else {
      NIR_PASS(_, nir, pan_nir_lower_fs_outputs, false, 0);
      /* PanVK links the FS against the VS layout (panvk_vX_shader.c:1550): LD_VAR_BUF_IMM instead of attribute-descriptor LD_VAR_IMM */
      if (getenv("SPIKE_LAYOUT_IN")) {
         FILE *li = fopen(getenv("SPIKE_LAYOUT_IN"), "rb");
         if (li && fread(&vl, sizeof vl, 1, li) == 1) in.varying_layout = &vl;
         if (li) fclose(li);
      }
   }

   NIR_PASS(_, nir, nir_shader_instructions_pass, lower_tex_handles, nir_metadata_control_flow, NULL);
   nir_remove_dead_variables(nir, nir_var_uniform | nir_var_image, NULL);

   struct pan_shader_info info = {0};
   pan_postprocess_nir(nir, &in, &info);
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_sysvals, nir_metadata_control_flow, NULL);
   in.fau.reserved = fau_reserved; /* 32-bit words reserved for driver sysvals/push consts */
   in.fau.promote_immediates = true;

   struct util_dynarray bin; util_dynarray_init(&bin, NULL);
   pan_shader_compile(nir, &in, &bin, &info);

   unsigned sz = util_dynarray_num_elements(&bin, uint8_t);
   FILE *o = fopen(argv[4], "wb"); fwrite(util_dynarray_begin(&bin), 1, sz, o); fclose(o);
   printf("arch=%u stage=%s binary_size=%u\n", arch, argv[2], sz);
   printf("work_reg_count=%u tls_size=%u wls_size=%u preload=0x%llx\n", info.work_reg_count, info.tls_size, info.wls_size, (unsigned long long)info.preload);
   printf("fau.count=%u (words) fau.reserved=%u ubo_count=%u attribute_count=%u texture_count=%u sampler_count=%u\n",
          info.fau.count, info.fau.reserved, info.ubo_count, info.attribute_count, info.texture_count, info.sampler_count);
   printf("varyings.formats.count=%u uses_ld_var=%d wait_6=%d wait_7=%d writes_global=%d\n",
          info.varyings.formats.count, info.bifrost.uses_ld_var, info.bifrost.wait_6, info.bifrost.wait_7, info.writes_global);
   if (stage==MESA_SHADER_FRAGMENT)
      printf("fs: can_discard=%d writes_depth=%d early_fragment_tests=%d can_early_z=%d can_fpk=%d sidefx=%d reads_frag_coord=%d\n",
         info.fs.can_discard, info.fs.writes_depth, info.fs.early_fragment_tests, info.fs.can_early_z, info.fs.can_fpk, info.fs.sidefx, info.fs.reads_frag_coord);
   else
      printf("vs: writes_point_size=%d secondary_enable=%d secondary_offset=%u secondary_work_reg_count=%u\n",
         info.vs.writes_point_size, info.vs.secondary_enable, info.vs.secondary_offset, info.vs.secondary_work_reg_count);
   /* machine-readable metadata, consumed by gen-shaders.sh */
   printf("META size=%u work_reg_count=%u preload=0x%llx fau_count=%u fau_reserved=%u\n", sz,
          info.work_reg_count, (unsigned long long)info.preload, info.fau.count, info.fau.reserved);
   for (unsigned i = info.fau.reserved; i < info.fau.count; i++)
      if (BITSET_TEST(info.fau.is_const, i))
         printf("FAU %u 0x%08x\n", i, info.fau.words[i].constant);
   if (stage == MESA_SHADER_VERTEX)
      printf("META_VS secondary_enable=%d secondary_offset=%u secondary_work_reg_count=%u secondary_preload=0x%llx generic_size_B=%u\n",
             info.vs.secondary_enable, info.vs.secondary_offset, info.vs.secondary_work_reg_count,
             (unsigned long long)info.vs.secondary_preload, vl.generic_size_B);
   else
      printf("META_FS fs_varying_attr_desc_count=%u\n", 0u);
   {
      const struct pan_varying_layout *L = stage == MESA_SHADER_VERTEX ? &vl : &info.varyings.formats;
      for (unsigned i = 0; i < L->count; i++) {
         const struct pan_varying_slot *s = pan_varying_layout_slot_at(L, i);
         if (s) printf("VSLOT %u loc=%u section=%u offset=%u ncomps=%u alu=0x%x\n", i, (unsigned)s->location,
                       (unsigned)s->section, (unsigned)s->offset, (unsigned)s->ncomps, (unsigned)s->alu_type);
      }
   }
   return 0;
}
