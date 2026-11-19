/* SPDX-License-Identifier: MIT */
/* M7: descriptor set layouts, pools, sets, updates and templates on the Vulkan runtime, over the Valhall hardware descriptors.
 * Descriptor bytes are the ones libkbase proved on hardware (kb_draw_texture_2d_rgba8, kb_draw_sampler_*, kb_draw_buffer; texture,
 * sampler and buffer descriptors are bit-identical on v11 and v13 as far as t_texture/t_texture_v13 go; `arch` is a parameter for
 * the day they diverge). Image views, samplers and buffers come from M5-style objects through struct mali_desc_res_ops. */
#pragma once
#include "vk_descriptor_set_layout.h"
#include "vk_descriptor_update_template.h"
#include "vk_device.h"
#include "vk_ycbcr_conversion.h"
#include "mali_pipeline_layout_flat.h"

struct mali_hw_desc { uint32_t w[8]; };   /* one 32-byte hardware descriptor */

/* Hardware packers (CPU side, no GPU). */
void mali_pack_buffer(unsigned arch, struct mali_hw_desc *d, uint64_t va, uint32_t size);
void mali_pack_plane_linear(unsigned arch, struct mali_hw_desc *d, uint64_t va, unsigned w, unsigned h, uint32_t row_stride);
void mali_pack_texture_2d_rgba8(unsigned arch, struct mali_hw_desc *d, unsigned w, unsigned h, uint64_t plane_va);
/* wrap: Mali wrap mode (8 repeat, 9 clamp to edge, 12 mirrored repeat); nearest_*: 1 = nearest filter, 0 = linear. */
void mali_pack_sampler(unsigned arch, struct mali_hw_desc *d, int nearest_min, int nearest_mag, unsigned wrap);

/* Resolves API handles to hardware data. Installed by the device/image code (M5/M8); tests install fakes.
 * All return 0 on success. */
struct mali_desc_res_ops {
   int (*image_view)(VkImageView v, VkDescriptorType type, struct mali_hw_desc *out);
   int (*sampler)(VkSampler s, struct mali_hw_desc *out);
   int (*buffer)(VkBuffer b, uint64_t *va, uint64_t *size);           /* base address and size of the whole buffer */
   int (*buffer_view)(VkBufferView v, struct mali_hw_desc *out);      /* texel buffer descriptor */
   int (*sampler_ycbcr)(VkSampler s, struct vk_ycbcr_conversion_state *out); /* conversion of a sampler: 1 and out filled, else 0 (may be NULL in tests) */
   int (*image_view_planes)(VkImageView v, struct mali_hw_desc out[2]); /* NV12 view: luma and chroma descriptors -> 2; else 0 (may be NULL in tests) */
};
void mali_desc_set_res_ops(const struct mali_desc_res_ops *ops);

/* Where pool memory comes from. Default: malloc, va = host pointer (fake backend). M2 installs a GPU-memory allocator. */
struct mali_desc_mem_ops {
   void *(*alloc)(struct vk_device *dev, size_t size, uint64_t *va);
   void (*free)(struct vk_device *dev, void *cpu, size_t size);
};
void mali_desc_set_mem_ops(const struct mali_desc_mem_ops *ops);

struct mali_dsl {
   struct vk_descriptor_set_layout vk;
   uint32_t nbindings, slot_count, dyn_count, imm_count;
   struct mali_lbinding *b;     /* nbindings entries */
   VkSampler *imm;              /* immutable samplers */
   struct vk_ycbcr_conversion_state *ycbcr;   /* conversion of each immutable sampler that has one (index = b->ycbcr_off + element), else NULL */
};
VK_DEFINE_NONDISP_HANDLE_CASTS(mali_dsl, vk.base, VkDescriptorSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT)

struct mali_dyn_buf { uint64_t va, size; };

struct mali_dset {
   struct vk_object_base base;
   struct mali_dsl *layout;
   struct mali_dpool *pool;
   uint8_t *cpu;                 /* slot_count * 32 bytes */
   uint64_t va;
   uint32_t bytes, off;
   struct mali_dset *next;       /* pool list, sorted by offset */
   struct mali_dyn_buf *dyn;     /* dynamic buffers (base address, range); the dynamic offset is added when binding */
   unsigned arch;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(mali_dset, base, VkDescriptorSet, VK_OBJECT_TYPE_DESCRIPTOR_SET)

struct mali_dpool {
   struct vk_object_base base;
   uint8_t *cpu; uint64_t va; uint32_t cap, max_sets, nsets;
   VkDescriptorPoolCreateFlags flags;
   struct mali_dset *sets;
   unsigned arch;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(mali_dpool, base, VkDescriptorPool, VK_OBJECT_TYPE_DESCRIPTOR_POOL)

/* Create the layout. dev->... arch is passed per object; `arch` = 11 or 13. */
VkResult mali_dsl_create(struct vk_device *dev, unsigned arch, const VkDescriptorSetLayoutCreateInfo *ci, struct mali_dsl **out);

/* Pure core, used by the entrypoints below and by tests. */
void mali_dset_write(struct mali_dset *set, const VkWriteDescriptorSet *w);
void mali_dset_copy(struct mali_dset *dst, const VkCopyDescriptorSet *c, struct mali_dset *src);
void mali_dset_update_template(struct mali_dset *set, const struct vk_descriptor_update_template *t, const void *data);
/* Hardware Buffer descriptor of dynamic buffer i of the set with the dynamic offset applied (written by CmdBindDescriptorSets). */
void mali_dyn_buf_pack(unsigned arch, struct mali_hw_desc *d, const struct mali_dyn_buf *b, uint32_t dyn_offset);

/* Entrypoints; M2 wires them into the per-arch dispatch tables (arch comes from the device). */
VkResult mali_create_descriptor_set_layout(struct vk_device *dev, unsigned arch, const VkDescriptorSetLayoutCreateInfo *ci,
                                           VkDescriptorSetLayout *out);
void mali_get_descriptor_set_layout_support(const VkDescriptorSetLayoutCreateInfo *ci, VkDescriptorSetLayoutSupport *s);
VkResult mali_create_descriptor_pool(struct vk_device *dev, unsigned arch, const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDescriptorPool *out);
void mali_destroy_descriptor_pool(struct vk_device *dev, VkDescriptorPool pool, const VkAllocationCallbacks *alloc);
VkResult mali_reset_descriptor_pool(struct vk_device *dev, VkDescriptorPool pool);
VkResult mali_allocate_descriptor_sets(struct vk_device *dev, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out);
VkResult mali_free_descriptor_sets(struct vk_device *dev, VkDescriptorPool pool, uint32_t n, const VkDescriptorSet *sets);
void mali_update_descriptor_sets(struct vk_device *dev, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc,
                                 const VkCopyDescriptorSet *c);
void mali_update_descriptor_set_with_template(struct vk_device *dev, VkDescriptorSet set, VkDescriptorUpdateTemplate t, const void *data);
