/* SPDX-License-Identifier: MIT */
/* M7 NIR descriptor lowering for Valhall (v11/v13): Vulkan descriptor sets -> panfrost resource handles.
 * Includes Mesa NIR, so only mali_compile.c and tests that link Mesa use it. */
#pragma once
#include "nir.h"
#include "mali_pipeline_layout_flat.h"

/* Address format the SPIR-V front end must use for UBO and SSBO when this pass is used. */
#define MALI_DESC_BUFFER_ADDR_FORMAT nir_address_format_vec2_index_32bit_offset

/* Lowers, in this order:
 *   vulkan_resource_index/reindex/load_vulkan_descriptor -> vec3(first handle, array index, 0) with the bounded index
 *   texture/sampler/image derefs -> texture/sampler/image handles pan_res_handle(set + 1, slot [+ index * stride]); a texture
 *   without sampler gets the dummy sampler pan_res_handle(0, MALI_DUMMY_SAMPLER_IDX)
 *   explicit io of UBO/SSBO (load_ubo/load_ssbo with the handle as index).
 * Dynamic buffers use pan_res_handle(MALI_DYN_TABLE, MALI_DYN_BUF_START + dyn_start[set] + dyn_idx).
 * Returns false and writes a message to err when the shader uses a set/binding that the layout lacks. */
bool mali_nir_lower_descriptors(nir_shader *nir, const struct mali_compile_layout *layout, char *err, size_t errsz);
