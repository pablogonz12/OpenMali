/* SPDX-License-Identifier: MIT */
/* Mip-chain probe: is a mip level's CONTENT right (layout/offset) and does the DESCRIPTOR read the level the shader
 * asks for? Two images per size:
 *   A  every level filled with a positional pattern (x,y,level) -> upload per level with vkCmdCopyBufferToImage the
 *      way dEQP does, copy every level back with vkCmdCopyImageToBuffer and compare byte-exact per level.
 *   B  every level filled with a CONSTANT colour that depends only on the level -> sample with textureLod(L) and a
 *      NEAREST/NEAREST sampler, render 64x64; every pixel must be level L's colour. A wrong level shows as every
 *      pixel differing, so the report names the level the descriptor actually read.
 * Loader-free: dlopens the ICD like t_vk_draw --direct.
 *   t_vk_mip --direct <icd.so> --spv-dir <dir with mip.vert.spv mip.frag.spv> [--sizes 4x8,32x64,127x99,128x128]
 * One JSON line per case, exit 0 = every level exact and every level sampled correctly.
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RT 64   /* render target size for the sampling pass */

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue q;
static VkPhysicalDeviceMemoryProperties mp;
static VkCommandPool pool;
static int g_fail;
static const char *g_spv = "shader";
static int g_ramp, g_probe, g_deriv2, g_bisect, g_edge;

#define FNS(X) \
   X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
   X(vkBindBufferMemory) X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkDestroyImage) \
   X(vkDestroyBuffer) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
   X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
   X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkDestroyFence) \
   X(vkMapMemory) X(vkUnmapMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) \
   X(vkDestroySampler) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) \
   X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkDestroyPipelineLayout) \
   X(vkDestroyDescriptorSetLayout) X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) \
   X(vkDestroyFramebuffer) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) \
   X(vkUpdateDescriptorSets) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
   X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDraw) X(vkDestroyCommandPool) X(vkResetCommandPool) X(vkFreeCommandBuffers)
#define DECL(f) static PFN_##f f##_;
FNS(DECL)
#undef DECL
/* call the loaded pointers under their plain names (VK_NO_PROTOTYPES: nothing to link against) */
#define vkGetDeviceQueue vkGetDeviceQueue_
#define vkCreateBuffer vkCreateBuffer_
#define vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements_
#define vkAllocateMemory vkAllocateMemory_
#define vkFreeMemory vkFreeMemory_
#define vkBindBufferMemory vkBindBufferMemory_
#define vkCreateImage vkCreateImage_
#define vkGetImageMemoryRequirements vkGetImageMemoryRequirements_
#define vkBindImageMemory vkBindImageMemory_
#define vkDestroyImage vkDestroyImage_
#define vkDestroyBuffer vkDestroyBuffer_
#define vkCreateCommandPool vkCreateCommandPool_
#define vkAllocateCommandBuffers vkAllocateCommandBuffers_
#define vkBeginCommandBuffer vkBeginCommandBuffer_
#define vkEndCommandBuffer vkEndCommandBuffer_
#define vkCmdPipelineBarrier vkCmdPipelineBarrier_
#define vkCmdCopyBufferToImage vkCmdCopyBufferToImage_
#define vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer_
#define vkQueueSubmit vkQueueSubmit_
#define vkQueueWaitIdle vkQueueWaitIdle_
#define vkCreateFence vkCreateFence_
#define vkWaitForFences vkWaitForFences_
#define vkResetFences vkResetFences_
#define vkDestroyFence vkDestroyFence_
#define vkMapMemory vkMapMemory_
#define vkUnmapMemory vkUnmapMemory_
#define vkCreateImageView vkCreateImageView_
#define vkDestroyImageView vkDestroyImageView_
#define vkCreateSampler vkCreateSampler_
#define vkDestroySampler vkDestroySampler_
#define vkCreateShaderModule vkCreateShaderModule_
#define vkDestroyShaderModule vkDestroyShaderModule_
#define vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout_
#define vkCreatePipelineLayout vkCreatePipelineLayout_
#define vkCreateGraphicsPipelines vkCreateGraphicsPipelines_
#define vkDestroyPipeline vkDestroyPipeline_
#define vkDestroyPipelineLayout vkDestroyPipelineLayout_
#define vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout_
#define vkCreateRenderPass vkCreateRenderPass_
#define vkDestroyRenderPass vkDestroyRenderPass_
#define vkCreateFramebuffer vkCreateFramebuffer_
#define vkDestroyFramebuffer vkDestroyFramebuffer_
#define vkCreateDescriptorPool vkCreateDescriptorPool_
#define vkDestroyDescriptorPool vkDestroyDescriptorPool_
#define vkAllocateDescriptorSets vkAllocateDescriptorSets_
#define vkUpdateDescriptorSets vkUpdateDescriptorSets_
#define vkCmdBeginRenderPass vkCmdBeginRenderPass_
#define vkCmdEndRenderPass vkCmdEndRenderPass_
#define vkCmdBindPipeline vkCmdBindPipeline_
#define vkCmdBindDescriptorSets vkCmdBindDescriptorSets_
#define vkCmdPushConstants vkCmdPushConstants_
#define vkCmdDraw vkCmdDraw_
#define vkDestroyCommandPool vkDestroyCommandPool_
#define vkResetCommandPool vkResetCommandPool_
#define vkFreeCommandBuffers vkFreeCommandBuffers_

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
   return ~0u;
}

struct buf { VkBuffer b; VkDeviceMemory m; void *p; VkDeviceSize n; };

static int buf_new(struct buf *o, VkDeviceSize n, VkBufferUsageFlags use, VkMemoryPropertyFlags want)
{
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = use, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   if (vkCreateBuffer_(dev, &bi, NULL, &o->b) != VK_SUCCESS) return 1;
   VkMemoryRequirements rq;
   vkGetBufferMemoryRequirements(dev, o->b, &rq);
   uint32_t t = mem_type(rq.memoryTypeBits, want);
   if (t == ~0u) { fprintf(stderr, "buffer: no mem type (bits %x)\n", rq.memoryTypeBits); return 2; }
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = t};
   if (vkAllocateMemory_(dev, &ai, NULL, &o->m) != VK_SUCCESS) { fprintf(stderr, "buffer: alloc %llu B failed\n", (unsigned long long)rq.size); return 3; }
   if (vkBindBufferMemory_(dev, o->b, o->m, 0) != VK_SUCCESS) return 4;
   o->n = n; o->p = NULL;
   if (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      if (vkMapMemory_(dev, o->m, 0, VK_WHOLE_SIZE, 0, &o->p) != VK_SUCCESS) return 5;
   }
   return 0;
}

static void buf_free(struct buf *o)
{
   if (o->p) vkUnmapMemory_(dev, o->m);
   vkDestroyBuffer_(dev, o->b, NULL);
   vkFreeMemory_(dev, o->m, NULL);
}

struct img { VkImage i; VkDeviceMemory m; uint32_t w, h, levels; };

static int img_new(struct img *o, uint32_t w, uint32_t h, uint32_t levels, VkFormat fmt, VkImageUsageFlags use)
{
   VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                           .extent = {w, h, 1}, .mipLevels = levels, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                           .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = use, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VkResult r = vkCreateImage_(dev, &ci, NULL, &o->i);
   if (r != VK_SUCCESS) { fprintf(stderr, "CreateImage %ux%u lv%u -> %d\n", w, h, levels, (int)r); return 1; }
   VkMemoryRequirements rq;
   vkGetImageMemoryRequirements(dev, o->i, &rq);
   uint32_t t = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (t == ~0u) { fprintf(stderr, "no mem type for %ux%u lv%u (bits %x)\n", w, h, levels, rq.memoryTypeBits); return 2; }
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = t};
   r = vkAllocateMemory_(dev, &ai, NULL, &o->m);
   if (r != VK_SUCCESS) { fprintf(stderr, "AllocateMemory %ux%u lv%u %llu B -> %d\n", w, h, levels, (unsigned long long)rq.size, (int)r); return 3; }
   if (vkBindImageMemory_(dev, o->i, o->m, 0) != VK_SUCCESS) return 4;
   o->w = w; o->h = h; o->levels = levels;
   return 0;
}

static uint32_t mip_levels(uint32_t w, uint32_t h)
{
   uint32_t m = w > h ? w : h, n = 1;
   while (m > 1) { m >>= 1; n++; }
   return n;
}

static uint32_t lw(uint32_t w, uint32_t l) { uint32_t v = w >> l; return v ? v : 1; }

/* level content */
static void pat_fill(uint8_t *p, uint32_t w, uint32_t h, uint32_t level)
{
   for (uint32_t y = 0; y < h; y++)
      for (uint32_t x = 0; x < w; x++, p += 4) {
         p[0] = (uint8_t)(x * 7 + y * 13 + level * 37);
         p[1] = (uint8_t)(x * 3 + level * 11 + 5);
         p[2] = (uint8_t)(y * 5 + level * 71 + 9);
         p[3] = 0xff;
      }
}

static void const_fill(uint8_t *p, uint32_t n, uint32_t level)
{
   for (uint32_t i = 0; i < n; i++, p += 4) {
      p[0] = (uint8_t)(level * 37 + 11);
      p[1] = (uint8_t)(level * 61 + 23);
      p[2] = (uint8_t)(level * 91 + 47);
      p[3] = 0xff;
   }
}

static VkCommandBuffer cb_begin(void)
{
   VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                     .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VkCommandBuffer cb;
   vkAllocateCommandBuffers(dev, &ai, &cb);
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   vkBeginCommandBuffer_(cb, &bi);
   return cb;
}

static void cb_submit(VkCommandBuffer cb)
{
   vkEndCommandBuffer_(cb);
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence f;
   vkCreateFence_(dev, &fi, NULL, &f);
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   vkQueueSubmit(q, 1, &si, f);
   vkWaitForFences_(dev, 1, &f, VK_TRUE, UINT64_MAX);
   vkDestroyFence_(dev, f, NULL);
   vkFreeCommandBuffers(dev, pool, 1, &cb);
}

static void barrier(VkCommandBuffer cb, VkImage im, VkImageAspectFlags asp, uint32_t base, uint32_t n,
                    VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da)
{
   VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = from, .newLayout = to,
                             .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                             .image = im, .subresourceRange = {asp, base, n, 0, 1}, .srcAccessMask = sa, .dstAccessMask = da};
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* upload every level of im from src (levels concatenated, rowLength = level width) */
static void upload_levels(VkCommandBuffer cb, struct img *im, struct buf *src, VkImageLayout cur)
{
   VkBufferImageCopy cps[16];
   for (uint32_t l = 0; l < im->levels; l++) {
      uint64_t off = 0;
      for (uint32_t k = 0; k < l; k++) off += (uint64_t)lw(im->w, k) * lw(im->h, k) * 4;
      cps[l] = (VkBufferImageCopy){.bufferOffset = off, .bufferRowLength = lw(im->w, l), .bufferImageHeight = lw(im->h, l),
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1},
                                   .imageExtent = {lw(im->w, l), lw(im->h, l), 1}};
   }
   barrier(cb, im->i, VK_IMAGE_ASPECT_COLOR_BIT, 0, im->levels, cur, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
           cur == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
   vkCmdCopyBufferToImage(cb, src->b, im->i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, im->levels, cps);
}

/* copy every level back into dst, compare with the expected pattern */
static int readback_levels(struct img *im, struct buf *dst, uint32_t **expect_of)
{
   VkCommandBuffer cb = cb_begin();
   VkBufferImageCopy cps[16];
   for (uint32_t l = 0; l < im->levels; l++) {
      uint64_t off = 0;
      for (uint32_t k = 0; k < l; k++) off += (uint64_t)lw(im->w, k) * lw(im->h, k) * 4;
      cps[l] = (VkBufferImageCopy){.bufferOffset = off, .bufferRowLength = lw(im->w, l), .bufferImageHeight = lw(im->h, l),
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1},
                                   .imageExtent = {lw(im->w, l), lw(im->h, l), 1}};
   }
   barrier(cb, im->i, VK_IMAGE_ASPECT_COLOR_BIT, 0, im->levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
   vkCmdCopyImageToBuffer(cb, im->i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->b, im->levels, cps);
   cb_submit(cb);

   int bad = 0;
   uint8_t *got = dst->p;
   uint8_t *exp = malloc(dst->n);
   memset(exp, 0, dst->n);
   uint8_t *p = exp;
   for (uint32_t l = 0; l < im->levels; l++) { pat_fill(p, lw(im->w, l), lw(im->h, l), l); p += (uint64_t)lw(im->w, l) * lw(im->h, l) * 4; }
   printf("  {\"levels\":[");
   for (uint32_t l = 0; l < im->levels; l++) {
      uint64_t off = 0;
      for (uint32_t k = 0; k < l; k++) off += (uint64_t)lw(im->w, k) * lw(im->h, k) * 4;
      uint32_t n = lw(im->w, l) * lw(im->h, l) * 4;
      uint32_t badpx = 0, first = ~0u;
      for (uint32_t i = 0; i < n; i += 4)
         if (memcmp(got + off + i, exp + off + i, 4)) { badpx++; if (first == ~0u) first = i / 4; }
      if (badpx) bad++;
      printf("%s{\"L%u\":\"%ux%u\",\"bad\":%u,\"first\":%d,\"got\":[%u,%u,%u,%u],\"exp\":[%u,%u,%u,%u]}", l ? "," : "", l,
             lw(im->w, l), lw(im->h, l), badpx, first < n / 4 ? (int)first : -1,
             got[off + (first < n / 4 ? first * 4 : 0)], got[off + (first < n / 4 ? first * 4 + 1 : 1)],
             got[off + (first < n / 4 ? first * 4 + 2 : 2)], got[off + (first < n / 4 ? first * 4 + 3 : 3)],
             exp[off + (first < n / 4 ? first * 4 : 0)], exp[off + (first < n / 4 ? first * 4 + 1 : 1)],
             exp[off + (first < n / 4 ? first * 4 + 2 : 2)], exp[off + (first < n / 4 ? first * 4 + 3 : 3)]);
   }
   (void)expect_of;
   printf("],\"badlevels\":%d}\n", bad);
   free(exp);
   return bad;
}

/* ---- sampling pass: textureLod(L) on image B, NEAREST/NEAREST, 64x64 ---- */
struct gfx { VkRenderPass rp; VkFramebuffer fb; VkPipeline pipe, p_ramp, p_grad, p_lod, p_grad2, p_deriv; VkPipelineLayout pl; VkDescriptorSetLayout dsl;
             VkDescriptorPool dp; VkDescriptorSet ds; VkSampler smp; VkImageView view; struct img target; struct buf *rb; };

static VkShaderModule shader(const char *name)
{
   char path[512];
   snprintf(path, sizeof path, "%s/%s.spv", g_spv, name);
   FILE *f = fopen(path, "rb");
   if (!f) { fprintf(stderr, "no %s\n", path); exit(2); }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   void *code = malloc(n);
   if (fread(code, 1, n, f) != (size_t)n) exit(2);
   fclose(f);
   VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = code};
   VkShaderModule m = VK_NULL_HANDLE;
   vkCreateShaderModule(dev, &ci, NULL, &m);
   free(code);
   return m;
}

/* render level `lod` of `im` into a 64x64 RGBA8 target, read it back into rb, return 1 if every pixel != expected */
static int sample_level(struct gfx *g, struct img *im, float lod, uint8_t want[4], uint8_t got_first[4])
{
   VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
   VkImageSubresourceRange rng1 = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                             .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                             .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                             .image = g->target.i, .subresourceRange = rng1};
   (void)rng;
   VkCommandBuffer cb = cb_begin();
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   VkClearValue clear = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
   VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = g->rp, .framebuffer = g->fb,
                               .renderArea = {{0, 0}, {RT, RT}}, .clearValueCount = 1, .pClearValues = &clear};
   vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pipe);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pl, 0, 1, &g->ds, 0, NULL);
   const float pcs[2] = {lod, 0.0f};
   vkCmdPushConstants(cb, g->pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pcs, pcs);
   vkCmdDraw(cb, 6, 1, 0, 0);
   vkCmdEndRenderPass(cb);
   VkImageMemoryBarrier b2 = b;
   b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b2.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
   VkBufferImageCopy cp = {.bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {RT, RT, 1}};
   vkCmdCopyImageToBuffer(cb, g->target.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g->rb->b, 1, &cp);
   cb_submit(cb);

   uint8_t *p = g->rb->p;
   uint32_t bad = 0;
   for (uint32_t i = 0; i < RT * RT; i++)
      if (memcmp(p + i * 4, want, 4)) bad++;
   memcpy(got_first, p, 4);
   return bad;
}


/* ---- --ramp: three LOD twins on a coordinate whose gradient rises with x ---- */
static void ramp_draw(struct gfx *g, VkPipeline pipe, float scale)
{
   VkImageSubresourceRange rng1 = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                             .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                             .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .srcAccessMask = 0,
                             .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .image = g->target.i, .subresourceRange = rng1};
   VkCommandBuffer cb = cb_begin();
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   VkClearValue clear = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
   VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = g->rp, .framebuffer = g->fb,
                               .renderArea = {{0, 0}, {RT, RT}}, .clearValueCount = 1, .pClearValues = &clear};
   vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pl, 0, 1, &g->ds, 0, NULL);
   const float pcs[2] = {0.0f, scale};
   vkCmdPushConstants(cb, g->pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pcs, pcs);
   vkCmdDraw(cb, 6, 1, 0, 0);
   vkCmdEndRenderPass(cb);
   VkImageMemoryBarrier b2 = b;
   b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b2.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
   VkBufferImageCopy cp = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {RT, RT, 1}};
   vkCmdCopyImageToBuffer(cb, g->target.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g->rb->b, 1, &cp);
   cb_submit(cb);
}

static int level_of(const uint8_t *px)
{
   int l = ((int)px[0] - 11) / 37;
   if (l < 0) l = 0;
   if (l > 9) l = 9;
   return l;
}

static void ramp_one(struct gfx *g, VkPipeline pipe, const char *twins, float scale)
{
   ramp_draw(g, pipe, scale);
   const uint8_t *row = (const uint8_t *)g->rb->p + (size_t)(RT / 2) * RT * 4;
   char str[RT + 1];
   int obs[16], nobs = 0, prev = level_of(row);
   for (int x = 0; x < RT; x++) {
      int l = level_of(row + (size_t)x * 4);
      str[x] = (char)('0' + l);
      if (l != prev && nobs < 16) { obs[nobs++] = x; prev = l; }
   }
   str[RT] = 0;
   printf("{\"scale\":%.2f,\"twin\":\"%s\",\"levels\":\"%s\",\"transitions\":[", scale, twins, str);
   for (int i = 0; i < nobs; i++) printf("%s%d", i ? "," : "", obs[i]);
   printf("],\"ideal\":[");
   double ideal[8];
   int nid = 0;
   for (int k = 0; k < 8; k++) {
      double xc = 0.70710678 * (double)(1 << k) / scale;   /* lod = k + 0.5 */
      if (xc >= 1.0) break;
      ideal[nid] = xc * RT - 0.5;
      printf("%s%.1f", nid ? "," : "", ideal[nid]);
      nid++;
   }
   printf("],\"dev_px\":[");
   double maxdev = 0;
   for (int i = 0; i < nobs && i < nid; i++) {
      double dv = (double)obs[i] - ideal[i];
      if (dv < 0) dv = -dv;
      if (dv > maxdev) maxdev = dv;
      printf("%s%.1f", i ? "," : "", (double)obs[i] - ideal[i]);
   }
   printf("],\"maxdev_px\":%.1f}\n", maxdev);
}

static void ramp_all(struct gfx *g)
{
   /* one 64x64 image, 8 levels, constant per level (same colours as the sample test) */
   struct img B;
   if (img_new(&B, RT, RT, mip_levels(RT, RT), VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
      printf("{\"fail\":\"ramp image\"}\n");
      return;
   }
   uint64_t total = 0;
   for (uint32_t l = 0; l < mip_levels(RT, RT); l++) total += (uint64_t)lw(RT, l) * lw(RT, l) * 4;
   struct buf ub;
   if (buf_new(&ub, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      printf("{\"fail\":\"ramp buffer\"}\n");
      return;
   }
   uint8_t *p = ub.p;
   for (uint32_t l = 0; l < mip_levels(RT, RT); l++) { const_fill(p, lw(RT, l) * lw(RT, l), l); p += (uint64_t)lw(RT, l) * lw(RT, l) * 4; }
   VkCommandBuffer cb = cb_begin();
   upload_levels(cb, &B, &ub, VK_IMAGE_LAYOUT_UNDEFINED);
   barrier(cb, B.i, VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels(RT, RT), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
   cb_submit(cb);
   VkImageViewCreateInfo bvi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = B.i, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1}};
   vkCreateImageView(dev, &bvi, NULL, &g->view);
   VkDescriptorImageInfo dii = {g->smp, g->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   VkWriteDescriptorSet wd = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g->ds, .dstBinding = 0,
                              .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii};
   vkUpdateDescriptorSets(dev, 1, &wd, 0, NULL);
   const float scales[3] = {1.0f, 2.0f, 4.0f};
   /* direct derivative check: dFdx(u) / analytic, as UNORM (ratio = channel/255*4) */
   ramp_draw(g, g->p_deriv, 4.0f);
   {
      const uint8_t *row = (const uint8_t *)g->rb->p + (size_t)(RT / 2) * RT * 4;
      printf("{\"deriv\":\"scale4\",\"ratio\":[");
      int first = 1;
      for (int x = 8; x < RT; x += 8) {
         double r = (double)row[(size_t)x * 4] / 255.0 * 4.0;
         printf("%s%.2f", first ? "" : ",", r);
         first = 0;
      }
      printf("]}\n");
   }
   for (int i = 0; i < 3; i++) {
      ramp_one(g, g->p_ramp, "implicit", scales[i]);
      ramp_one(g, g->p_grad, "grad_analytic", scales[i]);
      ramp_one(g, g->p_lod, "lod_analytic", scales[i]);
      ramp_one(g, g->p_grad2, "grad_dFdx", scales[i]);
   }
}


/* --create-probe: which images our driver refuses, by usage and level count (no rendering at all). */
static void create_probe(void)
{
   static const struct { const char *name; VkImageUsageFlags use; } U[] = {
      {"dst|sampled", VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT},
      {"src|dst|sampled", VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT},
      {"sampled", VK_IMAGE_USAGE_SAMPLED_BIT},
      {"dst", VK_IMAGE_USAGE_TRANSFER_DST_BIT},
      {"color|sampled", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT},
   };
   static const uint32_t S[4][2] = {{4, 8}, {64, 64}, {128, 128}, {127, 99}};
   for (int i = 0; i < 4; i++)
      for (int u = 0; u < 5; u++)
         for (int lv = 1; lv <= (int)mip_levels(S[i][0], S[i][1]); lv++) {
            if (lv != 1 && lv != (int)mip_levels(S[i][0], S[i][1])) continue;
            VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                    .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {S[i][0], S[i][1], 1}, .mipLevels = (uint32_t)lv,
                                    .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                    .usage = U[u].use, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
            VkImage im = VK_NULL_HANDLE;
            VkResult r = vkCreateImage_(dev, &ci, NULL, &im);
            printf("{\"create\":\"%ux%u\",\"levels\":%d,\"usage\":\"%s\",\"result\":%d}\n", S[i][0], S[i][1], lv, U[u].name, (int)r);
            if (r == VK_SUCCESS) vkDestroyImage_(dev, im, NULL);
         }
}


/* ---- --deriv2: what the quad's lanes see, one fact per draw, into an RGBA32F target ---- */
static void deriv2_all(void)
{
   struct img T;
   if (img_new(&T, RT, RT, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
      printf("{\"fail\":\"deriv2 target\"}\n");
      return;
   }
   struct buf rb;
   if (buf_new(&rb, (VkDeviceSize)RT * RT * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      printf("{\"fail\":\"deriv2 buffer\"}\n");
      return;
   }
   VkAttachmentDescription att = {.format = VK_FORMAT_R32G32B32A32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
   VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar};
   VkRenderPassCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att,
                                 .subpassCount = 1, .pSubpasses = &sp};
   VkRenderPass rp;
   vkCreateRenderPass(dev, &rci, NULL, &rp);
   VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = T.i, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R32G32B32A32_SFLOAT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkImageView tv;
   vkCreateImageView(dev, &vci, NULL, &tv);
   VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1,
                                  .pAttachments = &tv, .width = RT, .height = RT, .layers = 1};
   VkFramebuffer fb;
   vkCreateFramebuffer(dev, &fci, NULL, &fb);

   VkDescriptorSetLayoutBinding dslb = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &dslb};
   VkDescriptorSetLayout dsl;
   vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl);
   VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 2 * sizeof(float)};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   VkPipelineLayout pl;
   vkCreatePipelineLayout(dev, &pli, NULL, &pl);
   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = shader("mipderiv2.vert"), .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = shader("mipderiv2.frag"), .pName = "main"}};
   VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport vp = {0, 0, RT, RT, 0, 1};
   VkRect2D sc = {{0, 0}, {RT, RT}};
   VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp,
                                            .scissorCount = 1, .pScissors = &sc};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
                                                .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
   VkGraphicsPipelineCreateInfo gpci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
                                        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps,
                                        .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cbs,
                                        .layout = pl, .renderPass = rp};
   VkPipeline pipe;
   if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe) != VK_SUCCESS) { printf("{\"fail\":\"deriv2 pipeline\"}\n"); return; }

   for (int row = 0; row < 8; row++) {
      VkCommandBuffer cb = cb_begin();
      VkClearValue clear = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
      VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
                                  .renderArea = {{0, 0}, {RT, RT}}, .clearValueCount = 1, .pClearValues = &clear};
      vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      const float pcs[2] = {(float)row, 0.0f};
      vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pcs, pcs);
      vkCmdDraw(cb, 3, 1, 0, 0);
      vkCmdEndRenderPass(cb);
      VkBufferImageCopy cp = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {RT, RT, 1}};
      vkCmdCopyImageToBuffer(cb, T.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &cp);
      cb_submit(cb);
      const float *f = (const float *)((const uint8_t *)rb.p + ((size_t)(RT / 2) * RT + RT / 2) * 16);
      printf("{\"row\":%d,\"v\":[%.6f,%.6f,%.6f,%.6f]}\n", row, f[0], f[1], f[2], f[3]);
   }
}


/* ---- --bisect: which setup difference makes the interpolated varying half ---- */
static void bisect_all(void)
{
   static const struct { const char *name, *vs, *fs; int verts; } V[] = {
      {"ov 3v loc0 f32", "bis_ov.vert", "bis0.frag", 3},
      {"ov 3v loc1", "bis_ov.vert", "bis1.frag", 3},
      {"ov 1v loc0", "bis_1v.vert", "bis0.frag", 3},
      {"quad exact", "bis_quad.vert", "bis0.frag", 6},
      {"tri sweep-shape", "bis_tri.vert", "bis0.frag", 3},
      {"ov mediump", "bis_med.vert", "bis_med.frag", 3},
   };
   const VkFormat fmts[2] = {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM};
   for (int fi = 0; fi < 2; fi++) {
      struct img T;
      if (img_new(&T, RT, RT, 1, fmts[fi], VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
         printf("{\"fail\":\"bisect target\"}\n");
         return;
      }
      const uint32_t bpp = fmts[fi] == VK_FORMAT_R8G8B8A8_UNORM ? 4 : 16;
      struct buf rb;
      if (buf_new(&rb, (VkDeviceSize)RT * RT * bpp, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
         printf("{\"fail\":\"bisect buffer\"}\n");
         return;
      }
      VkAttachmentDescription att = {.format = fmts[fi], .samples = VK_SAMPLE_COUNT_1_BIT,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                     .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
      VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar};
      VkRenderPassCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att,
                                    .subpassCount = 1, .pSubpasses = &sp};
      VkRenderPass rp;
      vkCreateRenderPass(dev, &rci, NULL, &rp);
      VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = T.i, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                   .format = fmts[fi], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      VkImageView tv;
      vkCreateImageView(dev, &vci, NULL, &tv);
      VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1,
                                     .pAttachments = &tv, .width = RT, .height = RT, .layers = 1};
      VkFramebuffer fb;
      vkCreateFramebuffer(dev, &fci, NULL, &fb);
      for (unsigned v = 0; v < sizeof V / sizeof V[0]; v++) {
         VkPipelineShaderStageCreateInfo st[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = shader(V[v].vs), .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = shader(V[v].fs), .pName = "main"}};
         VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
         VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
         VkViewport vp = {0, 0, RT, RT, 0, 1};
         VkRect2D sc = {{0, 0}, {RT, RT}};
         VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp,
                                                  .scissorCount = 1, .pScissors = &sc};
         VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
                                                      .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
         VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
         VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
         VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
         VkGraphicsPipelineCreateInfo gpci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
                                              .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps,
                                              .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cbs,
                                              .layout = VK_NULL_HANDLE, .renderPass = rp};
         VkPipeline pipe;
         if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe) != VK_SUCCESS) { printf("{\"fail\":\"bisect pipeline\"}\n"); return; }
         VkCommandBuffer cb = cb_begin();
         VkClearValue clear = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
         VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
                                     .renderArea = {{0, 0}, {RT, RT}}, .clearValueCount = 1, .pClearValues = &clear};
         vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
         vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
         vkCmdDraw(cb, (uint32_t)V[v].verts, 1, 0, 0);
         vkCmdEndRenderPass(cb);
         VkBufferImageCopy cp = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {RT, RT, 1}};
         vkCmdCopyImageToBuffer(cb, T.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &cp);
         cb_submit(cb);
         const uint8_t *px = (const uint8_t *)rb.p + ((size_t)(RT / 2) * RT + RT / 2) * bpp;
         double u, du;
         if (bpp == 4) { u = px[0] / 255.0; du = px[1] / 255.0; }
         else { const float *f = (const float *)px; u = f[0]; du = f[1]; }
         printf("{\"fmt\":\"%s\",\"variant\":\"%s\",\"u\":%.6f,\"dudx\":%.6f}\n",
                bpp == 4 ? "rgba8" : "rgba32f", V[v].name, u, du);
         vkDestroyPipeline(dev, pipe, NULL);
      }
   }
}


/* ---- --edge: derivative quality across a SHARED triangle edge vs a lone triangle edge ---- */
static void edge_all(void)
{
   /* quad = both halves in one draw (shared diagonal); the two single-triangle cases have the same
    * diagonal as an outer edge, which is where helper lanes are needed. */
   static const struct { const char *name, *vs, *fs; int verts; } C[] = {
      {"shared quad (2 tris)", "bis_quad.vert", "edge.frag", 6},
      {"single lower-left tri", "edge_tri1.vert", "edge.frag", 3},
      {"single upper-right tri", "edge_tri2.vert", "edge.frag", 3},
      {"flat varying, shared quad", "bis_quad.vert", "edge_flat.frag", 6},
   };
   struct img T;
   if (img_new(&T, RT, RT, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
      printf("{\"fail\":\"edge target\"}\n");
      return;
   }
   struct buf rb;
   if (buf_new(&rb, (VkDeviceSize)RT * RT * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      printf("{\"fail\":\"edge buffer\"}\n");
      return;
   }
   VkAttachmentDescription att = {.format = VK_FORMAT_R32G32B32A32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
   VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar};
   VkRenderPassCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att,
                                 .subpassCount = 1, .pSubpasses = &sp};
   VkRenderPass rp;
   vkCreateRenderPass(dev, &rci, NULL, &rp);
   VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = T.i, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R32G32B32A32_SFLOAT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkImageView tv;
   vkCreateImageView(dev, &vci, NULL, &tv);
   VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1,
                                  .pAttachments = &tv, .width = RT, .height = RT, .layers = 1};
   VkFramebuffer fb;
   vkCreateFramebuffer(dev, &fci, NULL, &fb);
   for (unsigned c = 0; c < sizeof C / sizeof C[0]; c++) {
      VkPipelineShaderStageCreateInfo st[2] = {
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = shader(C[c].vs), .pName = "main"},
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = shader(C[c].fs), .pName = "main"}};
      VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
      VkViewport vp = {0, 0, RT, RT, 0, 1};
      VkRect2D sc = {{0, 0}, {RT, RT}};
      VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp,
                                               .scissorCount = 1, .pScissors = &sc};
      VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
                                                   .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
      VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
      VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
      VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
      VkGraphicsPipelineCreateInfo gpci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
                                           .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps,
                                           .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cbs,
                                           .layout = VK_NULL_HANDLE, .renderPass = rp};
      VkPipeline pipe;
      if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe) != VK_SUCCESS) { printf("{\"fail\":\"edge pipeline\"}\n"); return; }
      VkCommandBuffer cb = cb_begin();
      VkClearValue clear = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
      VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
                                  .renderArea = {{0, 0}, {RT, RT}}, .clearValueCount = 1, .pClearValues = &clear};
      vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      vkCmdDraw(cb, (uint32_t)C[c].verts, 1, 0, 0);
      vkCmdEndRenderPass(cb);
      VkBufferImageCopy cp = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {RT, RT, 1}};
      vkCmdCopyImageToBuffer(cb, T.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &cp);
      cb_submit(cb);
      printf("{\"case\":\"%s\",\"row32_dudx_24_40\":[", C[c].name);
      for (int x = 24; x <= 40; x++) {
         const float *f = (const float *)((const uint8_t *)rb.p + ((size_t)(RT / 2) * RT + (size_t)x) * 16);
         printf("%s%.2f", x == 24 ? "" : ",", f[1]);
      }
      printf("]}\n");
   }
}

int main(int argc, char **argv)
{
   const char *icd = NULL;
   static uint32_t sizes[8][2] = {{4, 8}, {32, 64}, {127, 99}, {128, 128}};
   int nsizes = 4;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) g_spv = argv[++i];
      else if (!strcmp(argv[i], "--ramp")) g_ramp = 1;
      else if (!strcmp(argv[i], "--create-probe")) g_probe = 1;
      else if (!strcmp(argv[i], "--deriv2")) g_deriv2 = 1;
      else if (!strcmp(argv[i], "--bisect")) g_bisect = 1;
      else if (!strcmp(argv[i], "--edge")) g_edge = 1;
      else { fprintf(stderr, "usage: t_vk_mip --direct icd.so --spv-dir dir\n"); return 2; }
   }
   if (!icd) { fprintf(stderr, "--direct required\n"); return 2; }
   setvbuf(stdout, NULL, _IONBF, 0);

   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("{\"fail\":\"dlopen: %s\"}\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   VkResult (*negotiate)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   uint32_t iv = 5;
   if (!gipa || !negotiate || negotiate(&iv)) { printf("{\"fail\":\"icd entry\"}\n"); return 1; }
   printf("{\"stage\":\"icd\"}\n");
   PFN_vkCreateInstance CI = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   if (!CI) { printf("{\"fail\":\"no CreateInstance\"}\n"); return 1; }
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   if (CI(&ici, NULL, &inst) != VK_SUCCESS) { printf("{\"fail\":\"CreateInstance\"}\n"); return 1; }
   printf("{\"stage\":\"instance\"}\n");
   /* instance-level entry points can only be looked up once the instance exists */
   PFN_vkEnumeratePhysicalDevices EPD = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceMemoryProperties GMP = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
   PFN_vkCreateDevice CD = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr GDPA = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   PFN_vkGetPhysicalDeviceQueueFamilyProperties GFQ = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
   if (!EPD || !GMP || !CD || !GDPA || !GFQ) { printf("{\"fail\":\"instance entry points\"}\n"); return 1; }
   uint32_t n = 1;
   { VkResult rr = EPD(inst, &n, &pd); printf("{\"epd\":%d,\"count\":%u,\"pd\":%llu}\n", (int)rr, n, (unsigned long long)(uintptr_t)pd);
     if ((rr != VK_SUCCESS && rr != VK_INCOMPLETE) || n < 1) { printf("{\"fail\":\"no device\"}\n"); return 1; } }
   GMP(pd, &mp);
   printf("{\"stage\":\"physical\"}\n");
   float prio = 1.0f;
   uint32_t fam = 0;
   VkQueueFamilyProperties qp;
   uint32_t nq = 1;
   GFQ(pd, &nq, &qp);
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = fam, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
   printf("{\"stage\":\"qfam\"}\n");
   if (CD(pd, &dci, NULL, &dev) != VK_SUCCESS) { printf("{\"fail\":\"CreateDevice\"}\n"); return 1; }
#define LOAD(f) f##_ = (PFN_##f)GDPA(dev, #f); if (!f##_) { printf("{\"fail\":\"%s\"}\n", #f); return 1; }
   FNS(LOAD)
#undef LOAD
   printf("{\"stage\":\"device\"}\n");
   vkGetDeviceQueue(dev, fam, 0, &q);
   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = fam};
   vkCreateCommandPool(dev, &pci, NULL, &pool);

   /* ---- graphics objects for the sampling pass ---- */
   struct gfx g;
   memset(&g, 0, sizeof g);
   VkAttachmentDescription att = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
   VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar};
   VkRenderPassCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att,
                                 .subpassCount = 1, .pSubpasses = &sp};
   vkCreateRenderPass(dev, &rci, NULL, &g.rp);
   if (img_new(&g.target, RT, RT, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
      printf("{\"fail\":\"target image\"}\n"); return 1;
   }
   VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g.target.i, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkImageView tv;
   vkCreateImageView(dev, &vci, NULL, &tv);
   VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = g.rp, .attachmentCount = 1,
                                  .pAttachments = &tv, .width = RT, .height = RT, .layers = 1};
   vkCreateFramebuffer(dev, &fci, NULL, &g.fb);
   VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .minLod = 0.0f, .maxLod = 1000.0f};
   vkCreateSampler(dev, &sci, NULL, &g.smp);
   VkDescriptorSetLayoutBinding dslb = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &dslb};
   vkCreateDescriptorSetLayout(dev, &dli, NULL, &g.dsl);
   VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 2 * sizeof(float)};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g.dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   vkCreatePipelineLayout(dev, &pli, NULL, &g.pl);
   VkDescriptorPoolSize dps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps};
   vkCreateDescriptorPool(dev, &dpi, NULL, &g.dp);
   VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g.dp,
                                       .descriptorSetCount = 1, .pSetLayouts = &g.dsl};
   vkAllocateDescriptorSets(dev, &dsai, &g.ds);

   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = shader("mip.vert"), .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = shader("mip.frag"), .pName = "main"}};
   VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport vp = {0, 0, RT, RT, 0, 1};
   VkRect2D sc = {{0, 0}, {RT, RT}};
   VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp,
                                            .scissorCount = 1, .pScissors = &sc};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
                                                .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
   VkGraphicsPipelineCreateInfo gpci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
                                        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps,
                                        .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cbs,
                                        .layout = g.pl, .renderPass = g.rp};
   printf("{\"stage\":\"pipeline\"}\n");
   if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &g.pipe) != VK_SUCCESS) { printf("{\"fail\":\"pipeline\"}\n"); return 1; }
   /* the three LOD twins of --ramp: implicit texture(), textureGrad with analytic gradients, textureLod with the same number */
   VkShaderModule rvs = shader("mipramp_q.vert");
   VkShaderModule rfs = shader("mipramp.frag"), gfs = shader("mipgrad.frag"), lfs = shader("mipramplod.frag"),
                 g2fs = shader("mipgrad2.frag"),
                 dvfs = shader("mipderiv.frag");
   st[0].module = rvs;
   st[1].module = rfs;
   VkPipeline *outs[5] = {&g.p_ramp, &g.p_grad, &g.p_lod, &g.p_grad2, &g.p_deriv};
   VkShaderModule fmods[5] = {rfs, gfs, lfs, g2fs, dvfs};
   for (int k = 0; k < 5; k++) {
      st[1].module = fmods[k];
      if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, outs[k]) != VK_SUCCESS) { printf("{\"fail\":\"ramp pipeline\"}\n"); return 1; }
   }

   g.rb = calloc(1, sizeof *g.rb);
   if (buf_new(g.rb, RT * RT * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      printf("{\"fail\":\"readback buffer\"}\n"); return 1;
   }

   if (g_edge) { edge_all(); printf("{\"done\":true}\n"); return 0; }
   if (g_bisect) { bisect_all(); printf("{\"done\":true}\n"); return 0; }
   if (g_deriv2) { deriv2_all(); printf("{\"done\":true}\n"); return 0; }
   if (g_probe) { create_probe(); printf("{\"done\":true}\n"); return 0; }
   if (g_ramp) { ramp_all(&g); printf("{\"done\":true}\n"); return 0; }

   for (int s = 0; s < nsizes; s++) {
      uint32_t w = sizes[s][0], h = sizes[s][1], lv = mip_levels(w, h);
      uint64_t total = 0;
      for (uint32_t l = 0; l < lv; l++) total += (uint64_t)lw(w, l) * lw(h, l) * 4;

      printf("{\"stage\":\"size %ux%u\",\"levels\":%u}\n", w, h, lv);
      /* ---- A: positional pattern, upload + readback per level ---- */
      struct img A;
      if (img_new(&A, w, h, lv, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
         printf("{\"case\":\"%ux%u\",\"fail\":\"image A\"}\n", w, h); g_fail = 1; continue;
      }
      struct buf ua, ra;
      if (buf_new(&ua, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
          buf_new(&ra, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
         printf("{\"case\":\"%ux%u\",\"fail\":\"buffers A\"}\n", w, h); g_fail = 1; continue;
      }
      uint8_t *p = ua.p;
      for (uint32_t l = 0; l < lv; l++) { pat_fill(p, lw(w, l), lw(h, l), l); p += (uint64_t)lw(w, l) * lw(h, l) * 4; }
      VkCommandBuffer cb = cb_begin();
      upload_levels(cb, &A, &ua, VK_IMAGE_LAYOUT_UNDEFINED);
      cb_submit(cb);
      printf("{\"case\":\"copyback %ux%u\",\"levels\":%u,\"result\":", w, h, lv);
      int bad = readback_levels(&A, &ra, NULL);
      if (bad) g_fail = 1;

      /* ---- B: constant per level, textureLod(L) NEAREST ---- */
      struct img B;
      if (img_new(&B, w, h, lv, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
         printf("{\"case\":\"%ux%u\",\"fail\":\"image B\"}\n", w, h); g_fail = 1; continue;
      }
      struct buf ub;
      if (buf_new(&ub, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
         printf("{\"case\":\"%ux%u\",\"fail\":\"buffer B\"}\n", w, h); g_fail = 1; continue;
      }
      p = ub.p;
      for (uint32_t l = 0; l < lv; l++) { const_fill(p, lw(w, l) * lw(h, l), l); p += (uint64_t)lw(w, l) * lw(h, l) * 4; }
      cb = cb_begin();
      upload_levels(cb, &B, &ub, VK_IMAGE_LAYOUT_UNDEFINED);
      barrier(cb, B.i, VK_IMAGE_ASPECT_COLOR_BIT, 0, lv, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
      cb_submit(cb);

      VkImageViewCreateInfo bvi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = B.i, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                   .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1}};
      vkCreateImageView(dev, &bvi, NULL, &g.view);
      VkDescriptorImageInfo dii = {g.smp, g.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet wd = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g.ds, .dstBinding = 0,
                                 .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii};
      vkUpdateDescriptorSets(dev, 1, &wd, 0, NULL);
      printf("{\"case\":\"sample %ux%u\",\"levels\":[", w, h);
      for (uint32_t l = 0; l < lv; l++) {
         uint8_t want[4] = {(uint8_t)(l * 37 + 11), (uint8_t)(l * 61 + 23), (uint8_t)(l * 91 + 47), 0xff}, got[4] = {0};
         int badpx = sample_level(&g, &B, (float)l, want, got);
         if (badpx) g_fail = 1;
         printf("%s{\"L%u\":%ux%u,\"wrong\":%d,\"got\":[%u,%u,%u,%u]}", l ? "," : "", l, lw(w, l), lw(h, l), badpx,
                got[0], got[1], got[2], got[3]);
      }
      printf("]}\n");
   }
   printf("{\"done\":%s}\n", g_fail ? "false" : "true");
   return g_fail ? 1 : 0;
}
