/* SPDX-License-Identifier: MIT */
/* M5: VkSampler -> Valhall sampler descriptor (32 B). Base bits match libkbase kb_draw_sampler_nearest_clamp (bits 23, 25) plus the
 * genxml defaults round-to-nearest-even (21) and clamp integer array indices (26). */
#include <math.h>
#include <string.h>
#include "mali_image.h"

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

void
mali_sampler_pack(unsigned arch, const VkSamplerCreateInfo *ci, const struct vk_sampler *s, struct mali_hw_desc *o)
{
   (void)arch;
   memset(o, 0, sizeof *o);
   uint32_t w0 = 1u | (mali_wrap_mode(ci->addressModeU) << 8) | (mali_wrap_mode(ci->addressModeV) << 12) |
                 (mali_wrap_mode(ci->addressModeW) << 16) | (1u << 21) | (1u << 23) | (1u << 25) | (1u << 26);
   if (ci->minFilter == VK_FILTER_NEAREST) w0 |= 1u << 27;
   if (ci->magFilter == VK_FILTER_NEAREST) w0 |= 1u << 28;
   if (ci->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR) w0 |= 3u << 30;
   if (s->reduction_mode == VK_SAMPLER_REDUCTION_MODE_MIN) w0 |= 2u << 4;
   else if (s->reduction_mode == VK_SAMPLER_REDUCTION_MODE_MAX) w0 |= 3u << 4;
   o->w[0] = w0;

   uint32_t w1 = 0;
   if (ci->compareEnable) w1 |= ((uint32_t)ci->compareOp & 7u) << 13;
   w1 |= lod_fixed(ci->maxLod, 0x1fffu) << 16;
   /* min LOD lives in the texture descriptor; the sampler clamps from 0 up to max */
   o->w[1] = w1;

   int32_t bias = (int32_t)lrintf(fminf(fmaxf(ci->mipLodBias, -16.0f), 15.99f) * 256.0f);
   uint32_t w2 = (uint32_t)bias & 0xffffu;
   if (ci->anisotropyEnable && ci->maxAnisotropy > 1.0f) {
      uint32_t n = (uint32_t)fminf(ci->maxAnisotropy, 16.0f);
      w2 |= ((n - 1u) & 0x1fu) << 16;
      w2 |= 3u << 24;
   }
   o->w[2] = w2;
   o->w[4] = s->border_color_value.uint32[0];
   o->w[5] = s->border_color_value.uint32[1];
   o->w[6] = s->border_color_value.uint32[2];
   o->w[7] = s->border_color_value.uint32[3];
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
