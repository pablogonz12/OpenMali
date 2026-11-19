/* SPDX-License-Identifier: MIT */
/* mali_compile(): SPIR-V -> NIR -> Mesa panfrost compiler (Valhall v11/v13) -> binary + metadata.
 * Plain C, no Mesa headers: the .c is the only user of Mesa internals. */
#pragma once
#include <stdio.h>
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
   unsigned fau_reserved;         /* 32-bit FAU words owned by the driver; 0 = default (VS 24, FS 8, CS 8) */
   unsigned push_base;            /* byte offset of the API push constant block inside the FAU table; VS default 40 (words 0-5 viewport, 6 BaseVertex, 7 BaseInstance, 8 DrawIndex), else 0 */
   int push_base_set;             /* 0: use the default above */
   const struct mali_varying_blob *link_vs;   /* FS: layout produced by the VS compile; NULL = attribute-descriptor varyings */
   const struct mali_compile_layout *layout;  /* NULL = descriptor-free (texture handle stub); else M7 descriptor lowering (UBO/SSBO/textures) */
   unsigned clip_distances;       /* FS: gl_ClipDistance count written by the VS (compiled.vs_clip_distances); the FS discards where an interpolated distance < 0 */
   int sample_shading;            /* FS: pipeline sampleShadingEnable with minSampleShading * samples > 1 (PanVK: nir->info.fs.uses_sample_shading) */
   int strip_psiz;                /* VS: drop the PointSize output (non-point topologies; the HW would read it as an array) */
   unsigned nspec;                         /* VkSpecializationInfo: mapEntryCount (0 = none) */
   const struct mali_spec_entry *spec;     /* pMapEntries */
   const void *spec_data;                  /* pData; every entry must lie inside it (caller checks dataSize) */
   const char *cache_dir;         /* NULL = $MALI_SHADER_CACHE or $XDG_CACHE_HOME/mali-gaming/shaders; "" = no disk cache */
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
   uint32_t fs_can_discard, fs_writes_depth, fs_writes_stencil, fs_writes_colour, fs_early_fragment_tests, fs_can_early_z, fs_can_fpk, fs_sidefx, fs_reads_frag_coord;
   /* VS */
   uint32_t vs_writes_point_size, vs_secondary_enable, vs_secondary_offset, vs_secondary_work_reg_count;
   uint64_t vs_secondary_preload;
   uint32_t vs_clip_distances;    /* VS: gl_ClipDistance array size (0 = none) */
   uint32_t generic_size_B;       /* VS: size of the generic varying section per vertex */
   uint32_t nvarying;
   struct mali_varying_slot varying[64];   /* VS: layout it writes; FS: layout it reads */
   struct mali_varying_blob layout;        /* VS: pass to the FS compile (link_vs) */
   uint32_t local_size[3];        /* CS: workgroup size */
   uint32_t allow_merge;          /* CS: COMPUTE_SIZE_WORKGROUP bit 31 (no shared memory, workgroup barrier or subgroup op: HW may merge workgroups) */
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
};
int mali_compile_blend(const struct mali_blend_opts *o, struct mali_compiled *out, char *err, size_t errsz);

/* 0 on success (out filled, release with mali_compiled_free); negative on error, message in err (may be NULL). */
int mali_compile(const struct mali_compile_opts *o, struct mali_compiled *out, char *err, size_t errsz);
void mali_compiled_free(struct mali_compiled *c);

/* Cache statistics of this process. */
struct mali_compile_stats {
   uint64_t hits, misses, stores;
   uint64_t bytes_read, bytes_written;   /* entry bytes loaded by hits / written by stores */
   uint64_t evictions, evicted_bytes;    /* entries removed by the LRU trimmer */
   uint64_t rejected;                    /* entries found but dropped (truncated, bit-flipped); also counted in misses */
};
void mali_compile_stats(struct mali_compile_stats *s);
/* One line with all counters; MALI_STATS=1 makes the process print it to stderr at exit. */
void mali_compile_stats_print(FILE *f);

/* 16-byte pipelineCacheUUID for a GPU id: changes with the compiler version, Mesa tag and every layout constant in the cache key. */
void mali_compile_cache_uuid(uint32_t gpu_id, uint8_t uuid[16]);

/* Disk cache size limit ($MALI_SHADER_CACHE_MAX_KB, default 1 GiB): delete the least recently used entries of `dir` until it is at 90% of
 * max_bytes (0 = no limit); temp files older than 10 min go in any case; an untrusted dir is left alone. Run automatically by the cache writer. */
void mali_compile_cache_trim(const char *dir, unsigned long long max_bytes);

#ifdef __cplusplus
}
#endif
