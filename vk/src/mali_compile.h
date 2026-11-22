/* SPDX-License-Identifier: MIT */
/* mali_compile(): SPIR-V -> NIR -> Mesa panfrost compiler (Valhall v11/v13) -> binary + metadata.
 * Plain C, no Mesa headers: the .c is the only user of Mesa internals. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "mali_pipeline_layout_flat.h"

#ifdef __cplusplus
extern "C" {
#endif

enum mali_stage { MALI_STAGE_VERTEX = 0, MALI_STAGE_FRAGMENT = 1, MALI_STAGE_COMPUTE = 2 };

#define MALI_VARYING_BLOB_MAX 1024

/* VS varying layout, passed to the FS compile so that it reads the varying buffer (LD_VAR_BUF_IMM). Opaque bytes. */
struct mali_varying_blob {
   uint32_t size;                          /* 0 = none */
   uint8_t data[MALI_VARYING_BLOB_MAX];
};

struct mali_varying_slot {
   uint16_t location;   /* gl_varying_slot (VARYING_SLOT_VAR0 = 32 + n) */
   uint8_t section;     /* pan_varying_section: 0 special/position header, 1 generic, ... */
   uint8_t ncomps;
   int16_t offset;      /* bytes inside the section */
   uint16_t pad;
   uint32_t alu_type;   /* nir_alu_type of the stored value */
};

/* One VkSpecializationMapEntry: constant id, byte offset into spec_data, byte size. */
struct mali_spec_entry { uint32_t id, offset, size; };

struct mali_compile_opts {
   unsigned arch;                 /* 11 or 13 */
   enum mali_stage stage;
   const uint32_t *spirv;
   size_t spirv_words;
   const char *entry;             /* NULL = "main" */
   unsigned xfb_pc;               /* VS only: push-constant offset of the 4 XFB buffer addresses (app push size) */
   unsigned xfb;                  /* VS only: the pipeline layout reserves 32 B at xfb_pc for them (0 = no XFB) */
   unsigned fau_reserved;         /* 32-bit FAU words owned by the driver; 0 = default (VS 24, FS 8, CS 8) */
   unsigned push_base;            /* byte offset of the API push constant block inside the FAU table; VS default 40 (words 0-5 viewport, 6 BaseVertex, 7 BaseInstance, 8 DrawIndex), else 0 */
   int push_base_set;             /* 0: use the default above */
   const struct mali_varying_blob *link_vs;   /* FS: layout produced by the VS compile; NULL = attribute-descriptor varyings */
   /* VS: the fragment shader that consumes the outputs, so the noperspective bitfield the backend asks for
    * (nir_intrinsic_load_noperspective_varyings_pan, inlined by pan_nir_lower_noperspective_vs) is the one the FS
    * actually interpolates with. NULL = no FS (rasterizer discard, meta shaders): mask 0. */
   const uint32_t *fs_spirv;
   size_t fs_spirv_words;
   const char *fs_entry;
   const struct mali_compile_layout *layout;  /* NULL = descriptor-free (texture handle stub); else M7 descriptor lowering (UBO/SSBO/textures) */
   unsigned clip_distances;       /* FS: gl_ClipDistance count written by the VS (compiled.vs_clip_distances); the FS discards where an interpolated distance < 0 */
   unsigned cull_distances;       /* FS: gl_CullDistance count written by the VS (compiled.vs_cull_distances); the VS stores d>=0 as 1, the FS discards where one interpolates to 0 (all three vertices negative) */
   unsigned cull_varying;         /* FS: the varying the VS wrote the merged clip+cull array in (compiled.cull_varying). NOT num_inputs: the VS's gl_Position takes varying 0, so the array is at 1 even when the FS reads nothing */
   int sample_shading;            /* FS: pipeline sampleShadingEnable with minSampleShading * samples > 1 (PanVK: nir->info.fs.uses_sample_shading) */
   int strip_psiz;                /* VS: drop the PointSize output (non-point topologies; the HW would read it as an array) */
   unsigned nspec;                         /* VkSpecializationInfo: mapEntryCount (0 = none) */
   const struct mali_spec_entry *spec;     /* pMapEntries */
   const void *spec_data;                  /* pData; every entry must lie inside it (caller checks dataSize) */
   const char *cache_dir;         /* NULL = $MALI_SHADER_CACHE or $XDG_CACHE_HOME/mali-gaming/shaders; "" = no disk cache */
   unsigned subgroup_size;        /* CS: VkPipelineShaderStageRequiredSubgroupSizeCreateInfo::requiredSubgroupSize; 0 = the arch's warp width.
                                   * Any other value than mali_subgroup_size_for_arch(arch) is refused: the hardware has one width. */
};

struct mali_imm { uint32_t word, value; };   /* promoted immediate: FAU word index (32-bit), value */

struct mali_compiled {
   uint32_t arch;
   uint32_t stage;
   uint8_t *bin;                  /* aligned(16) not guaranteed: copy to GPU memory */
   uint32_t bin_size;
   uint32_t work_reg_count, tls_size, wls_size;
   uint64_t preload;              /* register preload mask for the main (or VS position) shader */
   uint32_t fau_count, fau_reserved;   /* words; [0,fau_reserved) are filled by the driver */
   uint32_t nimm;
   struct mali_imm *imm;          /* nimm entries, words >= fau_reserved */
   uint32_t push_const_bytes;     /* highest API push constant byte used (before push_base), 0 = none */
   uint32_t ubo_count, attribute_count, texture_count, sampler_count;
   uint32_t uses_ld_var, wait_6, wait_7, writes_global;
   /* FS */
   uint32_t fs_colour_mask /* bit i: writes colour output i */, fs_can_discard, fs_writes_depth, fs_writes_stencil, fs_writes_colour, fs_early_fragment_tests, fs_can_early_z, fs_can_fpk, fs_sidefx, fs_reads_frag_coord;
   /* VS */
   uint32_t vs_writes_point_size, vs_secondary_enable, vs_secondary_offset, vs_secondary_work_reg_count;
   uint64_t vs_secondary_preload;
   uint32_t vs_clip_distances;    /* VS: gl_ClipDistance array size (0 = none) */
   uint32_t noperspective_mask;   /* VS: FS's noperspective bitfield inlined into the code; FS: the bitfield its inputs use */
   uint32_t vs_cull_distances;    /* VS: gl_CullDistance array size (0 = none), lowered to 0/1 varyings */
   uint32_t cull_varying;         /* VS: driver_location of the merged clip+cull varying (handed to the FS compile); FS: the one it read */
   uint32_t cull_varying_first;   /* the varying carrying cull distance 0 / the last one: the pipeline refuses a VS/FS disagreement */
   uint32_t cull_varying_last;
   uint32_t generic_size_B;       /* VS: size of the generic varying section per vertex */
   uint32_t nvarying;
   struct mali_varying_slot varying[64];   /* VS: layout it writes; FS: layout it reads */
   struct mali_varying_blob layout;        /* VS: pass to the FS compile (link_vs) */
   uint32_t local_size[3];        /* CS: workgroup size */
   uint32_t allow_merge;          /* CS: COMPUTE_SIZE_WORKGROUP bit 31 (no shared memory, workgroup barrier or subgroup op: HW may merge workgroups) */
   uint32_t subgroup_size;        /* CS: the warp width the code was compiled for (mali_subgroup_size_for_arch) */
   uint32_t cache_hit;            /* 1 if this result came from the disk cache */
   uint32_t fs_sample_shading;   /* FS runs per sample (info.fs.sample_shading): DCD0 "Evaluate per-sample" */
   uint32_t fs_blend0_type, fs_blend_src1_type;   /* FS: nir_alu_type of colour output 0 / dual-source output (0 = none) */
};

/* Blend shader (dual-source blending, or any equation the fixed-function unit cannot do): single RT.
 * Factors/ops are Vulkan enums. src0/src1_type are the FS's fs_blend0_type / fs_blend_src1_type.
 * Constants are read from FS FAU words 0..3. out->bin is the code only (16-byte aligned copy needed). */
struct mali_blend_opts {
   unsigned arch;                 /* 11 or 13 */
   unsigned vk_format;            /* VkFormat of the colour attachment */
   unsigned samples;
   unsigned rgb_op, alpha_op;     /* VkBlendOp */
   unsigned rgb_src, rgb_dst, alpha_src, alpha_dst;   /* VkBlendFactor */
   unsigned color_mask;           /* VkColorComponentFlags */
   unsigned src0_type, src1_type;
   unsigned logic_op_enable, logic_op;   /* VkLogicOp: replaces the equation (colour logic op on the destination) */
};
int mali_compile_blend(const struct mali_blend_opts *o, struct mali_compiled *out, char *err, size_t errsz);

/* mali_compile: 0 on success (out filled, release with mali_compiled_free); negative on error, message in err.
 * MALI_COMPILE_UNSUPPORTED means the shader is valid but this driver has no path for it yet (callers map it to
 * VK_ERROR_FEATURE_NOT_PRESENT rather than VK_ERROR_INVALID_SHADER_NV). */
#define MALI_COMPILE_UNSUPPORTED (-2)

int mali_compile(const struct mali_compile_opts *o, struct mali_compiled *out, char *err, size_t errsz);

/* 1 when the SPIR-V declares transform-feedback outputs (OpDecorate ... DecorationXfbBuffer), i.e. when the
 * pipeline layout must reserve the driver's 32 B of XFB buffer addresses after the application's push constants.
 * Checked before compiling because the layout is fixed at pipeline creation. */
int mali_spirv_has_xfb(const uint32_t *words, uint32_t nwords);
void mali_compiled_free(struct mali_compiled *c);

/* Threads per warp (subgroup width) of a Mali architecture. Mirrors Mesa's pan_subgroup_size() for the arches we support:
 * Valhall and later (arch >= 9) execute 16 lanes, Bifrost 8, Midgard 4 (no subgroups: 1). The G925 shader_present mask
 * (0xee0057) is non-contiguous in a way that only makes sense at 16. This is the single source for the reported
 * VkPhysicalDeviceSubgroupProperties::subgroupSize and for the compiler's lower_subgroups width. */
unsigned mali_subgroup_size_for_arch(unsigned arch);

/* Cache statistics of this process. */
struct mali_compile_stats { uint64_t hits, misses, stores; };
void mali_compile_stats(struct mali_compile_stats *s);

#ifdef __cplusplus
}
#endif
