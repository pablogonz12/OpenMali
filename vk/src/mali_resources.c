/* SPDX-License-Identifier: MIT */
/* M5/M7 wiring: resolves the seams of mali_descriptor.c / mali_image.c to the real objects and the M4 memory, and exposes the
 * descriptor entrypoints (the code in mali_descriptor.c is device-handle free; arch comes from the device here). */
#include "mali_descriptor.h"
#include "mali_entrypoints.h"
#include "mali_image.h"
#include "mali_queue.h"

#include <string.h>

/* GPU pool memory: one backend BO per allocation (uncached, zeroed). The first 64 bytes of the BO keep the struct mali_bo so
 * that free() can find it from the CPU pointer; the user part starts 64 bytes in (keeps 64-byte alignment). */
#define POOL_HDR 64u
_Static_assert(sizeof(struct mali_bo) <= POOL_HDR, "mali_bo header");

static void *
pool_alloc(struct vk_device *vkdev, size_t size, uint64_t *va)
{
   struct mali_device *dev = container_of(vkdev, struct mali_device, vk);
   struct mali_bo bo;
   if (mali_device_pdev(dev)->backend->bo_alloc(dev->kdev, size + POOL_HDR, 0, &bo) != 0)
      return NULL;
   memcpy(bo.cpu, &bo, sizeof bo);
   *va = bo.gpu_va + POOL_HDR;
   return (char *)bo.cpu + POOL_HDR;
}

static void
pool_free(struct vk_device *vkdev, void *cpu, size_t size)
{
   struct mali_device *dev = container_of(vkdev, struct mali_device, vk);
   struct mali_bo bo;
   memcpy(&bo, (char *)cpu - POOL_HDR, sizeof bo);
   mali_device_pdev(dev)->backend->bo_free(dev->kdev, &bo);
}

static const struct mali_desc_mem_ops pool_ops = { pool_alloc, pool_free };

void
mali_resources_init(void)
{
   mali_desc_set_res_ops(&mali_res_ops);
   mali_desc_set_mem_ops(&pool_ops);
   mali_image_set_mem_ops(&pool_ops);
}

static unsigned
arch_of(VkDevice d)
{
   VK_FROM_HANDLE(mali_device, dev, d);
   return mali_device_pdev(dev)->gpu.arch_major;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_CreateDescriptorSetLayout(VkDevice d, const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *a,
                               VkDescriptorSetLayout *out)
{
   VK_FROM_HANDLE(mali_device, dev, d);
   return mali_create_descriptor_set_layout(&dev->vk, arch_of(d), ci, out);
}

VKAPI_ATTR void VKAPI_CALL
mali_GetDescriptorSetLayoutSupport(VkDevice d, const VkDescriptorSetLayoutCreateInfo *ci, VkDescriptorSetLayoutSupport *s)
{
   mali_get_descriptor_set_layout_support(ci, s);
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_CreateDescriptorPool(VkDevice d, const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorPool *out)
{
   VK_FROM_HANDLE(mali_device, dev, d);
   return mali_create_descriptor_pool(&dev->vk, arch_of(d), ci, a, out);
}

VKAPI_ATTR void VKAPI_CALL
mali_DestroyDescriptorPool(VkDevice d, VkDescriptorPool p, const VkAllocationCallbacks *a)
{
   VK_FROM_HANDLE(mali_device, dev, d);
   if (p) mali_destroy_descriptor_pool(&dev->vk, p, a);
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_ResetDescriptorPool(VkDevice d, VkDescriptorPool p, VkDescriptorPoolResetFlags f)
{
   VK_FROM_HANDLE(mali_device, dev, d);
   return mali_reset_descriptor_pool(&dev->vk, p);
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_AllocateDescriptorSets(VkDevice d, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out)
{
   VK_FROM_HANDLE(mali_device, dev, d);
   return mali_allocate_descriptor_sets(&dev->vk, ai, out);
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_FreeDescriptorSets(VkDevice d, VkDescriptorPool p, uint32_t n, const VkDescriptorSet *sets)
{
   VK_FROM_HANDLE(mali_device, dev, d);
   return mali_free_descriptor_sets(&dev->vk, p, n, sets);
}

VKAPI_ATTR void VKAPI_CALL
mali_UpdateDescriptorSets(VkDevice d, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c)
{
   VK_FROM_HANDLE(mali_device, dev, d);
   mali_update_descriptor_sets(&dev->vk, nw, w, nc, c);
}

VKAPI_ATTR void VKAPI_CALL
mali_UpdateDescriptorSetWithTemplate(VkDevice d, VkDescriptorSet set, VkDescriptorUpdateTemplate t, const void *data)
{
   VK_FROM_HANDLE(mali_device, dev, d);
   mali_update_descriptor_set_with_template(&dev->vk, set, t, data);
}
