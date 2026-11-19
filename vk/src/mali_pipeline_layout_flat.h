/* SPDX-License-Identifier: MIT */
/* Plain-C (no Mesa/Vulkan headers) description of a pipeline layout as the shader compiler needs it.
 * Produced from a vk_pipeline_layout by mali_pipeline_layout_flatten() (mali_pipeline_layout.h), or built by hand in tests.
 *
 * Valhall resource model (v11 and v13 alike):
 *   resource table 0     = driver table: [0..15] VS attributes, [17..] vertex buffers, dummy sampler at
 *                          MALI_DUMMY_SAMPLER_IDX; resource table 9 = dynamic UBO/SSBO descriptors (MALI_DYN_BUF_START + dyn_idx)
 *   resource table n+1   = user descriptor set n (array of 32-byte descriptors); handle = pan_res_handle(table, index)
 *   push constants       = FAU words (mali_compile push_base)
 */
#pragma once
#include <stdint.h>

#define MALI_MAX_SETS            8u     /* tables 1..8 (the hardware has 16; 7 are kept for the driver) */
#define MALI_DESC_SLOT_SIZE      32u    /* one hardware descriptor (texture, sampler, buffer) */
#define MALI_DUMMY_SAMPLER_IDX   16u    /* table 0: all-zero sampler for texelFetch / sampler-less sampling (between the attributes 0..15 and the vertex buffers 17..) */
#define MALI_DYN_TABLE           9u     /* resource table 9 (user sets use 1..8): the dynamic-buffer Buffer descriptors, a small table of its own */
#define MALI_DYN_BUF_START       0u     /* index of the first dynamic-buffer descriptor in table MALI_DYN_TABLE */
#define MALI_MAX_PUSH_BYTES      128u
#define MALI_NO_IMM_SAMPLER      0xffffffffu
/* Plain-C copy of VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT (same value), so the compiler-side lowering needs no Vulkan header. */
#define MALI_DESC_BINDING_PARTIALLY_BOUND 0x00000004u
/* All-zero descriptor in resource table 0: the hardware Null Descriptor (reads return zero, writes are dropped). Used for an
 * out-of-range index into a partially-bound array; same slot the sampler-less path uses for its dummy sampler. */
#define MALI_NULL_DESC_IDX       MALI_DUMMY_SAMPLER_IDX

/* Values equal VkDescriptorType (so no Vulkan header is needed here). */
enum mali_desc_type {
   MALI_DT_SAMPLER = 0, MALI_DT_COMBINED_IMAGE_SAMPLER = 1, MALI_DT_SAMPLED_IMAGE = 2, MALI_DT_STORAGE_IMAGE = 3,
   MALI_DT_UNIFORM_TEXEL_BUFFER = 4, MALI_DT_STORAGE_TEXEL_BUFFER = 5, MALI_DT_UNIFORM_BUFFER = 6, MALI_DT_STORAGE_BUFFER = 7,
   MALI_DT_UNIFORM_BUFFER_DYNAMIC = 8, MALI_DT_STORAGE_BUFFER_DYNAMIC = 9, MALI_DT_INPUT_ATTACHMENT = 10,
   MALI_DT_NONE = 0xffffffffu,
};

struct mali_lbinding {
   uint32_t type;      /* enum mali_desc_type, MALI_DT_NONE = binding number unused */
   uint32_t count;     /* array size */
   uint32_t slot;      /* first 32-byte descriptor of element 0 in the set (non-dynamic) */
   uint32_t stride;    /* 32-byte descriptors per array element: combined image sampler 2 (texture, sampler), else 1; dynamic 0 */
   uint32_t dyn_idx;   /* dynamic buffers: index of element 0 among the set's dynamic buffers */
   uint32_t imm_off;   /* index of the first immutable sampler, MALI_NO_IMM_SAMPLER if none */
   uint32_t ycbcr_planes; /* combined image sampler whose immutable samplers carry a YCbCr conversion: 2 (stride 3: luma, chroma, sampler); else 0 */
   uint32_t ycbcr_off;    /* index of element 0's conversion in the set's ycbcr array (mali_lset::ycbcr) */
   uint32_t inline_bytes; /* inline uniform block: data size in bytes (type = UNIFORM_BUFFER, count 1, stride 1 + data slots); else 0 */
   uint32_t bflags;       /* VkDescriptorBindingFlags of the binding; the lowering reads PARTIALLY_BOUND for OOB -> null */
};

struct mali_lset {
   uint32_t nbindings;               /* highest binding number + 1 */
   uint32_t slot_count;              /* descriptors in the set */
   uint32_t dyn_count;               /* dynamic buffers in the set */
   const struct mali_lbinding *b;    /* nbindings entries */
   const void *ycbcr;                /* struct vk_ycbcr_conversion_state[] of the immutable YCbCr samplers (opaque here), NULL if none */
};

struct mali_compile_layout {
   uint32_t nsets;
   struct mali_lset set[MALI_MAX_SETS];
   uint32_t dyn_start[MALI_MAX_SETS]; /* running index of the set's first dynamic buffer in the pipeline's dynamic-buffer array */
   uint32_t push_size;                /* bytes of push constants (<= MALI_MAX_PUSH_BYTES) */
   uint32_t robust_access;            /* robustBufferAccess2/robustImageAccess2 enabled: bounds-check buffer/image accesses (nir_lower_robust_access); 0 = off */
};

static inline uint32_t mali_desc_stride(uint32_t type)
{
   switch (type) {
   case MALI_DT_COMBINED_IMAGE_SAMPLER: return 2;
   case MALI_DT_UNIFORM_BUFFER_DYNAMIC:
   case MALI_DT_STORAGE_BUFFER_DYNAMIC: return 0;
   default: return 1;
   }
}
