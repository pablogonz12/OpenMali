/* SPDX-License-Identifier: MIT */
/* M7 descriptor sets over the Vulkan runtime. See mali_descriptor.h. */
#include "mali_descriptor.h"
#include "mali_format.h"

#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_util.h"

#include <stdlib.h>
#include <string.h>

/* ---- hardware descriptor packers: same fields as libkbase/src/draw.c (hardware-proven on v11 and v13) ---- */

#define PIXFMT_RGBA8 (0u | (187u << 12))   /* pan format: RGB-like, RGBA8 UNORM, identity swizzle (see libkbase KB_PIXFMT_RGBA8) */

void
mali_pack_buffer(unsigned arch, struct mali_hw_desc *d, uint64_t va, uint32_t size)
{
   memset(d, 0, sizeof *d);
   d->w[0] = 9u | (1u << 4);   /* type Buffer, size in bytes */
   d->w[1] = size;
   d->w[2] = (uint32_t)va;
   d->w[3] = (uint32_t)(va >> 32);
}

void
mali_pack_plane_linear(unsigned arch, struct mali_hw_desc *d, uint64_t va, unsigned w, unsigned h, uint32_t row_stride)
{
   memset(d, 0, sizeof *d);
   d->w[0] = 10u | (1u << 4) | (2u << 8) | (2u << 24);   /* Plane, generic, linear clump order, RAW32 */
   d->w[1] = row_stride * h;
   d->w[2] = (uint32_t)va;
   d->w[3] = (uint32_t)(va >> 32) & 0xffffu;
   d->w[4] = row_stride;
   d->w[7] = (w - 1) | ((h - 1) << 16);
}

void
mali_pack_texture_2d_rgba8(unsigned arch, struct mali_hw_desc *d, unsigned w, unsigned h, uint64_t plane_va)
{
   memset(d, 0, sizeof *d);
   d->w[0] = 2u | (2u << 4) | (PIXFMT_RGBA8 << 10);   /* Texture, 2D, format */
   d->w[1] = (w - 1) | ((h - 1) << 16);
   d->w[2] = 0x688u;                                  /* identity RGBA swizzle, 1 level */
   d->w[4] = (uint32_t)plane_va;
   d->w[5] = (uint32_t)(plane_va >> 32);
}

void
mali_pack_sampler(unsigned arch, struct mali_hw_desc *d, int nearest_min, int nearest_mag, unsigned wrap)
{
   memset(d, 0, sizeof *d);
   wrap &= 15u;
   d->w[0] = 1u | (wrap << 8) | (wrap << 12) | (wrap << 16) | (1u << 23) | (1u << 25);
   if (nearest_min) d->w[0] |= 1u << 27;
   if (nearest_mag) d->w[0] |= 1u << 28;
}

/* ---- seams ---- */

static const struct mali_desc_res_ops *g_res;
static const struct mali_desc_mem_ops *g_mem;

void mali_desc_set_res_ops(const struct mali_desc_res_ops *ops) { g_res = ops; }
void mali_desc_set_mem_ops(const struct mali_desc_mem_ops *ops) { g_mem = ops; }

static void *
mem_alloc(struct vk_device *dev, size_t size, uint64_t *va)
{
   if (g_mem) return g_mem->alloc(dev, size, va);
   void *p = NULL;
   if (posix_memalign(&p, 64, size ? size : 64)) return NULL;
   memset(p, 0, size);
   *va = (uint64_t)(uintptr_t)p;
   return p;
}

static void
mem_free(struct vk_device *dev, void *p, size_t size)
{
   if (g_mem) g_mem->free(dev, p, size);
   else free(p);
}

/* ---- set layout ---- */

static bool
type_ok(VkDescriptorType t)
{
   return t <= VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT || t == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK;
}

/* inlineUniformBlock: descriptorCount is a byte size (multiple of 4, <= MALI_MAX_INLINE_BYTES). The binding is one UBO descriptor
 * (slot) pointing at the data stored in the following slots of the set memory. */
#define MALI_MAX_INLINE_BYTES 256u
static inline uint32_t inline_slots(uint32_t bytes) { return (bytes + MALI_DESC_SLOT_SIZE - 1) / MALI_DESC_SLOT_SIZE; }

static void
dsl_destroy(struct vk_device *dev, struct vk_descriptor_set_layout *l)
{
   struct mali_dsl *d = container_of(l, struct mali_dsl, vk);
   vk_free(&dev->alloc, d->b);
   vk_free(&dev->alloc, d->imm);
   vk_free(&dev->alloc, d->ycbcr);
   vk_object_free(dev, NULL, d);
}

VkResult
mali_dsl_create(struct vk_device *dev, unsigned arch, const VkDescriptorSetLayoutCreateInfo *ci, struct mali_dsl **out)
{
   uint32_t nb = 0, nimm = 0, nycbcr = 0;
   for (uint32_t i = 0; i < ci->bindingCount; i++) {
      const VkDescriptorSetLayoutBinding *b = &ci->pBindings[i];
      if (!type_ok(b->descriptorType)) return VK_ERROR_FEATURE_NOT_PRESENT;   /* inline uniform, accel structs: not supported */
      nb = MAX2(nb, b->binding + 1);
      if (b->pImmutableSamplers) nimm += b->descriptorCount;
   }
   struct mali_dsl *d = vk_descriptor_set_layout_zalloc(dev, sizeof *d, ci);
   if (!d) return VK_ERROR_OUT_OF_HOST_MEMORY;
   d->vk.destroy = dsl_destroy;
   d->b = vk_zalloc(&dev->alloc, (nb ? nb : 1) * sizeof *d->b, 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   d->imm = vk_zalloc(&dev->alloc, (nimm ? nimm : 1) * sizeof *d->imm, 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   d->ycbcr = vk_zalloc(&dev->alloc, (nimm ? nimm : 1) * sizeof *d->ycbcr, 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!d->b || !d->imm || !d->ycbcr) {
      dsl_destroy(dev, &d->vk);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   d->nbindings = nb;
   for (uint32_t i = 0; i < nb; i++)
      d->b[i].type = MALI_DT_NONE;
   /* Slots are assigned in binding-number order so that the layout does not depend on the order in pBindings. */
   for (uint32_t n = 0; n < nb; n++) {
      const VkDescriptorSetLayoutBinding *src = NULL;
      for (uint32_t i = 0; i < ci->bindingCount; i++)
         if (ci->pBindings[i].binding == n) src = &ci->pBindings[i];
      if (!src) continue;
      struct mali_lbinding *b = &d->b[n];
      b->type = src->descriptorType;
      b->count = src->descriptorCount;
      b->stride = mali_desc_stride(b->type);
      if (b->type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
         if (src->descriptorCount > MALI_MAX_INLINE_BYTES || (src->descriptorCount & 3)) {
            dsl_destroy(dev, &d->vk);
            return VK_ERROR_FEATURE_NOT_PRESENT;
         }
         b->type = MALI_DT_UNIFORM_BUFFER;
         b->inline_bytes = src->descriptorCount;
         b->count = 1;
         b->stride = 1 + inline_slots(src->descriptorCount);
      }
      b->imm_off = MALI_NO_IMM_SAMPLER;
      /* Immutable samplers with a YCbCr conversion: the element holds one texture descriptor per image plane, then the sampler. */
      if (b->type == MALI_DT_COMBINED_IMAGE_SAMPLER && src->pImmutableSamplers && g_res && g_res->sampler_ycbcr) {
         uint32_t first = nycbcr;
         bool any = false;
         for (uint32_t k = 0; k < b->count; k++) {
            struct vk_ycbcr_conversion_state st;
            memset(&st, 0, sizeof st);
            any |= g_res->sampler_ycbcr(src->pImmutableSamplers[k], &st) != 0;
            d->ycbcr[nycbcr++] = st;
         }
         if (any) {
            b->ycbcr_planes = MALI_YCBCR_PLANES;
            b->ycbcr_off = first;
            b->stride = MALI_YCBCR_SLOTS;
         } else {
            nycbcr = first;
            memset(&d->ycbcr[first], 0, b->count * sizeof *d->ycbcr);
         }
      }
      if (b->stride) {
         b->slot = d->slot_count;
         d->slot_count += b->stride * b->count;
      } else {
         b->dyn_idx = d->dyn_count;
         d->dyn_count += b->count;
      }
      if (src->pImmutableSamplers) {
         b->imm_off = d->imm_count;
         for (uint32_t k = 0; k < b->count; k++)
            d->imm[d->imm_count++] = src->pImmutableSamplers[k];
      }
   }
   d->vk.dynamic_descriptor_count = d->dyn_count;
   /* hash for the pipeline cache: everything that changes the lowering */
   blake3_hasher h;
   blake3_hasher_init(&h);
   blake3_hasher_update(&h, d->b, nb * sizeof *d->b);
   blake3_hasher_update(&h, d->ycbcr, nycbcr * sizeof *d->ycbcr);   /* model, range, siting, swizzle change the shader */
   blake3_hasher_finalize(&h, d->vk.blake3, sizeof d->vk.blake3);
   *out = d;
   return VK_SUCCESS;
}

VkResult
mali_create_descriptor_set_layout(struct vk_device *dev, unsigned arch, const VkDescriptorSetLayoutCreateInfo *ci,
                                  VkDescriptorSetLayout *out)
{
   struct mali_dsl *d;
   VkResult r = mali_dsl_create(dev, arch, ci, &d);
   if (r == VK_SUCCESS) *out = mali_dsl_to_handle(d);
   return r;
}

void
mali_get_descriptor_set_layout_support(const VkDescriptorSetLayoutCreateInfo *ci, VkDescriptorSetLayoutSupport *s)
{
   bool ok = true;
   uint64_t slots = 0;
   for (uint32_t i = 0; i < ci->bindingCount; i++) {
      const VkDescriptorSetLayoutBinding *b = &ci->pBindings[i];
      if (!type_ok(b->descriptorType)) ok = false;
      else if (b->descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK)
         ok &= b->descriptorCount <= MALI_MAX_INLINE_BYTES && !(b->descriptorCount & 3);
      else slots += (uint64_t)mali_desc_stride(b->descriptorType) * b->descriptorCount;
   }
   s->supported = ok && slots < (1u << 20);
}

/* ---- pools and sets ---- */

VkResult
mali_create_descriptor_pool(struct vk_device *dev, unsigned arch, const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDescriptorPool *out)
{
   uint64_t slots = 0;
   for (uint32_t i = 0; i < ci->poolSizeCount; i++) {
      const VkDescriptorPoolSize *p = &ci->pPoolSizes[i];
      if (!type_ok(p->type)) return VK_ERROR_FEATURE_NOT_PRESENT;
      if (p->type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) slots += inline_slots(p->descriptorCount);
      else slots += (uint64_t)MAX2(mali_desc_stride(p->type), 0u) * p->descriptorCount;
   }
   const VkDescriptorPoolInlineUniformBlockCreateInfo *pib =
      vk_find_struct_const(ci->pNext, DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO);
   if (pib) slots += pib->maxInlineUniformBlockBindings * 2ull;   /* descriptor slot + alignment slack per binding */
   uint64_t bytes = slots * MALI_DESC_SLOT_SIZE;
   if (bytes > (1u << 30)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   struct mali_dpool *p = vk_object_zalloc(dev, alloc, sizeof *p, VK_OBJECT_TYPE_DESCRIPTOR_POOL);
   if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
   p->cap = (uint32_t)bytes;
   p->max_sets = ci->maxSets;
   p->flags = ci->flags;
   p->arch = arch;
   if (p->cap) {
      p->cpu = mem_alloc(dev, p->cap, &p->va);
      if (!p->cpu) {
         vk_object_free(dev, alloc, p);
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      }
   }
   *out = mali_dpool_to_handle(p);
   return VK_SUCCESS;
}

static void
dset_free(struct vk_device *dev, struct mali_dset *s)
{
   vk_free(&dev->alloc, s->dyn);
   vk_descriptor_set_layout_unref(dev, &s->layout->vk);
   vk_object_free(dev, NULL, s);
}

static void
pool_clear(struct vk_device *dev, struct mali_dpool *p)
{
   while (p->sets) {
      struct mali_dset *n = p->sets->next;
      dset_free(dev, p->sets);
      p->sets = n;
   }
   p->nsets = 0;
}

void
mali_destroy_descriptor_pool(struct vk_device *dev, VkDescriptorPool h, const VkAllocationCallbacks *alloc)
{
   struct mali_dpool *p = mali_dpool_from_handle(h);
   if (!p) return;
   pool_clear(dev, p);
   if (p->cpu) mem_free(dev, p->cpu, p->cap);
   vk_object_free(dev, alloc, p);
}

VkResult
mali_reset_descriptor_pool(struct vk_device *dev, VkDescriptorPool h)
{
   struct mali_dpool *p = mali_dpool_from_handle(h);
   pool_clear(dev, p);
   if (p->cpu) memset(p->cpu, 0, p->cap);
   return VK_SUCCESS;
}

static void
write_immutable(struct mali_dset *s)
{
   const struct mali_dsl *l = s->layout;
   for (uint32_t n = 0; n < l->nbindings; n++) {
      const struct mali_lbinding *b = &l->b[n];
      if (b->type == MALI_DT_NONE || b->imm_off == MALI_NO_IMM_SAMPLER || !g_res) continue;
      for (uint32_t k = 0; k < b->count; k++) {
         unsigned sub = b->type == MALI_DT_COMBINED_IMAGE_SAMPLER ? (b->ycbcr_planes ? b->ycbcr_planes : 1) : 0;
         struct mali_hw_desc hw;
         if (g_res->sampler(l->imm[b->imm_off + k], &hw) == 0)
            memcpy(s->cpu + (size_t)(b->slot + k * b->stride + sub) * MALI_DESC_SLOT_SIZE, &hw, sizeof hw);
      }
   }
}

/* first fit between the sets (list sorted by offset) */
static VkResult
alloc_set(struct vk_device *dev, struct mali_dpool *p, struct mali_dsl *l, struct mali_dset **out)
{
   uint32_t bytes = l->slot_count * MALI_DESC_SLOT_SIZE;
   if (p->nsets >= p->max_sets) return VK_ERROR_OUT_OF_POOL_MEMORY;
   struct mali_dset **link = &p->sets;
   uint32_t at = 0;
   while (*link && (*link)->off - at < bytes) {
      at = (*link)->off + (*link)->bytes;
      link = &(*link)->next;
   }
   uint32_t end = *link ? (*link)->off : p->cap;
   if (end < at || end - at < bytes) return VK_ERROR_OUT_OF_POOL_MEMORY;
   struct mali_dset *s = vk_object_zalloc(dev, NULL, sizeof *s, VK_OBJECT_TYPE_DESCRIPTOR_SET);
   if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (l->dyn_count) {
      s->dyn = vk_zalloc(&dev->alloc, l->dyn_count * sizeof *s->dyn, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!s->dyn) {
         vk_object_free(dev, NULL, s);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }
   s->layout = (struct mali_dsl *)vk_descriptor_set_layout_ref(&l->vk);
   s->pool = p;
   s->off = at;
   s->bytes = bytes;
   s->cpu = p->cpu + at;
   s->va = p->va + at;
   s->arch = p->arch;
   if (bytes) memset(s->cpu, 0, bytes);   /* zero-capacity pool: s->cpu is NULL (memset(NULL, 0, 0) is UB) */
   write_immutable(s);
   for (uint32_t n = 0; n < l->nbindings; n++) {
      const struct mali_lbinding *b = &l->b[n];
      if (b->type == MALI_DT_NONE || !b->inline_bytes) continue;
      struct mali_hw_desc hw;
      mali_pack_buffer(p->arch, &hw, s->va + (uint64_t)(b->slot + 1) * MALI_DESC_SLOT_SIZE, b->inline_bytes);
      memcpy(s->cpu + (size_t)b->slot * MALI_DESC_SLOT_SIZE, &hw, sizeof hw);
   }
   s->next = *link;
   *link = s;
   p->nsets++;
   *out = s;
   return VK_SUCCESS;
}

VkResult
mali_allocate_descriptor_sets(struct vk_device *dev, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out)
{
   struct mali_dpool *p = mali_dpool_from_handle(ai->descriptorPool);
   uint32_t i;
   VkResult r = VK_SUCCESS;
   for (i = 0; i < ai->descriptorSetCount; i++) {
      struct mali_dset *s;
      r = alloc_set(dev, p, mali_dsl_from_handle(ai->pSetLayouts[i]), &s);
      if (r != VK_SUCCESS) break;
      out[i] = mali_dset_to_handle(s);
   }
   if (r != VK_SUCCESS) {   /* all or nothing; failed sets become VK_NULL_HANDLE */
      mali_free_descriptor_sets(dev, ai->descriptorPool, i, out);
      for (uint32_t k = 0; k < ai->descriptorSetCount; k++) out[k] = VK_NULL_HANDLE;
   }
   return r;
}

VkResult
mali_free_descriptor_sets(struct vk_device *dev, VkDescriptorPool h, uint32_t n, const VkDescriptorSet *sets)
{
   struct mali_dpool *p = mali_dpool_from_handle(h);
   for (uint32_t i = 0; i < n; i++) {
      struct mali_dset *s = mali_dset_from_handle(sets[i]);
      if (!s) continue;
      struct mali_dset **l = &p->sets;
      while (*l && *l != s) l = &(*l)->next;
      if (!*l) continue;   /* not a live set of this pool (already freed, or another pool's): nothing to unlink or free */
      *l = s->next;
      p->nsets--;
      dset_free(dev, s);
   }
   return VK_SUCCESS;
}

/* ---- writes ---- */

static struct mali_hw_desc *
slot_at(struct mali_dset *s, uint32_t slot)
{
   return (struct mali_hw_desc *)(s->cpu + (size_t)slot * MALI_DESC_SLOT_SIZE);
}

/* write one element `idx` (already resolved to binding b) */
static void
write_one(struct mali_dset *s, const struct mali_lbinding *b, uint32_t idx, VkDescriptorType type, const VkDescriptorImageInfo *ii,
          const VkDescriptorBufferInfo *bi, const VkBufferView *bv)
{
   if (!g_res) return;
   struct mali_hw_desc hw;
   /* robustness2 nullDescriptor: a VK_NULL_HANDLE resource writes the hardware Null Descriptor (Type field = 0, i.e. all zero;
    * Valhall v10+ genxml "Null Descriptor", 8 bytes). Reads then return zero and writes are dropped by the hardware. */
   struct mali_hw_desc null_hw;
   memset(&null_hw, 0, sizeof null_hw);
   uint32_t base = b->slot + idx * b->stride;
   switch (type) {
   case VK_DESCRIPTOR_TYPE_SAMPLER:
      if (b->imm_off == MALI_NO_IMM_SAMPLER) {
         if (ii->sampler && g_res->sampler(ii->sampler, &hw) == 0) *slot_at(s, base) = hw;
         else if (!ii->sampler) *slot_at(s, base) = null_hw;
      }
      break;
   case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      if (b->ycbcr_planes) {   /* [luma texture, chroma texture, sampler]; the sampler is immutable (written at allocation) */
         struct mali_hw_desc pl[2];
         if (!ii->imageView) {
            for (unsigned k = 0; k < b->ycbcr_planes; k++) *slot_at(s, base + k) = null_hw;
         } else if (g_res->image_view_planes && g_res->image_view_planes(ii->imageView, pl) == (int)b->ycbcr_planes) {
            for (unsigned k = 0; k < b->ycbcr_planes; k++) *slot_at(s, base + k) = pl[k];
         }
         break;
      }
      if (ii->imageView && g_res->image_view(ii->imageView, type, &hw) == 0)
         *slot_at(s, base) = hw;
      else if (!ii->imageView)
         *slot_at(s, base) = null_hw;
      if (b->imm_off == MALI_NO_IMM_SAMPLER && ii->sampler && g_res->sampler(ii->sampler, &hw) == 0)
         *slot_at(s, base + 1) = hw;
      break;
   case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
   case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
   case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
      if (!ii->imageView)
         *slot_at(s, base) = null_hw;
      else if (g_res->image_view(ii->imageView, type, &hw) == 0)
         *slot_at(s, base) = hw;
      break;
   case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
      if (!*bv)
         *slot_at(s, base) = null_hw;
      else if (g_res->buffer_view(*bv, &hw) == 0)
         *slot_at(s, base) = hw;
      break;
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
      uint64_t va = 0, size = 0;
      if (!bi->buffer) {
         if (b->stride) *slot_at(s, b->slot + idx) = null_hw;
         else s->dyn[b->dyn_idx + idx] = (struct mali_dyn_buf){ 0, 0 };
      } else if (g_res->buffer(bi->buffer, &va, &size) == 0) {
         uint64_t range = bi->range == VK_WHOLE_SIZE ? size - bi->offset : bi->range;
         va += bi->offset;
         if (b->stride) {
            mali_pack_buffer(s->arch, &hw, va, (uint32_t)range);
            *slot_at(s, b->slot + idx) = hw;
         } else {
            s->dyn[b->dyn_idx + idx] = (struct mali_dyn_buf){ va, range };
         }
      }
      break;
   }
   default: break;
   }
}

void
mali_dset_write(struct mali_dset *s, const VkWriteDescriptorSet *w)
{
   const struct mali_dsl *l = s->layout;
   uint32_t binding = w->dstBinding, elem = w->dstArrayElement;
   if (w->descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
      const VkWriteDescriptorSetInlineUniformBlock *ib = vk_find_struct_const(w->pNext, WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK);
      if (!ib || binding >= l->nbindings || !l->b[binding].inline_bytes) return;
      const struct mali_lbinding *b = &l->b[binding];
      if (elem > b->inline_bytes || w->descriptorCount > b->inline_bytes - elem) return;
      memcpy(s->cpu + (size_t)(b->slot + 1) * MALI_DESC_SLOT_SIZE + elem, ib->pData, w->descriptorCount);
      return;
   }
   for (uint32_t i = 0; i < w->descriptorCount; i++) {
      /* spec: writes spill into the following bindings */
      while (binding < l->nbindings && elem >= l->b[binding].count && l->b[binding].type != MALI_DT_NONE) {
         elem -= l->b[binding].count;
         binding++;
      }
      if (binding >= l->nbindings) return;
      const struct mali_lbinding *b = &l->b[binding];
      if (b->type == MALI_DT_NONE) return;
      write_one(s, b, elem, w->descriptorType,
                w->pImageInfo ? &w->pImageInfo[i] : NULL, w->pBufferInfo ? &w->pBufferInfo[i] : NULL,
                w->pTexelBufferView ? &w->pTexelBufferView[i] : NULL);
      elem++;
   }
}

void
mali_dset_copy(struct mali_dset *dst, const VkCopyDescriptorSet *c, struct mali_dset *src)
{
   const struct mali_lbinding *sb = &src->layout->b[c->srcBinding], *db = &dst->layout->b[c->dstBinding];
   if (sb->inline_bytes || db->inline_bytes) {   /* inline block: dstArrayElement/srcArrayElement are byte offsets, descriptorCount bytes */
      if (!sb->inline_bytes || !db->inline_bytes || c->srcArrayElement > sb->inline_bytes || c->dstArrayElement > db->inline_bytes ||
          c->descriptorCount > sb->inline_bytes - c->srcArrayElement || c->descriptorCount > db->inline_bytes - c->dstArrayElement)
         return;
      memmove(dst->cpu + (size_t)(db->slot + 1) * MALI_DESC_SLOT_SIZE + c->dstArrayElement,
              src->cpu + (size_t)(sb->slot + 1) * MALI_DESC_SLOT_SIZE + c->srcArrayElement, c->descriptorCount);
      return;
   }
   /* spec: a copy that runs past the end of a binding continues into the next consecutive binding, on both sides */
   uint32_t sbi = c->srcBinding, dbi = c->dstBinding, se = c->srcArrayElement, de = c->dstArrayElement;
   for (uint32_t i = 0; i < c->descriptorCount; i++) {
      while (sbi < src->layout->nbindings && se >= src->layout->b[sbi].count) {
         se -= src->layout->b[sbi].count;
         sbi++;
      }
      while (dbi < dst->layout->nbindings && de >= dst->layout->b[dbi].count) {
         de -= dst->layout->b[dbi].count;
         dbi++;
      }
      if (sbi >= src->layout->nbindings || dbi >= dst->layout->nbindings) return;
      sb = &src->layout->b[sbi];
      db = &dst->layout->b[dbi];
      if (sb->type == MALI_DT_NONE || db->type == MALI_DT_NONE) return;
      if (sb->stride && db->stride)
         memcpy(slot_at(dst, db->slot + de * db->stride), slot_at(src, sb->slot + se * sb->stride),
                MIN2(sb->stride, db->stride) * MALI_DESC_SLOT_SIZE);
      else if (!sb->stride && !db->stride)
         dst->dyn[db->dyn_idx + de] = src->dyn[sb->dyn_idx + se];
      se++;
      de++;
   }
}

void
mali_update_descriptor_sets(struct vk_device *dev, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c)
{
   for (uint32_t i = 0; i < nw; i++)
      mali_dset_write(mali_dset_from_handle(w[i].dstSet), &w[i]);
   for (uint32_t i = 0; i < nc; i++)
      mali_dset_copy(mali_dset_from_handle(c[i].dstSet), &c[i], mali_dset_from_handle(c[i].srcSet));
}

void
mali_dset_update_template(struct mali_dset *s, const struct vk_descriptor_update_template *t, const void *data)
{
   for (uint32_t e = 0; e < t->entry_count; e++) {
      const struct vk_descriptor_template_entry *en = &t->entries[e];
      if (en->type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
         VkWriteDescriptorSetInlineUniformBlock ib = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK,
                                                       .dataSize = en->array_count, .pData = (const char *)data + en->offset };
         VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &ib, .dstBinding = en->binding,
                                    .dstArrayElement = en->array_element, .descriptorCount = en->array_count, .descriptorType = en->type };
         mali_dset_write(s, &w);
         continue;
      }
      VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = en->binding,
                                 .dstArrayElement = en->array_element, .descriptorCount = 1, .descriptorType = en->type };
      for (uint32_t i = 0; i < en->array_count; i++) {
         const char *p = (const char *)data + en->offset + i * en->stride;
         /* one element at a time: the user stride may differ from the Vulkan struct size */
         w.dstArrayElement = en->array_element + i;
         switch (en->type) {
         case VK_DESCRIPTOR_TYPE_SAMPLER:
         case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
         case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
         case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: w.pImageInfo = (const void *)p; break;
         case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
         case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: w.pTexelBufferView = (const void *)p; break;
         default: w.pBufferInfo = (const void *)p; break;
         }
         mali_dset_write(s, &w);
         w.pImageInfo = NULL; w.pTexelBufferView = NULL; w.pBufferInfo = NULL;
      }
   }
}

void
mali_update_descriptor_set_with_template(struct vk_device *dev, VkDescriptorSet set, VkDescriptorUpdateTemplate t, const void *data)
{
   mali_dset_update_template(mali_dset_from_handle(set), vk_descriptor_update_template_from_handle(t), data);
}

void
mali_dyn_buf_pack(unsigned arch, struct mali_hw_desc *d, const struct mali_dyn_buf *b, uint32_t dyn_offset)
{
   if (!b->va) { memset(d, 0, sizeof *d); return; }   /* null dynamic buffer stays a Null Descriptor */
   mali_pack_buffer(arch, d, b->va + dyn_offset, (uint32_t)(b->size));
}
