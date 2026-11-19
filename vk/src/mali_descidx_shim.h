/* SPDX-License-Identifier: MIT */
/* MALI_ADVERTISE_DESCIDX=1: OPT-IN COMPATIBILITY SHIM, NOT CONFORMANCE. Default OFF.
 * DXVK hard-requires VkPhysicalDeviceVulkan12Features::descriptorIndexing (dxvk_device_info.cpp, ENABLE_FEATURE(vk12, descriptorIndexing, true)),
 * but DXVK itself only exercises sampled-image arrays (runtimeDescriptorArray, descriptorBindingPartiallyBound,
 * descriptorBindingSampledImageUpdateAfterBind, descriptorBindingUpdateUnusedWhilePending; every other NonUniform/UAB feature is ENABLE_FEATURE(..., false)).
 * Those are already advertised and tested (t_vk_feat didx). With this env the driver additionally reports descriptorIndexing = true, in the
 * Vulkan 1.2 struct only (VkPhysicalDeviceDescriptorIndexingFeatures has no such member; its mirrored members are unchanged). NOT honoured: buffer, storage-image,
 * texel-buffer and input-attachment array indexing, update-after-bind on those types, variableDescriptorCount, the 500000 UAB limits. The CTS descriptor-indexing
 * tests must stay excluded and nothing may report conformance with it. vk/README.md "Descriptor indexing shim". */
#ifndef MALI_DESCIDX_SHIM_H
#define MALI_DESCIDX_SHIM_H

#include <stdbool.h>
#include <stdlib.h>

static inline bool
mali_advertise_descidx(void)
{
   const char *e = getenv("MALI_ADVERTISE_DESCIDX");
   return e && e[0] == '1';
}

#endif
