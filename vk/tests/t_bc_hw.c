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

/* ---------------- RUNG 2: BC arrays, mips, buffer gaps, sub-regions, BC->BC copy, sRGB twin views ----------------
 * The single-layer loop above uploads tightly packed, one layer, and compares one view. Everything a D3D11 atlas/character texture does
 * instead: 3 array layers, 3 mip levels, bufferRowLength / bufferImageHeight != 0 with a non-zero bufferOffset, a level uploaded as two
 * sub-regions, a vkCmdCopyImage2 between two layers of the same BC image, and a UNORM view of an *_SRGB image (and the other way round).
 * Golden: tests/shader/golden_bc.h for levels 0 and 1 of every layer; level 2 (5x3, 2x1 blocks) has no golden and is compared across
 * layers instead (same block bytes in all three), which is what catches a per-layer addressing error there. */
#define AR_L 3u           /* array layers */
#define AR_RING 315u      /* texels per layer in the output buffer: 300 golden texels + 15 for level 2 */

struct stage {
   uint8_t *mem;
   uint64_t cur, cap;
};

/* One VkBufferImageCopy for a block rect of one (layer, level), staged into s->mem with `gapx`/`gapy` texels of padding per row / image
 * height (bufferRowLength / bufferImageHeight). The buffers are always allocated host visible. */
static VkBufferImageCopy
stage_add(struct stage *s, const uint8_t *blk, uint32_t bs, uint32_t level_bw, uint32_t bx0, uint32_t by0, uint32_t nbx, uint32_t nby, uint32_t gapx,
          uint32_t gapy, uint32_t layer, uint32_t level)
{
   const uint32_t rl = nbx * 4 + gapx, ih = nby * 4 + gapy;
   const uint32_t row = ((rl + 3) / 4) * bs, rows = (ih + 3) / 4;
   const uint64_t need = (uint64_t)row * rows;
   if (s->cur + need > s->cap) {
      printf("FAIL stage overflow\n");
      exit(1);
   }
   memset(s->mem + s->cur, 0x5a, need);   /* the gaps must never be read: a wrong pitch brings 0x5a in */
   for (uint32_t y = 0; y < nby; y++)
      memcpy(s->mem + s->cur + (uint64_t)y * row, blk + ((uint64_t)(by0 + y) * level_bw + bx0) * bs, (size_t)nbx * bs);
   VkBufferImageCopy r = { .bufferOffset = s->cur, .bufferRowLength = rl, .bufferImageHeight = ih,
      .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1 }, .imageOffset = { (int32_t)(bx0 * 4), (int32_t)(by0 * 4), 0 },
      .imageExtent = { nbx * 4, nby * 4, 1 } };
   s->cur += need;
   return r;
}

/* probe one level of a (BC) view into the output buffer at `obase`; kind/tol follow the caller's golden */
static void probe_level(VkCommandBuffer cb, VkPipeline pipe, VkPipelineLayout pl, VkDescriptorSet ds, uint32_t w, uint32_t h, uint32_t level, unsigned kind,
                        uint32_t obase)
{
   uint32_t pc[5] = { w, h, level, kind, obase };
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
   vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 20, pc);
   vkCmdDispatch(cb, (w * h + 63) / 64, 1, 1);
}

/* the golden linearised, for a view that samples through an sRGB format (the texture unit converts the raw decode on read) */
static void golden_linear(uint32_t *ex, unsigned n)
{
   for (unsigned t = 0; t < n; t++)
      for (int c = 0; c < 3; c++) {
         float s = ((ex[t * 2] >> (8 * c)) & 0xff) / 255.0f, l = s <= 0.04045f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
         ex[t * 2] = (ex[t * 2] & ~(0xffu << (8 * c))) | (uint32_t)(l * 255.0f + 0.5f) << (8 * c);
      }
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
   CK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, .maxSets = 256, .poolSizeCount = 2,
      .pPoolSizes = (VkDescriptorPoolSize[]){ { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256 } } }, NULL, &dp));
   VkCommandPool cp;
   CK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qi.queueFamilyIndex }, NULL, &cp));

   const unsigned l0 = BCV_W * BCV_H, l1 = (BCV_W / 2) * (BCV_H / 2);
   VkBuffer stg, out; VkDeviceMemory stgm, outm; void *stgp, *outp;
   mkbuf(BCV_NBLOCKS * 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stg, &stgm, &stgp);
   mkbuf(BCG_NTEXELS * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &out, &outm, &outp);

   if (getenv("BC_RGBA")) {   /* bisect: plain RGBA8 image, copy + compute texelFetch, no decode */
      VkBuffer sb; VkDeviceMemory sm_; void *sp;
      mkbuf(300 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sb, &sm_, &sp);
      uint32_t *pw = sp; for (unsigned i = 0; i < 300; i++) pw[i] = 0x80000000u | (i * 0x010203u & 0xffffffu);
      VkImage img; VkDeviceMemory imm; VkImageView iv;
      CK(vkCreateImage(dev, &(VkImageCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = { BCV_W, BCV_H, 1 }, .mipLevels = BCV_LEVELS, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED }, NULL, &img));
      VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
      CK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
         .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) }, NULL, &imm));
      CK(vkBindImageMemory(dev, img, imm, 0));
      CK(vkCreateImageView(dev, &(VkImageViewCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
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
         { .bufferOffset = 240 * 4, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1 }, .imageExtent = { BCV_W / 2, BCV_H / 2, 1 } } };
      vkCmdCopyBufferToImage(cb, sb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, rg);
      ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
      for (unsigned lv = 0; lv < 2; lv++) {
         uint32_t pc[5] = { BCV_W >> lv, BCV_H >> lv, lv, getenv("BC_KIND") ? (unsigned)atoi(getenv("BC_KIND")) : 0u, lv ? 240 : 0 };
         vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 20, pc);
         vkCmdDispatch(cb, ((lv ? 60 : 240) + 63) / 64, 1, 1);
      }
      VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
      CK(vkEndCommandBuffer(cb));
      CK(vkQueueSubmit(q, 1, &(VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb }, VK_NULL_HANDLE));
      CK(vkQueueWaitIdle(q));
      const uint32_t *got = outp; unsigned bad = 0;
      for (unsigned i = 0; i < 300; i++) if (got[i * 2] != (getenv("BC_KIND") && atoi(getenv("BC_KIND")) == 10 ? 0x12345678u + (i < 240 ? i : i - 240) : pw[i])) { if (!bad) printf("  first i=%u got %08x exp %08x\n", i, got[i * 2], pw[i]); bad++; }
      if (getenv("BC_DUMP")) for (unsigned i = 0; i < 300; i++) if (got[i * 2]) printf("   ok? i=%u got %08x exp %08x\n", i, got[i * 2], pw[i]);
      printf("t_bc_hw RGBA8 plain: %s bad %u/300 (got[0..3] %08x %08x %08x %08x)\n", bad ? "FAIL" : "PASS", bad, got[0], got[2], got[4], got[6]);
      return bad != 0;
   }
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
   /* ---- RUNG 2: arrays, mips, buffer gaps, sub-regions, BC->BC copy, sRGB twin views ---- */
   {
      const unsigned al0 = BCV_W * BCV_H, al1 = (BCV_W / 2) * (BCV_H / 2), al2 = (BCV_W / 4) * (BCV_H / 4);   /* 240 60 15 */
      const uint32_t ring = al0 + al1 + al2;
      VkBuffer astg, aout; VkDeviceMemory astgm, aoutm; void *astgp, *aoutp;
      mkbuf(65536, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &astg, &astgm, &astgp);
      mkbuf((VkDeviceSize)6 * ring * 8 + 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &aout, &aoutm, &aoutp);
      printf("t_bc_hw: arrays: 3 layers, 3 mips, gapped rows, 2-region level, BC->BC copy, sRGB twin views\n");
      for (unsigned ai = 0; ai < 12; ai++) {
         const struct bcg_px *g = &bcg_px[ai];
         { const char *only = getenv("BC_ONLY"); if (only && !strstr(g->name, only)) continue; }
         VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, g->vk, &fp);
         if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
            printf("  %-22s SKIP (format feature missing)\n", g->name); continue;
         }
         const bool bs8 = g->vk == VK_FORMAT_BC1_RGB_UNORM_BLOCK || g->vk == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || g->vk == VK_FORMAT_BC1_RGB_SRGB_BLOCK ||
                          g->vk == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || g->vk == VK_FORMAT_BC4_UNORM_BLOCK || g->vk == VK_FORMAT_BC4_SNORM_BLOCK;
         const unsigned bs = bs8 ? 8 : 16;
         uint8_t blk[24 * 16];
         bcv_fill(g->vk, 0xBC00 + ai, blk, 23, bs, 0);   /* the golden's own stream, 2 blocks more for level 2 (no golden there) */
         const uint8_t *lv0 = blk, *lv1 = blk + 15 * bs, *lv2 = blk + 21 * bs;
         struct stage st = { astgp, 0, 65536 };
         VkBufferImageCopy rg[16];
         unsigned ng = 0;
         for (uint32_t L = 0; L < AR_L; L++) {
            rg[ng++] = stage_add(&st, lv0, bs, 5, 0, 0, 5, 2, 8, 4, L, 0);   /* rows of blocks 0..1, gaps on both axes */
            rg[ng++] = stage_add(&st, lv0, bs, 5, 0, 2, 5, 1, 4, 8, L, 0);   /* block row 2, the other gaps */
            rg[ng++] = stage_add(&st, lv1, bs, 3, 0, 0, 3, 2, 6, 2, L, 1);
            rg[ng++] = stage_add(&st, lv2, bs, 2, 0, 0, 2, 1, 0, 0, L, 2);   /* tightly packed */
         }
         VkImage ai_img; VkDeviceMemory ai_mem; VkImageView av[AR_L]; VkDescriptorSet ads[AR_L];
         CK(vkCreateImage(dev, &(VkImageCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = g->vk,
            .extent = { BCV_W, BCV_H, 1 }, .mipLevels = 3, .arrayLayers = AR_L, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED }, NULL, &ai_img));
         VkMemoryRequirements amr; vkGetImageMemoryRequirements(dev, ai_img, &amr);
         CK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = amr.size,
            .memoryTypeIndex = memtype(amr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) }, NULL, &ai_mem));
         CK(vkBindImageMemory(dev, ai_img, ai_mem, 0));
         for (uint32_t L = 0; L < AR_L; L++) {
            CK(vkCreateImageView(dev, &(VkImageViewCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = ai_img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
               .format = g->vk, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 3, L, 1 } }, NULL, &av[L]));
            CK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
               .descriptorSetCount = 1, .pSetLayouts = &dsl }, &ads[L]));
            VkDescriptorImageInfo dii = { smp, av[L], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorBufferInfo dbi = { aout, 0, VK_WHOLE_SIZE };
            vkUpdateDescriptorSets(dev, 2, (VkWriteDescriptorSet[]){
               { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ads[L], .dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii },
               { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ads[L], .dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi } }, 0, NULL);
         }
         /* level 0 and 1 of every layer against the golden; level 2 (no golden) across layers */
         uint32_t aex[BCG_NTEXELS * 2];
         memcpy(aex, g->w, sizeof aex);
         unsigned atol = 1;
         if (g->vk == VK_FORMAT_BC7_SRGB_BLOCK) {
            atol = 2;
            golden_linear(aex, BCG_NTEXELS);
         }
         memset(aoutp, 0xEE, 6 * ring * 8);
         VkCommandBuffer acb;
         CK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }, &acb));
         CK(vkBeginCommandBuffer(acb, &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }));
         VkImageMemoryBarrier ab = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = ai_img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 3, 0, AR_L } };
         vkCmdPipelineBarrier(acb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ab);
         vkCmdCopyBufferToImage(acb, astg, ai_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ng, rg);
         ab.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ab.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
         ab.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ab.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
         vkCmdPipelineBarrier(acb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ab);
         for (uint32_t L = 0; L < AR_L; L++) {
            probe_level(acb, pipe, pl, ads[L], BCV_W, BCV_H, 0, g->kind, L * ring);
            probe_level(acb, pipe, pl, ads[L], BCV_W / 2, BCV_H / 2, 1, g->kind, L * ring + al0);
            probe_level(acb, pipe, pl, ads[L], BCV_W / 4, BCV_H / 4, 2, g->kind, L * ring + al0 + al1);
         }
         VkMemoryBarrier amb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
         vkCmdPipelineBarrier(acb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &amb, 0, NULL, 0, NULL);
         CK(vkEndCommandBuffer(acb));
         CK(vkQueueSubmit(q, 1, &(VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &acb }, VK_NULL_HANDLE));
         CK(vkQueueWaitIdle(q));
         vkFreeCommandBuffers(dev, cp, 1, &acb);
         const uint32_t *ag = aoutp;
         unsigned abad = 0, amax = 0, afirst = ~0u, l2bad = 0;
         for (uint32_t L = 0; L < AR_L; L++)
            for (unsigned t = 0; t < al0 + al1; t++)
               if (cmp(g->kind, ag + (L * ring + t) * 2, aex + t * 2, &amax, atol)) {
                  if (afirst == ~0u) afirst = L * ring + t;
                  abad++;
               }
         for (uint32_t L = 1; L < AR_L; L++)   /* level 2: same block bytes in every layer, so the layers must agree bit for bit */
            for (unsigned t = 0; t < al2; t++)
               if (memcmp(ag + (L * ring + al0 + al1 + t) * 2, ag + (al0 + al1 + t) * 2, 8)) l2bad++;
         printf("  %-22s arrays %s  maxdiff %u  bad %u/%u (mip2 cross-layer bad %u)", g->name, abad || l2bad ? "FAIL" : "PASS", amax, abad,
                3u * (al0 + al1), l2bad);
         if (abad) printf("  first layer*ring+t=%u got %08x exp %08x", afirst, ag[afirst * 2], aex[(afirst % ring) * 2]);
         printf("\n");
         fails += abad != 0 || l2bad != 0;
         /* BC -> BC image copy: layer 0 level 0 into layer 2 level 0, then layer 2 level 0 must hold the golden again */
         VkCommandBuffer ccb;
         CK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }, &ccb));
         CK(vkBeginCommandBuffer(ccb, &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }));
         VkImageMemoryBarrier cbs[2] = { ab, ab };
         cbs[0].subresourceRange = cbs[1].subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, AR_L };
         cbs[0].srcAccessMask = cbs[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
         cbs[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; cbs[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
         cbs[0].oldLayout = cbs[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
         cbs[0].newLayout = cbs[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
         vkCmdPipelineBarrier(ccb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, cbs);
         vkCmdCopyImage(ccb, ai_img, VK_IMAGE_LAYOUT_GENERAL, ai_img, VK_IMAGE_LAYOUT_GENERAL, 1, &(VkImageCopy){
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 1 },
            .extent = { BCV_W, BCV_H, 1 } });
         cbs[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; cbs[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
         cbs[0].dstAccessMask = cbs[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
         cbs[0].oldLayout = cbs[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
         cbs[0].newLayout = cbs[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
         vkCmdPipelineBarrier(ccb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 2, cbs);
         probe_level(ccb, pipe, pl, ads[2], BCV_W, BCV_H, 0, g->kind, 3 * ring);   /* scratch area (3 * ring) */
         vkCmdPipelineBarrier(ccb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &amb, 0, NULL, 0, NULL);
         CK(vkEndCommandBuffer(ccb));
         CK(vkQueueSubmit(q, 1, &(VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &ccb }, VK_NULL_HANDLE));
         CK(vkQueueWaitIdle(q));
         vkFreeCommandBuffers(dev, cp, 1, &ccb);
         unsigned cbad = 0, cmax = 0;
         for (unsigned t = 0; t < al0; t++)
            if (cmp(g->kind, ag + (3 * ring + t) * 2, aex + t * 2, &cmax, atol)) cbad++;
         printf("  %-22s copy 0->2 %s  maxdiff %u  bad %u/%u\n", g->name, cbad ? "FAIL" : "PASS", cmax, cbad, al0);
         fails += cbad != 0;
         for (uint32_t L = 0; L < AR_L; L++) vkDestroyImageView(dev, av[L], NULL);
         vkDestroyImage(dev, ai_img, NULL);
         vkFreeMemory(dev, ai_mem, NULL);
      }
      /* sRGB twin views: a UNORM view of an *_SRGB BC image must sample the raw decode (no transfer function), an SRGB view of a UNORM
       * image must linearise. Both share the one decoded shadow, so only the view's own format may pick the shadow format. */
      for (unsigned ti = 0; ti < 2; ti++) {
         const VkFormat imgf = ti ? VK_FORMAT_BC7_UNORM_BLOCK : VK_FORMAT_BC7_SRGB_BLOCK;
         const VkFormat viewf = ti ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
         const struct bcg_px *g = &bcg_px[11];   /* BC7_SRGB: one block stream serves both twins (same decode, only the view differs) */
         VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, imgf, &fp);
         if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { printf("  twin %s SKIP\n", g->name); continue; }
         uint8_t blk[24 * 16];
         bcv_fill(g->vk, 0xBC00 + 11, blk, 21, 16, 0);
         struct stage st = { astgp, 0, 65536 };
         VkBufferImageCopy rg[2];
         unsigned ng = 0;
         rg[ng++] = stage_add(&st, blk, 16, 5, 0, 0, 5, 3, 0, 0, 0, 0);
         rg[ng++] = stage_add(&st, blk + 15 * 16, 16, 3, 0, 0, 3, 2, 0, 0, 0, 1);
         VkImage ti_img; VkDeviceMemory ti_mem;
         CK(vkCreateImage(dev, &(VkImageCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = imgf,
            .extent = { BCV_W, BCV_H, 1 }, .mipLevels = 2, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED }, NULL, &ti_img));
         VkMemoryRequirements tmr; vkGetImageMemoryRequirements(dev, ti_img, &tmr);
         CK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = tmr.size,
            .memoryTypeIndex = memtype(tmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) }, NULL, &ti_mem));
         CK(vkBindImageMemory(dev, ti_img, ti_mem, 0));
         VkImageView tiv; VkDescriptorSet tds;
         CK(vkCreateImageView(dev, &(VkImageViewCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = ti_img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = viewf, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, 1 } }, NULL, &tiv));
         CK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
            .descriptorSetCount = 1, .pSetLayouts = &dsl }, &tds));
         VkDescriptorImageInfo dii = { smp, tiv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
         VkDescriptorBufferInfo dbi = { aout, 0, VK_WHOLE_SIZE };
         vkUpdateDescriptorSets(dev, 2, (VkWriteDescriptorSet[]){
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = tds, .dstBinding = 0, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = tds, .dstBinding = 1, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi } }, 0, NULL);
         VkCommandBuffer tcb;
         CK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }, &tcb));
         CK(vkBeginCommandBuffer(tcb, &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }));
         VkImageMemoryBarrier tb = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = ti_img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, 1 } };
         vkCmdPipelineBarrier(tcb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &tb);
         vkCmdCopyBufferToImage(tcb, astg, ti_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ng, rg);
         tb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; tb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
         tb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; tb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
         vkCmdPipelineBarrier(tcb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &tb);
         probe_level(tcb, pipe, pl, tds, BCV_W, BCV_H, 0, g->kind, 0);
         probe_level(tcb, pipe, pl, tds, BCV_W / 2, BCV_H / 2, 1, g->kind, al0);
         VkMemoryBarrier tmb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
         vkCmdPipelineBarrier(tcb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &tmb, 0, NULL, 0, NULL);
         CK(vkEndCommandBuffer(tcb));
         CK(vkQueueSubmit(q, 1, &(VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &tcb }, VK_NULL_HANDLE));
         CK(vkQueueWaitIdle(q));
         vkFreeCommandBuffers(dev, cp, 1, &tcb);
         uint32_t tex[BCG_NTEXELS * 2];
         memcpy(tex, g->w, sizeof tex);
         unsigned ttol = 1;
         /* the view's format decides: the SRGB view linearises the raw decode, the UNORM view returns it unchanged */
         if (viewf == VK_FORMAT_BC7_SRGB_BLOCK) {
            ttol = 2;
            golden_linear(tex, BCG_NTEXELS);
         }
         const uint32_t *tg = aoutp;
         unsigned tbad = 0, tmax = 0, tfirst = ~0u;
         for (unsigned t = 0; t < al0 + al1; t++)
            if (cmp(g->kind, tg + t * 2, tex + t * 2, &tmax, ttol)) {
               if (tfirst == ~0u) tfirst = t;
               tbad++;
            }
         printf("  twin %-18s %s view %s  maxdiff %u  bad %u/%u", g->name, tbad ? "FAIL" : "PASS",
                viewf == VK_FORMAT_BC7_SRGB_BLOCK ? "SRGB" : "UNORM", tmax, tbad, al0 + al1);
         if (tbad) printf("  first t=%u got %08x exp %08x", tfirst, tg[tfirst * 2], tex[tfirst * 2]);
         printf("\n");
         fails += tbad != 0;
         vkDestroyImageView(dev, tiv, NULL);
         vkDestroyImage(dev, ti_img, NULL);
         vkFreeMemory(dev, ti_mem, NULL);
      }
      vkDestroyBuffer(dev, astg, NULL); vkFreeMemory(dev, astgm, NULL);
      vkDestroyBuffer(dev, aout, NULL); vkFreeMemory(dev, aoutm, NULL);
   }
   printf("t_bc_hw: %d failed, %d skipped of 12\n", fails, nskip);
   return fails ? 1 : 0;
}
