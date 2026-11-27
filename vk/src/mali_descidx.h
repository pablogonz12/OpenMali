/* SPDX-License-Identifier: MIT */
/* VK_EXT_descriptor_indexing stage 1: per-binding flags validation. Pure (no device, no hardware), so a host test drives it directly.
 * Honoured unconditionally for the advertised subset; unsupported flag/type combinations are rejected. See docs/descidx-plan.md. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

#define MALI_DESCIDX_KNOWN_FLAGS (VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT | \
                                  VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT)
/* Types we can honestly update after bind today: the image/sampler types (descriptor memory is read at submit, writes are plain). */
#define MALI_DESCIDX_UAB_TYPE(t) ((t) == VK_DESCRIPTOR_TYPE_SAMPLER || (t) == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || \
                                  (t) == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
#define MALI_DESCIDX_DYNAMIC(t) ((t) == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || (t) == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)

/* Parses and validates ci's binding flags. flags_out (if not NULL) has ci->bindingCount entries, in pBindings order, zero when no
 * VkDescriptorSetLayoutBindingFlagsCreateInfo is chained. *uab_pool = layout has UPDATE_AFTER_BIND_POOL. Returns VK_SUCCESS or
 * VK_ERROR_FEATURE_NOT_PRESENT (a flag combination we cannot honour). */
static inline VkResult
mali_descidx_check_layout(const VkDescriptorSetLayoutCreateInfo *ci, VkDescriptorBindingFlags *flags_out, bool *uab_pool)
{
   const VkDescriptorSetLayoutBindingFlagsCreateInfo *bf = NULL;
   for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext)
      if (s->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) bf = (const void *)s;
   bool pool = ci->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
   if (uab_pool) *uab_pool = pool;
   if (flags_out) for (uint32_t i = 0; i < ci->bindingCount; i++) flags_out[i] = 0;
   uint32_t top = 0;
   for (uint32_t i = 0; i < ci->bindingCount; i++) if (ci->pBindings[i].binding > top) top = ci->pBindings[i].binding;
   if (pool)
      for (uint32_t i = 0; i < ci->bindingCount; i++)
         if (MALI_DESCIDX_DYNAMIC(ci->pBindings[i].descriptorType)) return VK_ERROR_FEATURE_NOT_PRESENT;
   if (!bf || !bf->bindingCount) return VK_SUCCESS;
   if (bf->bindingCount != ci->bindingCount || !bf->pBindingFlags) return VK_ERROR_FEATURE_NOT_PRESENT;
   for (uint32_t i = 0; i < ci->bindingCount; i++) {
      VkDescriptorBindingFlags f = bf->pBindingFlags[i];
      VkDescriptorType t = ci->pBindings[i].descriptorType;
      if (f & ~MALI_DESCIDX_KNOWN_FLAGS) return VK_ERROR_FEATURE_NOT_PRESENT;
      /* Only UPDATE_AFTER_BIND needs the descriptor type's update-after-bind support. UPDATE_UNUSED_WHILE_PENDING is
       * gated by its own feature (descriptorBindingUpdateUnusedWhilePending, advertised unconditionally), and the spec
       * does not make it a per-type update-after-bind feature, so a uniform/storage buffer binding may carry it alone. */
      if ((f & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) && !MALI_DESCIDX_UAB_TYPE(t))
         return VK_ERROR_FEATURE_NOT_PRESENT;
      if ((f & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) && (ci->pBindings[i].binding != top || MALI_DESCIDX_DYNAMIC(t)))
         return VK_ERROR_FEATURE_NOT_PRESENT;
      if (flags_out) flags_out[i] = f;
   }
   return VK_SUCCESS;
}

/* Sets from a layout with UPDATE_AFTER_BIND_POOL must come from a pool with UPDATE_AFTER_BIND. */
static inline bool
mali_descidx_pool_ok(bool layout_uab_pool, VkDescriptorPoolCreateFlags pool_flags)
{
   return !layout_uab_pool || (pool_flags & VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT);
}
