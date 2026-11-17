/* SPDX-License-Identifier: MIT */
/* R4-M7 hardware test: standalone Vulkan app. Uploads the deterministic BC vectors (t_bc_vec.h) into a 20x12, 2-level BC image of every
 * non-sRGB-twin format, texelFetches every texel of both levels with bc_probe.comp and compares against the golden (Mesa util_format oracle,
 * shader/golden_bc.h). Tolerance: +-1 per 8-bit channel, +-2 half-float ulps. Runs on any Vulkan device with textureCompressionBC (Intel/AMD:
 * native sanity check of the golden; Mali: via the driver ICD, qdirect-style, VK_ICD_FILENAMES=<panvk icd json> or the loader).
 * Env: BC_DEV = substring of the device name. Exit: 0 pass, 1 fail, 77 skipped (no BC support). Build: tests/build-t_bc.sh builds it too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>
#include "t_bc_vec.h"
#include "t_bc_probe_spv.h"
#include "../shader/golden_bc.h"

#include <math.h>
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (%s:%d)\n", #x, r_, __FILE__, __LINE__); exit(1); } } while (0)

static VkDevice dev; static VkPhysicalDevice pd;

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
   printf("FAIL no memory type\n"); exit(1);
}

static void mkbuf(VkDeviceSize sz, VkBufferUsageFlags u, VkBuffer *b, VkDeviceMemory *m, void **map)
{
   CK(vkCreateBuffer(dev, &(VkBufferCreateInfo){ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz, .usage = u }, NULL, b));
   VkMemoryRequirements r; vkGetBufferMemoryRequirements(dev, *b, &r);
   CK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size,
      .memoryTypeIndex = memtype(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) }, NULL, m));
   CK(vkBindBufferMemory(dev, *b, *m, 0));
   CK(vkMapMemory(dev, *m, 0, VK_WHOLE_SIZE, 0, map));
}

static int cmp(unsigned kind, const uint32_t *got, const uint32_t *exp, unsigned *maxd, unsigned tol)
{
   int bad = 0;
   if (kind == 2) {
      for (int k = 0; k < 4; k++) {
         int a = (got[k / 2] >> (16 * (k & 1))) & 0xffff, b = (exp[k / 2] >> (16 * (k & 1))) & 0xffff;
         unsigned d = (unsigned)abs(a - b);
         if (d > *maxd) *maxd = d;
         if (d > 2) bad = 1;
      }
   } else {
      for (int k = 0; k < 4; k++) {
         int a = (got[0] >> (8 * k)) & 0xff, b = (exp[0] >> (8 * k)) & 0xff;
         if (kind == 1) { a = (int8_t)a; b = (int8_t)b; }
         unsigned d = (unsigned)abs(a - b);
         if (d > *maxd) *maxd = d;
         if (d > tol) bad = 1;
      }
   }
   return bad;
}

int main(void)
{
   VkInstance inst;
   CK(vkCreateInstance(&(VkInstanceCreateInfo){ .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &(VkApplicationInfo){ .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 } }, NULL, &inst));
   uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, NULL);
   VkPhysicalDevice pds[16]; if (n > 16) n = 16; vkEnumeratePhysicalDevices(inst, &n, pds);
   const char *want = getenv("BC_DEV"); pd = VK_NULL_HANDLE;
   for (uint32_t i = 0; i < n; i++) {
      VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
      if (!want || strstr(p.deviceName, want)) { pd = pds[i]; break; }
   }
   if (!pd) { printf("SKIP no device\n"); return 77; }
   VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
   VkPhysicalDeviceFeatures feat; vkGetPhysicalDeviceFeatures(pd, &feat);
   printf("t_bc_hw: device %s\n", props.deviceName);
   if (!feat.textureCompressionBC) { printf("SKIP textureCompressionBC not advertised\n"); return 77; }

   float prio = 1;
   VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
   uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
   VkQueueFamilyProperties qp[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
   for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qi.queueFamilyIndex = i; break; }
   VkPhysicalDeviceFeatures en = { .textureCompressionBC = VK_TRUE };
   CK(vkCreateDevice(pd, &(VkDeviceCreateInfo){ .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
      .pEnabledFeatures = &en }, NULL, &dev));
   VkQueue q; vkGetDeviceQueue(dev, qi.queueFamilyIndex, 0, &q);

   VkSampler smp;
   CK(vkCreateSampler(dev, &(VkSamplerCreateInfo){ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .maxLod = 8 }, NULL, &smp));
   VkDescriptorSetLayout dsl;
   VkDescriptorSetLayoutBinding bnd[2] = {
      { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
      { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } };
   CK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bnd }, NULL, &dsl));
   VkPipelineLayout pl;
   CK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl,
      .pushConstantRangeCount = 1, .pPushConstantRanges = &(VkPushConstantRange){ VK_SHADER_STAGE_COMPUTE_BIT, 0, 20 } }, NULL, &pl));
   VkShaderModule sm;
   CK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){ .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof bc_probe_spv, .pCode = bc_probe_spv }, NULL, &sm));
   VkPipeline pipe;
   CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){ .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
      .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" } }, NULL, &pipe));
   VkDescriptorPool dp;
   CK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, .maxSets = 16, .poolSizeCount = 2,
      .pPoolSizes = (VkDescriptorPoolSize[]){ { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16 } } }, NULL, &dp));
   VkCommandPool cp;
   CK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qi.queueFamilyIndex }, NULL, &cp));

   const unsigned l0 = BCV_W * BCV_H, l1 = (BCV_W / 2) * (BCV_H / 2);
   VkBuffer stg, out; VkDeviceMemory stgm, outm; void *stgp, *outp;
   mkbuf(BCV_NBLOCKS * 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stg, &stgm, &stgp);
   mkbuf(BCG_NTEXELS * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &out, &outm, &outp);

   int fails = 0, nskip = 0;
   for (unsigned i = 0; i < 12; i++) {
      const struct bcg_px *g = &bcg_px[i];
      { const char *only = getenv("BC_ONLY"); if (only && !strstr(g->name, only)) continue; }
      VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, g->vk, &fp);
      if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
         printf("  %-22s SKIP (format feature missing)\n", g->name); nskip++; continue;
      }
      unsigned bs = (g->vk == VK_FORMAT_BC1_RGB_UNORM_BLOCK || g->vk == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || g->vk == VK_FORMAT_BC4_UNORM_BLOCK ||
                     g->vk == VK_FORMAT_BC4_SNORM_BLOCK) ? 8 : 16;
      bcv_fill(g->vk, 0xBC00 + i, stgp, BCV_NBLOCKS, bs, 0);
      VkImage img; VkDeviceMemory imm; VkImageView iv;
      CK(vkCreateImage(dev, &(VkImageCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = g->vk,
         .extent = { BCV_W, BCV_H, 1 }, .mipLevels = BCV_LEVELS, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED }, NULL, &img));
      VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
      CK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
         .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) }, NULL, &imm));
      CK(vkBindImageMemory(dev, img, imm, 0));
      CK(vkCreateImageView(dev, &(VkImageViewCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = g->vk,
         .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, BCV_LEVELS, 0, 1 } }, NULL, &iv));
      VkDescriptorSet ds;
      CK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl }, &ds));
      VkDescriptorImageInfo dii = { smp, iv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
      VkDescriptorBufferInfo dbi = { out, 0, VK_WHOLE_SIZE };
      vkUpdateDescriptorSets(dev, 2, (VkWriteDescriptorSet[]){
         { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii },
         { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi } }, 0, NULL);

      memset(outp, 0xEE, BCG_NTEXELS * 8);
      VkCommandBuffer cb;
      CK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }, &cb));
      CK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }));
      VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
         .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, BCV_LEVELS, 0, 1 } };
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
      VkBufferImageCopy rg[2] = {
         { .bufferOffset = 0, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { BCV_W, BCV_H, 1 } },
         { .bufferOffset = 15 * bs, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1 }, .imageExtent = { BCV_W / 2, BCV_H / 2, 1 } } };
      vkCmdCopyBufferToImage(cb, stg, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, rg);
      ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
      for (unsigned lv = 0; lv < 2; lv++) {
         uint32_t pc[5] = { BCV_W >> lv, BCV_H >> lv, lv, g->kind, lv ? l0 : 0 };
         vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 20, pc);
         vkCmdDispatch(cb, ((lv ? l1 : l0) + 63) / 64, 1, 1);
      }
      VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
      CK(vkEndCommandBuffer(cb));
      CK(vkQueueSubmit(q, 1, &(VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb }, VK_NULL_HANDLE));
      CK(vkQueueWaitIdle(q));

      /* sRGB: sampling converts rgb to linear, the golden holds the raw decode: convert it here (tolerance below is +-2 for it) */
      uint32_t ex[BCG_NTEXELS * 2]; memcpy(ex, g->w, sizeof ex);
      unsigned tol = 1;
      if (g->vk == VK_FORMAT_BC7_SRGB_BLOCK) {
         tol = 2;
         for (unsigned t = 0; t < BCG_NTEXELS; t++)
            for (int c = 0; c < 3; c++) {
               float s = ((ex[t * 2] >> (8 * c)) & 0xff) / 255.0f, l = s <= 0.04045f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
               ex[t * 2] = (ex[t * 2] & ~(0xffu << (8 * c))) | (uint32_t)(l * 255.0f + 0.5f) << (8 * c);
            }
      }
      const uint32_t *got = outp; unsigned maxd = 0, nbad = 0, first = ~0u;
      for (unsigned t = 0; t < BCG_NTEXELS; t++)
         if (cmp(g->kind, got + t * 2, ex + t * 2, &maxd, tol)) { if (first == ~0u) first = t; nbad++; }
      uint64_t h = bcv_fnv(got, BCG_NTEXELS * 8, BCV_FNV0);
      printf("  %-22s %s  maxdiff %u  bad %u/%u  hash %016llx (golden %016llx)", g->name, nbad ? "FAIL" : "PASS", maxd, nbad, BCG_NTEXELS,
             (unsigned long long)h, (unsigned long long)g->hash);
      if (nbad) printf("  first t=%u got %08x %08x exp %08x %08x", first, got[first * 2], got[first * 2 + 1], ex[first * 2], ex[first * 2 + 1]);
      printf("\n");
      fails += nbad != 0;
      vkFreeCommandBuffers(dev, cp, 1, &cb);
      vkFreeDescriptorSets(dev, dp, 1, &ds);
      vkDestroyImageView(dev, iv, NULL); vkDestroyImage(dev, img, NULL); vkFreeMemory(dev, imm, NULL);
   }
   printf("t_bc_hw: %d failed, %d skipped of 12\n", fails, nskip);
   return fails ? 1 : 0;
}
