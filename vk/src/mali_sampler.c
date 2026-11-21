/* SPDX-License-Identifier: MIT */
/* M5: VkSampler -> Valhall sampler descriptor (32 B). Base bits match libkbase kb_draw_sampler_nearest_clamp (bits 23, 25) plus the
 * genxml defaults round-to-nearest-even (21) and clamp integer array indices (26). */
#include <math.h>
#include <string.h>
#include "mali_image.h"
#include "mali_dbg.h"

uint32_t
mali_wrap_mode(VkSamplerAddressMode m)
{
   switch (m) {
   case VK_SAMPLER_ADDRESS_MODE_REPEAT: return 8;
   case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return 12;
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: return 9;
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: return 11;
   case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: return 13;
   default: return 9;
   }
}

static uint32_t
lod_fixed(float v, uint32_t max)
{
   if (!(v > 0.0f)) return 0;
   float f = v * 256.0f;
   if (f > (float)max) return max;
   return (uint32_t)f;
}

/* VK_EXT_border_color_swizzle groundwork: the HW is assumed to apply the view swizzle to the border colour too, so scatter the wanted colour through the
 * mapping (hw[swz[c]] = want[c]) so the view's output equals it. IDENTITY/ZERO/ONE channels are left alone; an all-zero (unset) mapping is identity.
 * UNVERIFIED on silicon: do not advertise the extension before a phone check. */
void
mali_border_color_unswizzle(const VkComponentMapping *m, const uint32_t want[4], uint32_t out[4])
{
   const VkComponentSwizzle sw[4] = { m->r, m->g, m->b, m->a };
   uint32_t tmp[4] = { want[0], want[1], want[2], want[3] };
   for (unsigned c = 0; c < 4; c++) {
      VkComponentSwizzle x = sw[c];
      if (x >= VK_COMPONENT_SWIZZLE_R && x <= VK_COMPONENT_SWIZZLE_A) tmp[x - VK_COMPONENT_SWIZZLE_R] = want[c];
   }
   memcpy(out, tmp, sizeof tmp);
}

void
mali_sampler_pack(unsigned arch, const VkSamplerCreateInfo *ci, const struct vk_sampler *s, struct mali_hw_desc *o)
{
   (void)arch;
   memset(o, 0, sizeof *o);
   mali_dbg_line("sampler", "SAMPLER min=%d mag=%d mip=%d addr=%d,%d,%d aniso=%d/%g cmp=%d(%d) unnorm=%d lod=%g..%g",
                 (int)ci->minFilter, (int)ci->magFilter, (int)ci->mipmapMode, (int)ci->addressModeU, (int)ci->addressModeV,
                 (int)ci->addressModeW, (int)ci->anisotropyEnable, (double)ci->maxAnisotropy, (int)ci->compareEnable,
                 (int)ci->compareOp, (int)ci->unnormalizedCoordinates, (double)ci->minLod, (double)ci->maxLod);
   /* genxml Sampler: Wrap Mode R = bits 8, T = 12, S = 16; Vulkan U -> S, V -> T, W -> R (as panvk_vX_sampler.c). Unnormalised coordinates ignore W. */
   uint32_t wr = ci->unnormalizedCoordinates ? mali_wrap_mode(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE) : mali_wrap_mode(ci->addressModeW);
   uint32_t w0 = 1u | (wr << 8) | (mali_wrap_mode(ci->addressModeV) << 12) |
                 (mali_wrap_mode(ci->addressModeU) << 16) | (1u << 21) | (1u << 23) | (1u << 25) | (1u << 26);
   if (ci->minFilter == VK_FILTER_NEAREST) w0 |= 1u << 27;
   if (ci->magFilter == VK_FILTER_NEAREST) w0 |= 1u << 28;
   if (ci->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR) w0 |= 3u << 30;
   if (s->reduction_mode == VK_SAMPLER_REDUCTION_MODE_MIN) w0 |= 2u << 4;
   else if (s->reduction_mode == VK_SAMPLER_REDUCTION_MODE_MAX) w0 |= 3u << 4;
   o->w[0] = w0;

   uint32_t w1 = 0;
   if (ci->compareEnable) w1 |= ((uint32_t)ci->compareOp & 7u) << 13;
   w1 |= lod_fixed(ci->minLod, 0x1fffu);   /* genxml Sampler "Minimum LOD": word 1 bits 0:12, 8.8 fixed (the view's min_lod is a separate clamp) */
   w1 |= lod_fixed(ci->maxLod, 0x1fffu) << 16;
   o->w[1] = w1;

   int32_t bias = (int32_t)lrintf(fminf(fmaxf(ci->mipLodBias, -16.0f), 15.99f) * 256.0f);
   uint32_t w2 = (uint32_t)bias & 0xffffu;
   if (ci->anisotropyEnable && ci->maxAnisotropy > 1.0f) {
      uint32_t n = (uint32_t)fminf(ci->maxAnisotropy, 16.0f);
      w2 |= ((n - 1u) & 0x1fu) << 16;
      w2 |= 3u << 24;
   }
   o->w[2] = w2;
   uint32_t bc[4];
   VkComponentMapping bm = {0};   /* vk_sampler drops VkSamplerBorderColorComponentMappingCreateInfoEXT: read it from the create info */
   for (const VkBaseInStructure *e = ci->pNext; e; e = e->pNext)
      if (e->sType == VK_STRUCTURE_TYPE_SAMPLER_BORDER_COLOR_COMPONENT_MAPPING_CREATE_INFO_EXT)
         bm = ((const VkSamplerBorderColorComponentMappingCreateInfoEXT *)e)->components;
   mali_border_color_unswizzle(&bm, s->border_color_value.uint32, bc);
   memcpy(&o->w[4], bc, sizeof bc);
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_CreateSampler(VkDevice _dev, const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *alloc, VkSampler *out)
{
   VK_FROM_HANDLE(vk_device, dev, _dev);
   struct mali_sampler *s = vk_sampler_create(dev, ci, alloc, sizeof *s);
   if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
   mali_sampler_pack(0, ci, &s->vk, &s->hw);
   *out = mali_sampler_to_handle(s);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
mali_DestroySampler(VkDevice _dev, VkSampler h, const VkAllocationCallbacks *alloc)
{
   VK_FROM_HANDLE(vk_device, dev, _dev);
   VK_FROM_HANDLE(mali_sampler, s, h);
   if (!s) return;
   vk_sampler_destroy(dev, alloc, &s->vk);
}
