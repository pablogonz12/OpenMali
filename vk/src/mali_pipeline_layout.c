/* SPDX-License-Identifier: MIT */
#include "mali_pipeline_layout.h"

#include <string.h>

VkResult
mali_pipeline_layout_flatten(const struct vk_pipeline_layout *l, struct mali_flat_layout *out)
{
   memset(out, 0, sizeof *out);
   if (l->set_count > MALI_MAX_SETS)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   uint32_t push = 0;
   for (uint32_t i = 0; i < l->push_range_count; i++)
      push = MAX2(push, l->push_ranges[i].offset + l->push_ranges[i].size);
   if (push > MALI_MAX_PUSH_BYTES)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   out->c.push_size = push;
   out->c.nsets = l->set_count;
   uint32_t dyn = 0;
   for (uint32_t i = 0; i < l->set_count; i++) {
      const struct mali_dsl *d = (const struct mali_dsl *)l->set_layouts[i];
      out->c.dyn_start[i] = dyn;
      if (!d) continue;   /* unused set (VK_NULL_HANDLE with graphics pipeline libraries) */
      out->c.set[i].nbindings = d->nbindings;
      out->c.set[i].slot_count = d->slot_count;
      out->c.set[i].dyn_count = d->dyn_count;
      out->c.set[i].b = d->b;
      dyn += d->dyn_count;
   }
   return VK_SUCCESS;
}
