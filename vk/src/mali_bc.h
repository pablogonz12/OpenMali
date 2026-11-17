/* SPDX-License-Identifier: MIT */
/* R4-M7: BC1-BC7 (S3TC/RGTC/BPTC) sampled images on Mali (no BC hardware) by decode-to-shadow.
 *
 * Model (same trick as Mesa vk_texcompress_etc2/astc, but one linear shadow instead of a storage-image view):
 *   - a BC image owns TWO regions in one allocation: the compressed data in the real block layout (copies, vkCmdCopyImageToBuffer, buffer<->image
 *     meta paths work unchanged on it, bpt = block bytes, element = block) and a hidden decoded SHADOW (RGBA8 / R8 / RG8 / RGBA16F, linear,
 *     rows padded to whole 4x4 blocks, 64 B aligned).
 *   - after every write to the compressed data (CopyBufferToImage2, CopyImage2 into it, UpdateBuffer cannot, clears are illegal) the command
 *     buffer appends mali_bc_plan_decode() for the written subresources: one compute job per region, 1 invocation = 1 block, 2 SSBO bindings
 *     (compressed, shadow), 48 B push constants. Frames are serialised (R3 M12), so no barrier is needed before the first sampler read.
 *   - image views of a BC image (same format only, no block-texel-view in this first cut) are built over the shadow with the shadow format.
 * Plain C like mali_meta.h (only <vulkan/vulkan_core.h> for the enums). mali_bc_ref_run() is the host-pointer software reference of the shaders
 * with a pluggable block decoder (tests use Mesa's util_format_*). Shaders: shader/bc{1,4,6h,7}.comp -> mali_bc_spv.h (gen-bc-spv.sh). */
#ifndef MALI_BC_H
#define MALI_BC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALI_BC_MAX_MIPS 16u
#define MALI_BC_PUSH_WORDS 12

enum mali_bc_shader { MALI_BC_BC1 = 0, MALI_BC_BC4 = 1, MALI_BC_BC6H = 2, MALI_BC_BC7 = 3, MALI_BC_NSHADERS };

/* push `mode` words: BC1 shader 0 BC1-RGBA, 1 BC2, 2 BC3, 3 BC1-RGB; BC4 shader bit0 signed bit1 two channels; BC6H bit0 signed; BC7 0. */
struct mali_bc_fmt {
   VkFormat vk;
   const char *name;
   enum mali_bc_shader sh;
   uint32_t mode;
   uint8_t block_bytes;      /* 8 or 16 */
   VkFormat shadow;          /* decoded format the views use */
   uint8_t shadow_bpt;       /* bytes per decoded texel: 4, 1, 2, 8 */
   bool srgb;
};

/* NULL when `f` is not one of the 18 BC formats. */
const struct mali_bc_fmt *mali_bc_format(VkFormat f);
bool mali_bc_is_bc(VkFormat f);

/* ---- advertisement ---- */
/* optimalTilingFeatures of a BC format: SAMPLED + FILTER_LINEAR + BLIT_SRC + TRANSFER_SRC/DST (the Vulkan minimum for textureCompressionBC);
 * 0 for non-BC. linear tiling and buffer features are 0 (compressed images are optimal-only, like mali_format.c). */
VkFormatFeatureFlags mali_bc_optimal_features(VkFormat f);
/* 0 or VK_ERROR_FORMAT_NOT_SUPPORTED: BC supports 2D and 3D (not 1D), optimal tiling, 1 sample, usage in {SAMPLED, TRANSFER_SRC, TRANSFER_DST}
 * (no storage, attachments, input attachment: the shadow is read-only), no DISJOINT/ALIAS-less creation flags beyond
 * {MUTABLE_FORMAT only when the view formats are the same BC format or its shadow-compatible sRGB/UNORM twin (not checked here), CUBE_COMPATIBLE,
 * 2D_ARRAY_COMPATIBLE, SPARSE* not}. BLOCK_TEXEL_VIEW_COMPATIBLE is rejected. */
VkResult mali_bc_image_supported(VkFormat f, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags,
                                 uint32_t samples);

/* ---- layout (pure) ---- */
struct mali_bc_level {
   uint64_t off;             /* from the start of the layer's first level */
   uint32_t row;             /* bytes per block row (compressed) / per texel row of the 4-aligned level (shadow) */
   uint32_t w, h, d;         /* texels of the level (shadow rows/slices are padded to w,h rounded up to 4) */
   uint32_t bw, bh;          /* blocks */
   uint64_t slice;           /* bytes per z slice (3D) */
   uint64_t size;            /* slice * d */
};
struct mali_bc_surf {
   uint64_t base;            /* from the start of the image allocation */
   uint64_t array_stride;
   uint64_t size;            /* array_stride * layers */
   struct mali_bc_level lv[MALI_BC_MAX_MIPS];
};
struct mali_bc_layout {
   const struct mali_bc_fmt *fmt;
   VkImageType type;
   VkExtent3D ext;
   uint32_t levels, layers;
   struct mali_bc_surf comp, shadow;
   uint64_t size;            /* total allocation: comp + shadow (the memory requirement) */
   uint32_t alignment;       /* 64 */
};
#define MALI_BC_ALIGN 64u

/* 0, -EINVAL (not BC / zero or too many levels / extents / 1D / layers>1 with depth>1), -ERANGE (> 4 GiB per region). */
int mali_bc_layout_init(VkFormat f, VkImageType type, VkExtent3D ext, uint32_t levels, uint32_t layers, struct mali_bc_layout *out);

/* ---- decode planner ---- */
struct mali_bc_bind { uint64_t va; uint32_t size; };

struct mali_bc_job {
   enum mali_bc_shader sh;
   const struct mali_bc_fmt *fmt;
   uint32_t push[MALI_BC_PUSH_WORDS];   /* src_off src_row src_slice dst_off dst_row dst_slice bw bh bz mode pad pad (byte offsets from the binds) */
   unsigned push_bytes;                 /* 48 */
   struct mali_bc_bind bind[2];         /* 0 compressed (set 0 binding 0), 1 shadow (binding 1) */
   unsigned nbind;                      /* 2 */
   uint32_t groups[3];                  /* local size 64 x1x1; x <= 32768, rest in y */
};
struct mali_bc_list { struct mali_bc_job *j; unsigned n, cap; };
void mali_bc_list_free(struct mali_bc_list *l);

/* Decode the compressed region (level, layers [layer0, layer0+nlayers), texel offset/extent; offset x,y multiples of 4, extent x,y multiples of 4
 * or reaching the level edge) into the shadow. `image_va` = GPU address of the allocation start. 3D images: nlayers must be 1 and z comes from off/ext;
 * array images: z is the layer and off.z = 0, ext.depth = 1. One job. 0, -EINVAL (bounds/alignment), -ENOMEM, -ERANGE. */
int mali_bc_plan_decode(struct mali_bc_list *l, const struct mali_bc_layout *lay, uint64_t image_va, uint32_t level, uint32_t layer0,
                        uint32_t nlayers, VkOffset3D off, VkExtent3D ext);
/* Whole image: every level, every layer (one job per level). */
int mali_bc_plan_decode_all(struct mali_bc_list *l, const struct mali_bc_layout *lay, uint64_t image_va);

/* Software reference of the jobs: bind VAs are host pointers. `dec` decodes ONE block (block_bytes of input) to 4 rows of 4 shadow texels
 * (out stride 4*shadow_bpt bytes per row, 16 texels row-major). Replicates the shader addressing (so planner bugs show). */
typedef void (*mali_bc_block_fn)(const struct mali_bc_fmt *f, const uint8_t *block, uint8_t *out);
void mali_bc_ref_run(const struct mali_bc_job *j, mali_bc_block_fn dec);

/* ---- shader access (SPIR-V embedded in mali_bc_spv.h) ---- */
const uint32_t *mali_bc_spirv(enum mali_bc_shader s, size_t *words);

#ifdef __cplusplus
}
#endif
#endif
