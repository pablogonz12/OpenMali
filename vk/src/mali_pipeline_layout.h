/* SPDX-License-Identifier: MIT */
/* M7: pipeline layouts. vkCreatePipelineLayout is the runtime's vk_common_CreatePipelineLayout (a plain vk_pipeline_layout that
 * references our mali_dsl set layouts); this file validates it and flattens it for the compiler. */
#pragma once
#include "vk_pipeline_layout.h"
#include "mali_descriptor.h"

/* Storage for a flattened layout: `out` points into bindings stored here, so keep this alive while `out` is used. */
struct mali_flat_layout {
   struct mali_compile_layout c;
};

/* VK_SUCCESS or VK_ERROR_FEATURE_NOT_PRESENT when the layout exceeds the hardware model (sets > MALI_MAX_SETS, push > 128 B). */
VkResult mali_pipeline_layout_flatten(const struct vk_pipeline_layout *l, struct mali_flat_layout *out);

/* Resource table index (1-based, table 0 is the driver table) a set is bound to. */
static inline uint32_t mali_set_table(uint32_t set) { return set + 1; }

/* First dynamic buffer of set `s` in the draw's dynamic-buffer array: vk_pipeline_layout::dynamic_descriptor_offset. Descriptor of
 * dynamic buffer `i` is written at table MALI_DYN_TABLE, index MALI_DYN_BUF_START + i. */
