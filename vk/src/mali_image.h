/* SPDX-License-Identifier: MIT */
/* M5: images, image views, buffers, buffer views, samplers on the Vulkan runtime (vk_image/vk_image_view/vk_buffer/...), over the Valhall
 * descriptors (texture 32 B + plane payload 32 B per level/layer, sampler 32 B, texel buffer 32 B), all proven on hardware by libkbase
 * t_texture/t_depth (v11) and t_texture_v13. Layout code is pure (no device) so tests can run it offline. */
#ifndef MALI_IMAGE_H
#define MALI_IMAGE_H

#include "mali_afbc.h"
#include "mali_bc.h"
#include "mali_descriptor.h"
#include "mali_format.h"
#include "mali_queue.h"
#include "vk_buffer.h"
#include "vk_buffer_view.h"
#include "vk_image.h"
#include "vk_sampler.h"

/* ---- image layout (pure) ---- */

struct mali_slice {
   uint64_t offset;          /* from the start of the plane's first array layer */
   uint32_t row_stride;      /* bytes per row (linear) / per row of 16x16 tiles (tiled) */
   uint64_t surface_stride;  /* bytes per 2D surface (one z slice or one sample) */
   uint64_t size;            /* surface_stride * depth * samples */
};

struct mali_plane_layout {
   VkImageAspectFlagBits aspect;
   int pipe;                 /* enum pipe_format */
   uint8_t clump, bs, bw, bh;
   uint8_t ssx, ssy;         /* log2 subsampling of the plane extent (NV12 chroma: 1, 1) */
   uint32_t hw;              /* texture pixel format word */
   uint64_t offset;          /* of layer 0 level 0 within the image */
   uint64_t array_stride;
   uint64_t size;            /* array_stride * layers */
   struct mali_slice slice[MALI_MAX_MIPS];
};

struct mali_image_desc {
   VkFormat format;
   VkImageType type;
   VkExtent3D extent;
   uint32_t levels, layers, samples;
   bool tiled;               /* 16x16 u-interleaved instead of linear */
};

struct mali_image_layout {
   struct mali_image_desc d;
   uint8_t nplanes;
   struct mali_plane_layout plane[2];
   uint64_t size;
   uint32_t alignment;
   /* MALI_AFBC=1 only (default off, zero otherwise): an eligible colour image is sized for the AFBC header+body layout
    * (size/alignment above are max(linear, AFBC)). Descriptors and meta paths still use the linear planes: nothing is rendered
    * through AFBC yet and no modifier is exposed. */
   bool afbc;
   struct mali_afbc_image_layout afbc_lay;
};

#define MALI_IMAGE_ALIGN 64u
#define MALI_TILE_EL 16u   /* u-interleaved tile = 16x16 elements */

/* 0, or -EINVAL when the description is out of range (extent, levels, layers, samples) or the format is not advertised. */
int mali_image_layout_init(unsigned arch, const struct mali_image_desc *d, struct mali_image_layout *out);

/* Stage 2 wiring: when `enabled` (MALI_AFBC=1) and the image is AFBC-eligible, grow lay->size/alignment to the AFBC layout and fill
 * lay->afbc*. Returns true when applied. With !enabled it never touches `lay` (default path byte-identical). */
int mali_image_ci_layout(unsigned arch, const VkImageCreateInfo *ci, struct mali_image_layout *lay, struct mali_bc_layout *bc,
                         struct mali_image_layout *sh, bool *is_bc);
/* Header init at bind: for an AFBC image zero the headers inside the bound memory (cpu = BO mapping, cpu_size its size, off the bind offset).
 * Returns bytes zeroed, 0 for linear images or when the image does not fit. */
uint64_t mali_image_afbc_bind_init(const struct mali_image_layout *lay, void *cpu, uint64_t cpu_size, uint64_t off);
int mali_image_plain_layout(unsigned arch, const VkImageCreateInfo *ci, const struct mali_image_desc *d, bool afbc_on, struct mali_image_layout *lay);
bool mali_image_afbc_apply(unsigned arch, const VkImageCreateInfo *ci, bool enabled, struct mali_image_layout *lay);
/* MALI_IMAGE_TILED=1 selects u-interleaved for OPTIMAL images (single sampled, not 1D). Default: linear everywhere (tiled is not
 * hardware-verified yet, see R3-PLAN M5). */
bool mali_image_wants_tiled(VkImageTiling tiling, VkImageType type, uint32_t samples);

const struct mali_plane_layout *mali_image_plane_for_aspect(const struct mali_image_layout *l, VkImageAspectFlags aspect);
/* Byte offset of (level, layer) of the plane within the image, and strides (z/sample surface stride from the slice). */
uint64_t mali_image_subresource_offset(const struct mali_plane_layout *p, unsigned level, unsigned layer);

/* ---- hardware packers (pure) ---- */

/* Generic Plane descriptor. ordering: 1 tiled u-interleaved, 2 linear. slice_stride: z/sample stride (0 for 2D arrays/1 layer). */
void mali_pack_plane(struct mali_hw_desc *d, unsigned ordering, unsigned clump, uint64_t va, uint64_t size, uint32_t row_stride,
                     uint64_t slice_stride, unsigned w, unsigned h);

struct mali_view_desc {
   VkFormat format;
   VkImageViewType type;
   VkImageAspectFlags aspect;
   uint32_t base_level, levels, base_layer, layers;
   VkComponentMapping swizzle;     /* components already resolved (no IDENTITY) */
   float min_lod;
   bool storage;                   /* storage image use: 1 level, identity swizzle, LOD 0 */
};

/* Number of 32 B payload entries (levels * layers; 3D images: one entry per level unless viewed as 2D / 2D array). */
unsigned mali_view_payload_entries(const struct mali_image_layout *l, const struct mali_view_desc *v);
/* Fill `payload` (entries * 32 B) for an image whose memory starts at `image_va`, and the texture descriptor pointing at `payload_va`. */
int mali_view_emit(unsigned arch, const struct mali_image_layout *l, uint64_t image_va, const struct mali_view_desc *v, void *payload,
                   uint64_t payload_va, struct mali_hw_desc *tex);

/* Texel buffer (Buffer descriptor, type Structure) for format f over [va, va+range). */
void mali_pack_texel_buffer(struct mali_hw_desc *d, const struct mali_fmt *f, uint64_t va, uint64_t range);

/* Sampler descriptor from a runtime sampler (pure given the vk_sampler fields). */
void mali_border_color_unswizzle(const VkComponentMapping *m, const uint32_t want[4], uint32_t out[4]);
void mali_sampler_pack(unsigned arch, const VkSamplerCreateInfo *ci, const struct vk_sampler *s, struct mali_hw_desc *out);
uint32_t mali_wrap_mode(VkSamplerAddressMode m);

/* ---- objects ---- */

extern const uint64_t mali_wsi_modifier;   /* DRM_FORMAT_MOD_LINEAR (mali_wsi.c) */

struct mali_image {
   struct vk_image vk;
   struct mali_image_layout lay;
   struct mali_memory *mem;
   uint64_t mem_off;
   uint64_t va;                  /* bound GPU address, 0 when unbound */
   unsigned arch;
   /* R4-M7 BC emulation: `lay` describes the compressed region (planes taken from bc_lay.comp), views are built over sh_lay (decoded shadow) */
   bool bc;
   bool native_view;   /* ETC2 / EAC: `bc` marks the decoded blit-source shadow, but views and descriptors use the native `lay` */
   struct mali_bc_layout bc_lay;
   struct mali_image_layout sh_lay;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(mali_image, vk.base, VkImage, VK_OBJECT_TYPE_IMAGE)

struct mali_image_view {
   struct vk_image_view vk;
   unsigned arch;
   struct mali_view_desc vd;
   struct mali_hw_desc tex, storage;
   /* YCbCr (NV12) view: tex is the luma plane (R8), tex_c the chroma plane (R8G8), each with its own payload (payload_c). */
   bool ycbcr;
   struct mali_hw_desc tex_c;
   void *payload_c;
   uint64_t payload_c_va;
   uint32_t payload_c_entries;
   void *payload;
   uint64_t payload_va;
   uint32_t payload_entries;
   uint64_t emitted_for_va;      /* image va the payload was emitted for (re-emitted when the image gets bound/rebound) */
   bool emitted;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(mali_image_view, vk.base, VkImageView, VK_OBJECT_TYPE_IMAGE_VIEW)

struct mali_buffer {
   struct vk_buffer vk;
   struct mali_memory *mem;
   uint64_t mem_off;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(mali_buffer, vk.base, VkBuffer, VK_OBJECT_TYPE_BUFFER)

struct mali_buffer_view {
   struct vk_buffer_view vk;
   struct mali_fmt fmt;
   struct mali_hw_desc desc;
   bool emitted;
   uint64_t emitted_for_va;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(mali_buffer_view, vk.base, VkBufferView, VK_OBJECT_TYPE_BUFFER_VIEW)

struct mali_sampler {
   struct vk_sampler vk;
   struct mali_hw_desc hw;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(mali_sampler, vk.base, VkSampler, VK_OBJECT_TYPE_SAMPLER)

/* GPU memory for image view payloads: same shape as struct mali_desc_mem_ops (M2/M4 installs a BO sub-allocator; default malloc, va = host
 * pointer, for the fake backend and tests). */
void mali_image_set_mem_ops(const struct mali_desc_mem_ops *ops);

/* Resolvers for the descriptor code (struct mali_desc_res_ops): install once with mali_desc_set_res_ops(&mali_res_ops). */
extern const struct mali_desc_res_ops mali_res_ops;
int mali_image_view_desc(VkImageView v, VkDescriptorType type, struct mali_hw_desc *out);
/* NV12 view: the two plane descriptors (luma, chroma) -> 2; any other view -> 0 and out is untouched. */
int mali_image_view_planes(VkImageView v, struct mali_hw_desc out[2]);
int mali_buffer_view_desc(VkBufferView v, struct mali_hw_desc *out);

/* Entrypoints (device level). */
VKAPI_ATTR VkResult VKAPI_CALL mali_CreateBuffer(VkDevice, const VkBufferCreateInfo *, const VkAllocationCallbacks *, VkBuffer *);
VKAPI_ATTR void VKAPI_CALL mali_DestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks *);
VKAPI_ATTR void VKAPI_CALL mali_GetDeviceBufferMemoryRequirements(VkDevice, const VkDeviceBufferMemoryRequirements *, VkMemoryRequirements2 *);
VKAPI_ATTR VkResult VKAPI_CALL mali_BindBufferMemory2(VkDevice, uint32_t, const VkBindBufferMemoryInfo *);
VKAPI_ATTR VkResult VKAPI_CALL mali_CreateBufferView(VkDevice, const VkBufferViewCreateInfo *, const VkAllocationCallbacks *, VkBufferView *);
VKAPI_ATTR void VKAPI_CALL mali_DestroyBufferView(VkDevice, VkBufferView, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL mali_CreateImage(VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *, VkImage *);
VKAPI_ATTR void VKAPI_CALL mali_DestroyImage(VkDevice, VkImage, const VkAllocationCallbacks *);
VKAPI_ATTR void VKAPI_CALL mali_GetImageMemoryRequirements2(VkDevice, const VkImageMemoryRequirementsInfo2 *, VkMemoryRequirements2 *);
VKAPI_ATTR void VKAPI_CALL mali_GetDeviceImageMemoryRequirements(VkDevice, const VkDeviceImageMemoryRequirements *, VkMemoryRequirements2 *);
VKAPI_ATTR VkResult VKAPI_CALL mali_BindImageMemory2(VkDevice, uint32_t, const VkBindImageMemoryInfo *);
VKAPI_ATTR void VKAPI_CALL mali_GetImageSubresourceLayout2(VkDevice, VkImage, const VkImageSubresource2 *, VkSubresourceLayout2 *);
VKAPI_ATTR void VKAPI_CALL mali_GetDeviceImageSubresourceLayout(VkDevice, const VkDeviceImageSubresourceInfo *, VkSubresourceLayout2 *);
VKAPI_ATTR void VKAPI_CALL mali_GetImageSparseMemoryRequirements2(VkDevice, const VkImageSparseMemoryRequirementsInfo2 *, uint32_t *,
                                                                  VkSparseImageMemoryRequirements2 *);
VKAPI_ATTR VkResult VKAPI_CALL mali_CreateImageView(VkDevice, const VkImageViewCreateInfo *, const VkAllocationCallbacks *, VkImageView *);
VKAPI_ATTR void VKAPI_CALL mali_DestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL mali_CreateSampler(VkDevice, const VkSamplerCreateInfo *, const VkAllocationCallbacks *, VkSampler *);
VKAPI_ATTR void VKAPI_CALL mali_DestroySampler(VkDevice, VkSampler, const VkAllocationCallbacks *);

#endif
