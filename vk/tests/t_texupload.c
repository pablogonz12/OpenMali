/* SPDX-License-Identifier: MIT */
/* Small-texture diagnostic matrix for the Chromium footer-icon corruption: every path by which a small narrow-texel
 * texture gets its content and is then sampled by a fragment shader. Loader-free (dlopens the ICD).
 *
 *   t_texupload [--direct [icd.so]] [--only upload|param|atlas|reupload|recycle|rt|i2i|i2b|mip|deriv|msaa] [--full] [--quiet]
 *
 * --direct without a path (or no --direct) uses the single dev-so manifest in VK_DRIVER_FILES (smoke --guest-bin):
 * <manifest dir>/libvulkan_mali.so. One "PASS|FAIL <variant>" line each, then "SUMMARY ..."; exit 1 on any FAIL.
 * MALI_BACKEND=fake: nothing executes shaders, so only creation / recording / submission is checked and every line
 * says "(norender)": that run proves the driver accepts the matrix, NOT that one pixel is right. */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "texupload_spv.h"

#define INST_FNS(X) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceProperties) \
   X(GetPhysicalDeviceFormatProperties) \
   X(CreateDevice) X(GetDeviceProcAddr)
#define DEV_FNS(X) X(GetDeviceQueue) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(FlushMappedMemoryRanges) \
   X(InvalidateMappedMemoryRanges) X(CreateFence) X(ResetFences) X(WaitForFences) X(QueueSubmit) X(CreateBuffer) \
   X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) \
   X(BindImageMemory) X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(CreateShaderModule) \
   X(CreateDescriptorSetLayout) X(CreatePipelineLayout) X(CreateDescriptorPool) X(AllocateDescriptorSets) \
   X(UpdateDescriptorSets) X(CreateGraphicsPipelines) X(CreateCommandPool) X(AllocateCommandBuffers) \
   X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) \
   X(CmdCopyImage) X(CmdResolveImage) X(CmdBeginRendering) X(CmdEndRendering) X(CmdBindPipeline) X(CmdBindDescriptorSets) \
   X(CmdPushConstants) X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw) X(CmdClearAttachments) X(DeviceWaitIdle)
#define DECL(n) static PFN_vk##n n;
INST_FNS(DECL) DEV_FNS(DECL)

static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memprops;
static bool fake, quiet;
static unsigned n_pass, n_fail;
static const char *g_only;

#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL setup %s:%d %s = %d\nSUMMARY pass=%u fail=%u setup-error\n", __FILE__, __LINE__, #x, r_, n_pass, n_fail + 1); fflush(stdout); exit(2); } } while (0)

static uint32_t
mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      if ((bits >> i & 1) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      if (bits >> i & 1) return i;
   return 0;
}

/* ---- buffers: one staging (upload) and one readback, both persistently mapped ---- */
struct buf { VkBuffer b; VkDeviceMemory m; uint8_t *p; };
#define BUF_SIZE (8u << 20)
static struct buf up, rb;

static void
make_buf(struct buf *b)
{
   VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = BUF_SIZE,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
   VKC(CreateBuffer(dev, &ci, NULL, &b->b));
   VkMemoryRequirements rq;
   GetBufferMemoryRequirements(dev, b->b, &rq);
   VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size,
      .memoryTypeIndex = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
   VKC(AllocateMemory(dev, &ai, NULL, &b->m));
   VKC(BindBufferMemory(dev, b->b, b->m, 0));
   VKC(MapMemory(dev, b->m, 0, VK_WHOLE_SIZE, 0, (void **)&b->p));
}

/* ---- images ---- */
struct img { VkImage i; VkDeviceMemory m; VkImageView v; uint32_t w, h, levels; VkFormat f; };

static VkImageView
make_view(const struct img *im, uint32_t level, bool alpha_swz)
{
   VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->i, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = im->f, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1 } };
   if (alpha_swz)
      vi.components = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R };
   VkImageView v;
   VKC(CreateImageView(dev, &vi, NULL, &v));
   return v;
}

static struct img
make_img_s(VkFormat f, uint32_t w, uint32_t h, uint32_t levels, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
   struct img im = { .w = w, .h = h, .levels = levels, .f = f };
   VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
      .extent = { w, h, 1 }, .mipLevels = levels, .arrayLayers = 1, .samples = samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage };
   VKC(CreateImage(dev, &ci, NULL, &im.i));
   VkMemoryRequirements rq;
   GetImageMemoryRequirements(dev, im.i, &rq);
   VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size,
                               .memoryTypeIndex = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
   VKC(AllocateMemory(dev, &ai, NULL, &im.m));
   VKC(BindImageMemory(dev, im.i, im.m, 0));
   if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
      im.v = make_view(&im, 0, false);
   } else {
      VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im.i, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                   .format = f, .subresourceRange = { aspect, 0, 1, 0, 1 } };
      VKC(CreateImageView(dev, &vi, NULL, &im.v));
   }
   return im;
}

static struct img
make_img(VkFormat f, uint32_t w, uint32_t h, uint32_t levels, bool color_att)
{
   return make_img_s(f, w, h, levels, VK_SAMPLE_COUNT_1_BIT,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     (color_att ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0), VK_IMAGE_ASPECT_COLOR_BIT);
}

static void
free_img(struct img *im)
{
   DestroyImageView(dev, im->v, NULL);
   DestroyImage(dev, im->i, NULL);
   FreeMemory(dev, im->m, NULL);
}

/* ---- command buffer ---- */
static VkCommandPool pool;
static VkCommandBuffer cb;
static VkFence fence;

static void
begin(void)
{
   VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
   VKC(BeginCommandBuffer(cb, &bi));
}

static void
submit(void)
{
   VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
   VKC(EndCommandBuffer(cb));
   VkMappedMemoryRange mr = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = up.m, .size = VK_WHOLE_SIZE };
   VKC(FlushMappedMemoryRanges(dev, 1, &mr));
   VKC(ResetFences(dev, 1, &fence));
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
   VKC(QueueSubmit(queue, 1, &si, fence));
   VKC(WaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
   mr.memory = rb.m;
   VKC(InvalidateMappedMemoryRanges(dev, 1, &mr));
}

static void
barrier_all(void)
{
   VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void
to_layout(const struct img *im, VkImageLayout from, VkImageLayout to)
{
   VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = from, .newLayout = to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im->i,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, im->levels, 0, 1 } };
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ---- pipelines ---- */
static VkDescriptorSetLayout dsl;
static VkPipelineLayout layout;
#define NSETS 8
static VkDescriptorSet sets[NSETS];
static VkSampler sampler;
static VkPipeline pipe_sample, pipe_deriv, pipe_gen[4], pipe_gen_blend[4];
struct push { int32_t a[4]; float rect[4]; };

static VkShaderModule
module(const uint32_t *code, size_t bytes)
{
   VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code };
   VkShaderModule m;
   VKC(CreateShaderModule(dev, &ci, NULL, &m));
   return m;
}

/* stencil: 0 none, 1 "stencil" pass (INVERT, colour writes off), 2 "cover" pass (NOT_EQUAL 0, zero it, write colour) */
static VkPipeline
make_pipe_s(VkShaderModule vs, VkShaderModule fs, VkFormat fmt, bool blend, VkSampleCountFlagBits samples, int stencil, VkFormat sfmt)
{
   VkPipelineShaderStageCreateInfo st[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
   VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
   VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
   VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
   VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .lineWidth = 1.0f };
   VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = samples };
   VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = stencil == 1 ? 0 : 0xf, .blendEnable = blend,
      .srcColorBlendFactor = VK_BLEND_FACTOR_ONE, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE, .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .alphaBlendOp = VK_BLEND_OP_ADD };
   VkPipelineColorBlendStateCreateInfo bs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba };
   VkDynamicState dy[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
   VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dy };
   VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt,
      .depthAttachmentFormat = sfmt == VK_FORMAT_S8_UINT ? VK_FORMAT_UNDEFINED : sfmt, .stencilAttachmentFormat = sfmt };
   VkStencilOpState so = { .failOp = VK_STENCIL_OP_KEEP, .passOp = stencil == 1 ? VK_STENCIL_OP_INVERT : VK_STENCIL_OP_ZERO,
      .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = stencil == 1 ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_NOT_EQUAL,
      .compareMask = 0xff, .writeMask = 0xff, .reference = 0 };
   VkPipelineDepthStencilStateCreateInfo dss = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .stencilTestEnable = stencil != 0, .front = so, .back = so };
   VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st,
      .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
      .pColorBlendState = &bs, .pDynamicState = &ds, .pDepthStencilState = &dss, .layout = layout };
   VkPipeline p;
   VKC(CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p));
   return p;
}

static VkPipeline
make_pipe(VkShaderModule vs, VkShaderModule fs, VkFormat fmt, bool blend)
{
   return make_pipe_s(vs, fs, fmt, blend, VK_SAMPLE_COUNT_1_BIT, 0, VK_FORMAT_UNDEFINED);
}

/* ---- formats ---- */
struct fmt { VkFormat f; unsigned bs, nc; const char *name; };
static const struct fmt fmts[] = {
   { VK_FORMAT_R8_UNORM, 1, 1, "R8" }, { VK_FORMAT_R8G8_UNORM, 2, 2, "RG8" },
   { VK_FORMAT_R8G8B8A8_UNORM, 4, 4, "RGBA8" }, { VK_FORMAT_B8G8R8A8_UNORM, 4, 4, "BGRA8" } };

/* Distinct byte per (seed, x, y, channel): a wrong offset, pitch, channel or stale texture all change it. */
static uint8_t
pat(unsigned seed, unsigned x, unsigned y, unsigned c)
{
   uint32_t v = (x * 7u + y * 53u + c * 101u + seed * 29u);
   v ^= (v >> 5) ^ (x * y * 3u);
   return (uint8_t)v;
}

/* What a fragment shader must return (as RGBA8) for a texel whose memory bytes are t[0..bs). */
static void
expect_sample(const struct fmt *f, const uint8_t *t, bool aswz, uint8_t out[4])
{
   uint8_t s[4] = { 0, 0, 0, 255 };
   for (unsigned c = 0; c < f->nc; c++) s[c] = t[c];
   if (f->f == VK_FORMAT_B8G8R8A8_UNORM) { s[0] = t[2]; s[2] = t[0]; }
   if (aswz) { out[0] = out[1] = out[2] = 255; out[3] = s[0]; }
   else memcpy(out, s, 4);
}

/* CPU model of an image level: w*h texels of bs bytes, tightly packed. */
static uint8_t *
model_new(const struct fmt *f, unsigned w, unsigned h, unsigned seed)
{
   uint8_t *m = malloc((size_t)w * h * f->bs);
   for (unsigned y = 0; y < h; y++)
      for (unsigned x = 0; x < w; x++)
         for (unsigned c = 0; c < f->bs; c++) m[((size_t)y * w + x) * f->bs + c] = pat(seed, x, y, c);
   return m;
}

/* Write a rw x rh source rect into the staging buffer at `off` with row length `rl` texels, copy it to (ox,oy) of
 * `level`, and mirror it into the model (model width mw). Returns the end offset in the staging buffer. */
static size_t
upload(const struct fmt *f, const struct img *im, uint32_t level, size_t off, uint32_t rl, int ox, int oy, uint32_t rw, uint32_t rh,
       unsigned seed, uint8_t *model, uint32_t mw)
{
   const uint32_t row = rl ? rl : rw;
   for (uint32_t y = 0; y < rh; y++)
      for (uint32_t x = 0; x < row; x++)
         for (unsigned c = 0; c < f->bs; c++) {
            const uint8_t v = x < rw ? pat(seed, x, y, c) : 0xa5;
            up.p[off + ((size_t)y * row + x) * f->bs + c] = v;
            if (x < rw && model) model[((size_t)(oy + y) * mw + ox + x) * f->bs + c] = v;
         }
   VkBufferImageCopy r = { .bufferOffset = off, .bufferRowLength = rl, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
                           .imageOffset = { ox, oy, 0 }, .imageExtent = { rw, rh, 1 } };
   CmdCopyBufferToImage(cb, up.b, im->i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
   return off + (size_t)row * rh * f->bs;
}

static void
render_begin(const struct img *rt, bool load, float clear, uint32_t w, uint32_t h)
{
   VkRenderingAttachmentInfo att = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = rt->v,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.color.float32 = { clear, clear, clear, clear } };
   VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { w, h } }, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &att };
   CmdBeginRendering(cb, &ri);
   VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
   VkRect2D sc = { { 0, 0 }, { w, h } };
   CmdSetViewport(cb, 0, 1, &vp);
   CmdSetScissor(cb, 0, 1, &sc);
}

static void
draw(VkPipeline p, struct push pc)
{
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
   CmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
   CmdDraw(cb, 4, 1, 0, 0);
}

static const struct push full_rect = { .rect = { -1, -1, 1, 1 } };

/* Draw w x h texels of `view` 1:1 into a fresh RGBA8 target and copy that target into the readback buffer at rb_off.
 * `set` must be distinct for each use inside one command buffer. `view` is in SHADER_READ_ONLY_OPTIMAL. */
static struct img
sample_to_rb(VkImageView view, uint32_t w, uint32_t h, unsigned set, size_t rb_off)
{
   struct img rt = make_img(VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, true);
   VkDescriptorImageInfo ii = { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
   VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[set], .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii };
   UpdateDescriptorSets(dev, 1, &wr, 0, NULL);
   to_layout(&rt, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
   render_begin(&rt, false, 0.5f, w, h);
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &sets[set], 0, NULL);
   draw(pipe_sample, full_rect);
   CmdEndRendering(cb);
   to_layout(&rt, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
   VkBufferImageCopy r = { .bufferOffset = rb_off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { w, h, 1 } };
   CmdCopyImageToBuffer(cb, rt.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &r);
   return rt;
}

static void
report(const char *name, unsigned bad, unsigned total, const char *detail)
{
   if (fake) bad = 0;
   if (bad) {
      n_fail++;
      printf("FAIL %s bad=%u/%u %s\n", name, bad, total, detail);
   } else {
      n_pass++;
      if (!quiet) printf("PASS %s%s\n", name, fake ? " (norender)" : "");
   }
   fflush(stdout);
}

struct diff { unsigned bad, total; char detail[200]; };

static void
cmp_px(struct diff *d, unsigned x, unsigned y, const uint8_t *got, const uint8_t *want, unsigned n, unsigned tol)
{
   d->total++;
   for (unsigned c = 0; c < n; c++)
      if ((unsigned)abs((int)got[c] - (int)want[c]) > tol) {
         if (!d->bad)
            snprintf(d->detail, sizeof d->detail, "first=(%u,%u) got=%02x%02x%02x%02x want=%02x%02x%02x%02x", x, y,
                     got[0], n > 1 ? got[1] : 0, n > 2 ? got[2] : 0, n > 3 ? got[3] : 0, want[0], n > 1 ? want[1] : 0,
                     n > 2 ? want[2] : 0, n > 3 ? want[3] : 0);
         d->bad++;
         return;
      }
}

static void
check_sampled(struct diff *d, const struct fmt *f, const uint8_t *model, uint32_t w, uint32_t h, bool aswz, size_t rb_off)
{
   for (uint32_t y = 0; y < h; y++)
      for (uint32_t x = 0; x < w; x++) {
         uint8_t want[4];
         expect_sample(f, model + ((size_t)y * w + x) * f->bs, aswz, want);
         cmp_px(d, x, y, rb.p + rb_off + ((size_t)y * w + x) * 4, want, 4, 0);
      }
}

/* ---- upload -> sample ---- */
static void
v_upload(const char *kind, const struct fmt *f, uint32_t w, uint32_t h, uint32_t boff, uint32_t rl_extra, int ox, int oy, bool aswz)
{
   const uint32_t iw = ox || oy ? w + ox + 2 : w, ih = ox || oy ? h + oy + 2 : h;
   const uint32_t rl = rl_extra ? w + rl_extra : 0;
   struct img im = make_img(f->f, iw, ih, 1, false);
   uint8_t *model = model_new(f, iw, ih, 1);
   memset(up.p, 0x5a, 1 << 20);
   memset(rb.p, 0xcd, (size_t)iw * ih * 4);
   begin();
   to_layout(&im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
   /* background: the whole image, so the border around the sub-rectangle is defined and checked */
   size_t end = upload(f, &im, 0, 0, 0, 0, 0, iw, ih, 1, model, iw);
   barrier_all();
   if (ox || oy || boff || rl)
      upload(f, &im, 0, (end + 255) / 256 * 256 + boff, rl, ox, oy, w, h, 2, model, iw);
   else
      upload(f, &im, 0, (end + 255) / 256 * 256, 0, 0, 0, w, h, 2, model, iw);
   to_layout(&im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
   VkImageView v = make_view(&im, 0, aswz);
   struct img rt = sample_to_rb(v, iw, ih, 0, 0);
   submit();
   struct diff d = { 0 };
   check_sampled(&d, f, model, iw, ih, aswz, 0);
   char name[160];
   snprintf(name, sizeof name, "%s fmt=%s w=%u h=%u bufoff=%u rowlen=%u imgoff=%d,%d swz=%s", kind, f->name, w, h, boff, rl, ox, oy,
            aswz ? "111R" : "id");
   report(name, d.bad, d.total, d.detail);
   free_img(&rt);
   DestroyImageView(dev, v, NULL);
   free_img(&im);
   free(model);
}

static const uint32_t dims[] = { 1, 2, 3, 5, 7, 13, 17, 31, 33, 37, 63, 65, 67 };
#define NDIMS (sizeof dims / sizeof dims[0])

static void
t_upload(bool full)
{
   static const uint32_t offs[] = { 0, 1, 2, 3, 5, 64, 65, 4, 68 };
   static const int io[2][2] = { { 0, 0 }, { 3, 5 } };
   static const uint32_t rls[] = { 0, 1, 3 };
   /* every format x width x height, the other parameters rotating */
   if (!g_only || !strcmp(g_only, "upload")) {
      unsigned k = 0;
      for (unsigned fi = 0; fi < 4; fi++)
         for (unsigned wi = 0; wi < NDIMS; wi++)
            for (unsigned hi = 0; hi < NDIMS; hi++, k++) {
               unsigned oi = k % 9;
               while (offs[oi] % fmts[fi].bs) oi = (oi + 1) % 9;
               v_upload("upload", &fmts[fi], dims[wi], dims[hi], offs[oi], rls[(k / 3) % 3], io[(k / 2) % 2][0], io[(k / 2) % 2][1], k & 1);
            }
   }
   /* full cross of offset x row length x image offset x swizzle at a few sizes (--full: at every w == h size too) */
   if (!g_only || !strcmp(g_only, "param")) {
      static const uint32_t sz[][2] = { { 33, 33 }, { 13, 37 }, { 67, 3 } };
      const unsigned nsz = full ? 3 + NDIMS : 3;
      for (unsigned fi = 0; fi < 4; fi++)
         for (unsigned s = 0; s < nsz; s++)
            for (unsigned oi = 0; oi < 9; oi++)
               for (unsigned ri = 0; ri < 3; ri++)
                  for (unsigned ii = 0; ii < 2; ii++)
                     for (unsigned sw = 0; sw < 2; sw++)
                        if (!(offs[oi] % fmts[fi].bs))
                           v_upload("param", &fmts[fi], s < 3 ? sz[s][0] : dims[s - 3], s < 3 ? sz[s][1] : dims[s - 3], offs[oi], rls[ri],
                                    io[ii][0], io[ii][1], sw);
   }
}

/* Glyph / path atlas: many abutting sub-rectangles of odd widths (neighbours share destination words) in ONE command
 * buffer, without barriers between them, then one with an approx-fit style partial upload. */
static void
t_atlas(void)
{
   for (unsigned fi = 0; fi < 2; fi++)
      for (unsigned rep = 0; rep < 8; rep++) {
         const struct fmt *f = &fmts[fi];
         const uint32_t S = 128;
         struct img im = make_img(f->f, S, S, 1, false);
         uint8_t *model = model_new(f, S, S, 1);
         begin();
         to_layout(&im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
         size_t off = upload(f, &im, 0, 0, 0, 0, 0, S, S, 1, model, S);
         barrier_all();
         unsigned seed = 3 + rep;
         for (uint32_t y = 0, row = 0; y + 11 <= S; y += 11, row++)
            for (uint32_t x = 0, i = 0;; i++) {
               const uint32_t w = 1 + (i * 5 + row * 3 + rep) % 13, h = rep & 1 ? 11 : 1 + (i + rep) % 11;
               if (x + w > S) break;
               off = (off + f->bs - 1) / f->bs * f->bs + (rep >= 4 ? (size_t)(i % 4) * f->bs : 0);
               off = upload(f, &im, 0, off, 0, x, y, w, h, seed++, model, S);
               x += w;
            }
         to_layout(&im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
         VkImageView v = make_view(&im, 0, rep & 1);
         struct img rt = sample_to_rb(v, S, S, 0, 0);
         submit();
         struct diff d = { 0 };
         check_sampled(&d, f, model, S, S, rep & 1, 0);
         char name[80];
         snprintf(name, sizeof name, "atlas fmt=%s rep=%u", f->name, rep);
         report(name, d.bad, d.total, d.detail);
         free_img(&rt);
         DestroyImageView(dev, v, NULL);
         free_img(&im);
         free(model);
      }
}

/* Upload, sample, upload new content into the same image, sample again: stale texture / L2 lines. mode 0: two command
 * buffers, mode 1: one command buffer. */
static void
t_reupload(void)
{
   static const uint32_t sz[] = { 5, 17, 33, 65, 128 };
   for (unsigned fi = 0; fi < 4; fi += 2)
      for (unsigned s = 0; s < 5; s++)
         for (unsigned mode = 0; mode < 2; mode++) {
            const struct fmt *f = &fmts[fi];
            const uint32_t S = sz[s];
            struct img im = make_img(f->f, S, S, 1, false);
            VkImageView v = make_view(&im, 0, false);
            struct img rt[4];
            struct diff d = { 0 };
            uint8_t *m[4];
            size_t off = 0;
            begin();
            for (unsigned it = 0; it < 4; it++) {
               if (mode == 0 && it) begin();
               m[it] = model_new(f, S, S, 1);
               to_layout(&im, it ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
               off = upload(f, &im, 0, (off + 3) & ~(size_t)3, 0, 0, 0, S, S, 10 + it * 17, m[it], S);
               to_layout(&im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
               rt[it] = sample_to_rb(v, S, S, mode ? it : 0, (size_t)it * S * S * 4);
               if (mode == 0) {
                  submit();
                  check_sampled(&d, f, m[it], S, S, false, (size_t)it * S * S * 4);
               }
            }
            if (mode == 1) {
               submit();
               for (unsigned it = 0; it < 4; it++) check_sampled(&d, f, m[it], S, S, false, (size_t)it * S * S * 4);
            }
            char name[96];
            snprintf(name, sizeof name, "reupload fmt=%s size=%u %s", f->name, S, mode ? "one-cmdbuf" : "per-submit");
            report(name, d.bad, d.total, d.detail);
            for (unsigned it = 0; it < 4; it++) { free_img(&rt[it]); free(m[it]); }
            DestroyImageView(dev, v, NULL);
            free_img(&im);
         }
}

/* Many short-lived mask textures (SoftwarePathRenderer / scratch surfaces): create, upload, sample, destroy; the next
 * one tends to reuse the same GPU address. */
static void
t_recycle(void)
{
   for (unsigned fi = 0; fi < 4; fi += 2) {
      const struct fmt *f = &fmts[fi];
      struct diff d = { 0 };
      for (unsigned it = 0; it < 120; it++) {
         const uint32_t w = it % 3 ? 64 : 33, h = it % 5 ? 64 : 31;
         struct img im = make_img(f->f, w, h, 1, false);
         uint8_t *m = model_new(f, w, h, 1);
         begin();
         to_layout(&im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
         upload(f, &im, 0, 4 * (it % 7), 0, 0, 0, w, h, 100 + it, m, w);
         to_layout(&im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
         struct img rt = sample_to_rb(im.v, w, h, 0, 0);
         submit();
         check_sampled(&d, f, m, w, h, false, 0);
         free_img(&rt);
         free_img(&im);
         free(m);
      }
      char name[64];
      snprintf(name, sizeof name, "recycle fmt=%s x120", f->name);
      report(name, d.bad, d.total, d.detail);
   }
}

/* Coverage mask rendered on the GPU into a narrow render target, then sampled. gen.frag writes channel c =
 * ((x*7 + y*13 + c*61 + seed*29) & 255)/255. mode 0: clear + draw; 1: + second render pass with loadOp LOAD adding a
 * constant by blending on the right half; 2: + scissored vkCmdClearAttachments on a sub-rectangle. */
static void
t_rt(void)
{
   static const uint32_t sz[][2] = { { 1, 1 }, { 3, 5 }, { 7, 13 }, { 17, 17 }, { 31, 33 }, { 33, 31 }, { 37, 63 }, { 64, 64 },
                                     { 65, 65 }, { 67, 7 }, { 128, 128 }, { 256, 64 }, { 257, 129 } };
   for (unsigned fi = 0; fi < 4; fi++)
      for (unsigned s = 0; s < sizeof sz / sizeof sz[0]; s++)
         for (unsigned mode = 0; mode < 3; mode++) {
            const struct fmt *f = &fmts[fi];
            const uint32_t w = sz[s][0], h = sz[s][1];
            struct img im = make_img(f->f, w, h, 1, true);
            uint8_t *m = malloc((size_t)w * h * f->bs);
            for (uint32_t y = 0; y < h; y++)
               for (uint32_t x = 0; x < w; x++)
                  for (unsigned c = 0; c < f->nc; c++) {
                     unsigned byte = f->f == VK_FORMAT_B8G8R8A8_UNORM && c != 1 && c != 3 ? 2 - c : c;
                     unsigned v = (x * 7 + y * 13 + c * 61 + s * 29) & 127;
                     if (mode == 1 && x >= w / 2) v += 64;
                     if (mode == 2 && x >= w / 4 && x < w / 4 + (w + 1) / 2 && y >= h / 4 && y < h / 4 + (h + 1) / 2) v = 255;
                     m[((size_t)y * w + x) * f->bs + byte] = (uint8_t)v;
                  }
            begin();
            to_layout(&im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            render_begin(&im, false, 0.0f, w, h);
            draw(pipe_gen[fi], (struct push){ .a = { (int)s, 0, 127, 0 }, .rect = { -1, -1, 1, 1 } });
            if (mode == 2) {
               VkClearAttachment ca = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .clearValue.color.float32 = { 1, 1, 1, 1 } };
               VkClearRect cr = { { { (int)(w / 4), (int)(h / 4) }, { (w + 1) / 2, (h + 1) / 2 } }, 0, 1 };
               CmdClearAttachments(cb, 1, &ca, 1, &cr);
            }
            CmdEndRendering(cb);
            if (mode == 1 && w >= 2) {
               barrier_all();
               render_begin(&im, true, 0.0f, w, h);
               VkRect2D sc = { { (int)(w / 2), 0 }, { w - w / 2, h } };
               CmdSetScissor(cb, 0, 1, &sc);
               draw(pipe_gen_blend[fi], (struct push){ .a = { 64, 1, 0, 0 }, .rect = { -1, -1, 1, 1 } });
               CmdEndRendering(cb);
            } else if (mode == 1) {
               for (uint32_t y = 0; y < h; y++)   /* w == 1: no right half pass */
                  for (unsigned c = 0; c < f->bs; c++) m[(size_t)y * w * f->bs + c] -= 64;
            }
            to_layout(&im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            struct img rt = sample_to_rb(im.v, w, h, 0, 0);
            submit();
            struct diff d = { 0 };
            check_sampled(&d, f, m, w, h, false, 0);
            char name[96];
            snprintf(name, sizeof name, "rt fmt=%s w=%u h=%u %s", f->name, w, h,
                     mode == 0 ? "draw" : mode == 1 ? "load-blend" : "clear-rect");
            report(name, d.bad, d.total, d.detail);
            free_img(&rt);
            free_img(&im);
            free(m);
         }
}

/* vkCmdCopyImage between narrow images with odd offsets (glCopyTexSubImage2D / atlas growth), and image -> buffer. */
static void
t_i2i_i2b(void)
{
   static const uint32_t sz[][2] = { { 1, 1 }, { 3, 7 }, { 13, 13 }, { 31, 5 }, { 33, 33 }, { 63, 17 }, { 65, 67 } };
   static const int off[][4] = { { 0, 0, 0, 0 }, { 1, 0, 0, 0 }, { 0, 0, 3, 5 }, { 2, 3, 1, 1 }, { 5, 1, 64, 2 } };
   for (unsigned fi = 0; fi < 4; fi++)
      for (unsigned s = 0; s < 7; s++)
         for (unsigned o = 0; o < 5; o++) {
            const struct fmt *f = &fmts[fi];
            const uint32_t w = sz[s][0], h = sz[s][1], S = 144;
            const int sx = off[o][0], sy = off[o][1], dx = off[o][2], dy = off[o][3];
            if (!g_only || !strcmp(g_only, "i2i")) {
               struct img a = make_img(f->f, S, S, 1, false), b = make_img(f->f, S, S, 1, false);
               uint8_t *ma = model_new(f, S, S, 1), *mb = model_new(f, S, S, 1);
               begin();
               to_layout(&a, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
               to_layout(&b, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
               size_t e = upload(f, &a, 0, 0, 0, 0, 0, S, S, 21, ma, S);
               upload(f, &b, 0, e, 0, 0, 0, S, S, 99, mb, S);
               to_layout(&a, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
               barrier_all();
               VkImageCopy r = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { sx, sy, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { dx, dy, 0 }, { w, h, 1 } };
               CmdCopyImage(cb, a.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
               for (uint32_t y = 0; y < h; y++)
                  memcpy(mb + ((size_t)(dy + y) * S + dx) * f->bs, ma + ((size_t)(sy + y) * S + sx) * f->bs, (size_t)w * f->bs);
               to_layout(&b, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
               struct img rt = sample_to_rb(b.v, S, S, 0, 0);
               submit();
               struct diff d = { 0 };
               check_sampled(&d, f, mb, S, S, false, 0);
               char name[96];
               snprintf(name, sizeof name, "i2i fmt=%s w=%u h=%u src=%d,%d dst=%d,%d", f->name, w, h, sx, sy, dx, dy);
               report(name, d.bad, d.total, d.detail);
               free_img(&rt); free_img(&a); free_img(&b); free(ma); free(mb);
            }
            if (!g_only || !strcmp(g_only, "i2b")) {
               struct img a = make_img(f->f, S, S, 1, false);
               uint8_t *ma = model_new(f, S, S, 1);
               const uint32_t boff = (uint32_t)(f->bs == 1 ? (o * 3 + 1) : (o * 2 + 1) * f->bs), rl = o & 1 ? w + 3 : 0;
               memset(rb.p, 0xee, 1 << 16);
               begin();
               to_layout(&a, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
               upload(f, &a, 0, 0, 0, 0, 0, S, S, 21, ma, S);
               to_layout(&a, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
               VkBufferImageCopy r = { .bufferOffset = boff, .bufferRowLength = rl, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                       .imageOffset = { dx % 64, dy, 0 }, .imageExtent = { w, h, 1 } };
               CmdCopyImageToBuffer(cb, a.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &r);
               submit();
               struct diff d = { 0 };
               const uint32_t row = rl ? rl : w;
               const size_t total = (size_t)boff + (size_t)row * h * f->bs + 8;
               uint8_t *want = malloc(total);
               memset(want, 0xee, total);
               for (uint32_t y = 0; y < h; y++)
                  memcpy(want + boff + (size_t)y * row * f->bs, ma + ((size_t)(dy + y) * S + dx % 64) * f->bs, (size_t)w * f->bs);
               for (size_t i = 0; i < total; i++) cmp_px(&d, (unsigned)i, 0, rb.p + i, want + i, 1, 0);
               char name[96];
               snprintf(name, sizeof name, "i2b fmt=%s w=%u h=%u bufoff=%u rowlen=%u", f->name, w, h, boff, rl);
               report(name, d.bad, d.total, d.detail);
               free(want); free_img(&a); free(ma);
            }
         }
}

static void
t_mip(void)
{
   static const uint32_t base[][2] = { { 65, 65 }, { 33, 17 }, { 128, 64 }, { 67, 3 } };
   for (unsigned fi = 0; fi < 4; fi++)
      for (unsigned s = 0; s < 4; s++) {
         const struct fmt *f = &fmts[fi];
         uint32_t m = base[s][0] > base[s][1] ? base[s][0] : base[s][1], levels = 1;
         while (m >>= 1) levels++;
         struct img im = make_img(f->f, base[s][0], base[s][1], levels, false);
         uint8_t *models[16];
         begin();
         to_layout(&im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
         size_t off = 0;
         for (uint32_t l = 0; l < levels; l++) {
            const uint32_t w = im.w >> l ? im.w >> l : 1, h = im.h >> l ? im.h >> l : 1;
            models[l] = model_new(f, w, h, 1);
            off = upload(f, &im, l, (off + 3) & ~(size_t)3, 0, 0, 0, w, h, 40 + l, models[l], w);
         }
         to_layout(&im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
         for (uint32_t l = 0; l < levels; l++) {
            const uint32_t w = im.w >> l ? im.w >> l : 1, h = im.h >> l ? im.h >> l : 1;
            if (l) begin();
            VkImageView v = make_view(&im, l, false);
            struct img rt = sample_to_rb(v, w, h, 0, 0);
            submit();
            struct diff d = { 0 };
            check_sampled(&d, f, models[l], w, h, false, 0);
            char name[96];
            snprintf(name, sizeof name, "mip fmt=%s base=%ux%u level=%u", f->name, im.w, im.h, l);
            report(name, d.bad, d.total, d.detail);
            free_img(&rt);
            DestroyImageView(dev, v, NULL);
            free(models[l]);
         }
         free_img(&im);
      }
}

/* What Skia's distance-field / coverage path shaders rely on: derivatives of an unnormalised varying, fwidth, a flat
 * integer varying, on quads at odd positions (helper invocations at the edges). Expect R=dFdx(st.x) G=dFdy(st.y)
 * B=fwidth(st.x+st.y)/2 each = 1.0 -> 0x80 after the 0.5 scale; A = flat int. */
static void
t_deriv(void)
{
   const uint32_t S = 128;
   for (unsigned q = 0; q < 12; q++) {
      const uint32_t x0 = 1 + q * 3, y0 = 2 + q * 5, w = 1 + q * 7 % 41, h = 1 + q * 11 % 37;
      struct img rt = make_img(VK_FORMAT_R8G8B8A8_UNORM, S, S, 1, true);
      begin();
      to_layout(&rt, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      render_begin(&rt, false, 0.0f, S, S);
      draw(pipe_deriv, (struct push){ .a = { (int)(40 + q), (int)w, (int)h, 0 },
           .rect = { 2.0f * x0 / S - 1, 2.0f * y0 / S - 1, 2.0f * (x0 + w) / S - 1, 2.0f * (y0 + h) / S - 1 } });
      CmdEndRendering(cb);
      to_layout(&rt, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { S, S, 1 } };
      CmdCopyImageToBuffer(cb, rt.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &r);
      submit();
      struct diff d = { 0 };
      for (uint32_t y = 0; y < S; y++)
         for (uint32_t x = 0; x < S; x++) {
            uint8_t want[4] = { 0, 0, 0, 0 };
            if (x >= x0 && x < x0 + w && y >= y0 && y < y0 + h) { want[0] = want[1] = want[2] = 128; want[3] = (uint8_t)(40 + q); }
            cmp_px(&d, x, y, rb.p + ((size_t)y * S + x) * 4, want, 4, 1);
         }
      char name[80];
      snprintf(name, sizeof name, "deriv quad=%u,%u %ux%u", x0, y0, w, h);
      report(name, d.bad, d.total, d.detail);
      free_img(&rt);
   }
}

/* ---- 4x MSAA: what Skia's atlas / tessellation path renderers and "dynamic MSAA" do when the driver advertises 4x ---- */
static VkPhysicalDevice g_pd;
static VkShaderModule m_vs, m_gen;

/* Samples (standard 4x locations) of pixel (x,y) inside the half-open rectangle r = {x0,y0,x1,y1}; bit per sample.
 * Rectangle edges sit on multiples of 0.5, where no sample does. */
static unsigned
cover_mask(uint32_t x, uint32_t y, const float r[4])
{
   static const float sp[4][2] = { { .375f, .125f }, { .875f, .375f }, { .125f, .625f }, { .625f, .875f } };
   unsigned m = 0;
   for (unsigned s = 0; s < 4; s++) {
      const float px = x + sp[s][0], py = y + sp[s][1];
      if (px >= r[0] && px < r[2] && py >= r[1] && py < r[3]) m |= 1u << s;
   }
   return m;
}

static void
rect_push(struct push *p, const float r[4], uint32_t w, uint32_t h)
{
   p->rect[0] = 2.0f * r[0] / w - 1; p->rect[1] = 2.0f * r[1] / h - 1;
   p->rect[2] = 2.0f * r[2] / w - 1; p->rect[3] = 2.0f * r[3] / h - 1;
}

enum { MS_PATTERN, MS_PATTERN_CMD, MS_EDGE, MS_EDGE_CMD, MS_LOAD, MS_STENCIL, MS_AREA, MS_DISCARD, MS_NMODES };
static const char *const ms_names[] = { "pattern-resolve", "pattern-cmdresolve", "edge-resolve", "edge-cmdresolve", "load-resolve",
                                        "stencil-cover", "subarea-resolve", "pattern-resolve-discard" };

static void
t_msaa(void)
{
   static const uint32_t sz[][2] = { { 5, 7 }, { 17, 17 }, { 33, 31 }, { 64, 64 }, { 65, 67 }, { 128, 32 }, { 256, 256 }, { 512, 512 } };
   VkFormat sfmt = VK_FORMAT_UNDEFINED;
   const VkFormat cand[] = { VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
   for (unsigned i = 0; i < 3 && !sfmt; i++) {
      VkFormatProperties fp;
      GetPhysicalDeviceFormatProperties(g_pd, cand[i], &fp);
      if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) sfmt = cand[i];
   }
   VkPhysicalDeviceProperties pp;
   GetPhysicalDeviceProperties(g_pd, &pp);
   if (!(pp.limits.framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT)) {
      printf("SKIP msaa: 4x not advertised\n");
      return;
   }
   for (unsigned fi = 0; fi < 4; fi++) {
      const struct fmt *f = &fmts[fi];
      const VkSampleCountFlagBits S4 = VK_SAMPLE_COUNT_4_BIT;
      VkPipeline p_draw = make_pipe_s(m_vs, m_gen, f->f, false, S4, 0, VK_FORMAT_UNDEFINED);
      VkPipeline p_blend = make_pipe_s(m_vs, m_gen, f->f, true, S4, 0, VK_FORMAT_UNDEFINED);
      VkPipeline p_st = sfmt ? make_pipe_s(m_vs, m_gen, f->f, false, S4, 1, sfmt) : VK_NULL_HANDLE;
      VkPipeline p_cv = sfmt ? make_pipe_s(m_vs, m_gen, f->f, false, S4, 2, sfmt) : VK_NULL_HANDLE;
      for (unsigned s = 0; s < sizeof sz / sizeof sz[0]; s++)
         for (int mode = 0; mode < MS_NMODES; mode++) {
            if (mode == MS_STENCIL && !sfmt) continue;
            const uint32_t w = sz[s][0], h = sz[s][1];
            struct img ms = make_img_s(f->f, w, h, 1, S4, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            struct img one = make_img(f->f, w, h, 1, true), st = { 0 };
            const VkImageAspectFlags saspect = sfmt == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_STENCIL_BIT | VK_IMAGE_ASPECT_DEPTH_BIT;
            if (mode == MS_STENCIL)
               st = make_img_s(sfmt, w, h, 1, S4, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, saspect);
            uint8_t *m = model_new(f, w, h, 77);
            begin();
            to_layout(&one, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            upload(f, &one, 0, 0, 0, 0, 0, w, h, 77, NULL, w);   /* sentinel: what the resolve must overwrite (or keep) */
            to_layout(&one, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            to_layout(&ms, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            if (st.i) {
               VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                  .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = st.i,
                  .subresourceRange = { saspect, 0, 1, 0, 1 } };
               CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
            }
            const bool cmdres = mode == MS_PATTERN_CMD || mode == MS_EDGE_CMD;
            VkRect2D area = { { 0, 0 }, { w, h } };
            if (mode == MS_AREA) area = (VkRect2D){ { (int)(w / 4) + 1, (int)(h / 4) + 1 }, { (w + 1) / 2, (h + 1) / 2 } };
            VkRenderingAttachmentInfo att = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = ms.v,
               .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
               /* STORE + resolve: separate resolve pass after the frame; DONT_CARE + resolve: tile-buffer average writeback */
               .storeOp = mode == MS_DISCARD ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE };
            if (!cmdres) {
               att.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
               att.resolveImageView = one.v;
               att.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            VkRenderingAttachmentInfo satt = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = st.v,
               .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
               .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE };
            VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = area, .layerCount = 1,
                                   .colorAttachmentCount = 1, .pColorAttachments = &att, .pStencilAttachment = st.i ? &satt : NULL,
                                   .pDepthAttachment = st.i && sfmt != VK_FORMAT_S8_UINT ? &satt : NULL };
            VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
            CmdBeginRendering(cb, &ri);
            CmdSetViewport(cb, 0, 1, &vp);
            CmdSetScissor(cb, 0, 1, &area);
            /* outer / inner rectangles on half pixels: the "disc with a knocked-out letter" of a footer glyph */
            const float outer[4] = { (float)(w / 8) + .5f, (float)(h / 8) + .5f, (float)(w - w / 8) - .5f, (float)(h - h / 8) - .5f };
            const float inner[4] = { (float)(w / 3) + .5f, (float)(h / 3), (float)(w - w / 3), (float)(h - h / 3) - .5f };
            struct push pc = { .a = { 255, 1, 127, 0 }, .rect = { -1, -1, 1, 1 } };
            if (mode == MS_PATTERN || mode == MS_PATTERN_CMD || mode == MS_LOAD || mode == MS_AREA || mode == MS_DISCARD) {
               pc = (struct push){ .a = { (int)s, 0, 127, 0 }, .rect = { -1, -1, 1, 1 } };
               draw(p_draw, pc);
            } else if (mode == MS_EDGE || mode == MS_EDGE_CMD) {
               rect_push(&pc, outer, w, h);
               draw(p_draw, pc);
            } else {
               rect_push(&pc, outer, w, h);
               draw(p_st, pc);
               rect_push(&pc, inner, w, h);
               draw(p_st, pc);
               pc.rect[0] = pc.rect[1] = -1; pc.rect[2] = pc.rect[3] = 1;
               draw(p_cv, pc);
            }
            CmdEndRendering(cb);
            if (mode == MS_LOAD) {
               /* second pass: LOAD the 4x contents, add 64 on the right half, resolve again */
               barrier_all();
               att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
               CmdBeginRendering(cb, &ri);
               CmdSetViewport(cb, 0, 1, &vp);
               VkRect2D sc = { { (int)(w / 2), 0 }, { w - w / 2, h } };
               CmdSetScissor(cb, 0, 1, &sc);
               draw(p_blend, (struct push){ .a = { 64, 1, 0, 0 }, .rect = { -1, -1, 1, 1 } });
               CmdEndRendering(cb);
            }
            VkImageLayout one_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            if (cmdres) {
               to_layout(&ms, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
               to_layout(&one, one_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
               VkImageResolve rr = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { w, h, 1 } };
               CmdResolveImage(cb, ms.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, one.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rr);
               one_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            }
            to_layout(&one, one_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            struct img rt = sample_to_rb(one.v, w, h, 0, 0);
            submit();
            /* expected 1x contents, channel c of memory byte order */
            for (uint32_t y = 0; y < h; y++)
               for (uint32_t x = 0; x < w; x++) {
                  if (mode == MS_AREA && !((int)x >= area.offset.x && x < area.offset.x + area.extent.width && (int)y >= area.offset.y &&
                                           y < area.offset.y + area.extent.height))
                     continue;   /* outside the render area the resolve target keeps the sentinel */
                  unsigned cov = 0;
                  if (mode == MS_EDGE || mode == MS_EDGE_CMD) cov = __builtin_popcount(cover_mask(x, y, outer));
                  if (mode == MS_STENCIL) cov = __builtin_popcount(cover_mask(x, y, outer) ^ cover_mask(x, y, inner));
                  for (unsigned c = 0; c < f->nc; c++) {
                     unsigned byte = f->f == VK_FORMAT_B8G8R8A8_UNORM && c != 1 && c != 3 ? 2 - c : c;
                     unsigned v = (x * 7 + y * 13 + c * 61 + s * 29) & 127;
                     if (mode == MS_LOAD && x >= w / 2) v += 64;
                     if (mode == MS_EDGE || mode == MS_EDGE_CMD || mode == MS_STENCIL) v = (cov * 255 + 2) / 4;
                     m[((size_t)y * w + x) * f->bs + byte] = (uint8_t)v;
                  }
               }
            struct diff d = { 0 };
            for (uint32_t y = 0; y < h; y++)
               for (uint32_t x = 0; x < w; x++) {
                  uint8_t want[4];
                  expect_sample(f, m + ((size_t)y * w + x) * f->bs, false, want);
                  cmp_px(&d, x, y, rb.p + ((size_t)y * w + x) * 4, want, 4, 1);
               }
            char name[96];
            snprintf(name, sizeof name, "msaa fmt=%s w=%u h=%u %s", f->name, w, h, ms_names[mode]);
            report(name, d.bad, d.total, d.detail);
            free_img(&rt);
            free_img(&ms);
            free_img(&one);
            if (st.i) free_img(&st);
            free(m);
         }
   }
}

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   static char staged[4096];
   bool full = false;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct")) { if (i + 1 < argc && strncmp(argv[i + 1], "--", 2)) icd = argv[++i]; }
      else if (!strcmp(argv[i], "--only") && i + 1 < argc) g_only = argv[++i];
      else if (!strcmp(argv[i], "--full")) full = true;
      else if (!strcmp(argv[i], "--quiet")) quiet = true;
      else { fprintf(stderr, "usage: t_texupload [--direct [icd.so]] [--only group] [--full] [--quiet]\n"); return 2; }
   }
   if (!icd) {
      const char *m = getenv("VK_DRIVER_FILES");
      if (!m) m = getenv("VK_ICD_FILENAMES");
      const char *slash = m ? strrchr(m, '/') : NULL;
      if (!slash || strchr(m, ':')) { fprintf(stderr, "no --direct path and no single absolute ICD manifest in VK_DRIVER_FILES\n"); return 2; }
      snprintf(staged, sizeof staged, "%.*s/libvulkan_mali.so", (int)(slash - m), m);
      icd = staged;
   }
   const char *be = getenv("MALI_BACKEND");
   fake = be && !strcmp(be, "fake");
   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("FAIL dlopen %s\nSUMMARY pass=0 fail=1\n", dlerror()); return 2; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   VkResult (*negotiate)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   uint32_t iv = 5;
   if (!gipa || !negotiate || negotiate(&iv)) { printf("FAIL icd entry\nSUMMARY pass=0 fail=1\n"); return 2; }
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
   VkInstance inst;
   VKC(CreateInstance(&ici, NULL, &inst));
#define ILOAD(n) n = (PFN_vk##n)gipa(inst, "vk" #n);
   INST_FNS(ILOAD)
   VkPhysicalDevice pd;
   uint32_t cnt = 1;
   VkResult er = EnumeratePhysicalDevices(inst, &cnt, &pd);
   if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || !cnt) { printf("FAIL no device\nSUMMARY pass=0 fail=1\n"); return 2; }
   VkPhysicalDeviceProperties pp;
   GetPhysicalDeviceProperties(pd, &pp);
   GetPhysicalDeviceMemoryProperties(pd, &memprops);
   printf("device %s driver=%s\n", pp.deviceName, icd);
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
   VkPhysicalDeviceDynamicRenderingFeatures dr = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, .dynamicRendering = VK_TRUE };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &dr, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi };
   VKC(CreateDevice(pd, &dci, NULL, &dev));
   g_pd = pd;
#define DLOAD(n) n = (PFN_vk##n)GetDeviceProcAddr(dev, "vk" #n); if (!n) { printf("FAIL missing vk" #n "\nSUMMARY pass=0 fail=1\n"); return 2; }
   DEV_FNS(DLOAD)
   GetDeviceQueue(dev, 0, 0, &queue);

   VkDescriptorSetLayoutBinding bind = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
   VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &bind };
   VKC(CreateDescriptorSetLayout(dev, &lci, NULL, &dsl));
   VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct push) };
   VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl,
                                       .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
   VKC(CreatePipelineLayout(dev, &plci, NULL, &layout));
   VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NSETS };
   VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = NSETS, .poolSizeCount = 1, .pPoolSizes = &ps };
   VkDescriptorPool dpool;
   VKC(CreateDescriptorPool(dev, &dpci, NULL, &dpool));
   for (unsigned i = 0; i < NSETS; i++) {
      VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
      VKC(AllocateDescriptorSets(dev, &ai, &sets[i]));
   }
   VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
   VKC(CreateSampler(dev, &sci, NULL, &sampler));
   VkShaderModule vs = module(texupload_vert_spv, sizeof texupload_vert_spv);
   VkShaderModule fs_sample = module(texupload_sample_spv, sizeof texupload_sample_spv);
   VkShaderModule fs_gen = module(texupload_gen_spv, sizeof texupload_gen_spv);
   VkShaderModule fs_deriv = module(texupload_deriv_spv, sizeof texupload_deriv_spv);
   m_vs = vs;
   m_gen = fs_gen;
   pipe_sample = make_pipe(vs, fs_sample, VK_FORMAT_R8G8B8A8_UNORM, false);
   pipe_deriv = make_pipe(vs, fs_deriv, VK_FORMAT_R8G8B8A8_UNORM, false);
   for (unsigned i = 0; i < 4; i++) {
      pipe_gen[i] = make_pipe(vs, fs_gen, fmts[i].f, false);
      pipe_gen_blend[i] = make_pipe(vs, fs_gen, fmts[i].f, true);
   }
   VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
   VKC(CreateCommandPool(dev, &cpci, NULL, &pool));
   VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VKC(AllocateCommandBuffers(dev, &cbai, &cb));
   VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VKC(CreateFence(dev, &fci, NULL, &fence));
   make_buf(&up);
   make_buf(&rb);

#define GROUP(n) (!g_only || !strcmp(g_only, n))
   if (GROUP("deriv")) t_deriv();
   if (GROUP("atlas")) t_atlas();
   if (GROUP("reupload")) t_reupload();
   if (GROUP("recycle")) t_recycle();
   if (GROUP("rt")) t_rt();
   if (GROUP("mip")) t_mip();
   if (GROUP("i2i") || GROUP("i2b")) t_i2i_i2b();
   t_upload(full);
   if (GROUP("msaa")) t_msaa();   /* last: a refusal or fault here must not hide the 1x results */
   DeviceWaitIdle(dev);
   printf("SUMMARY pass=%u fail=%u%s\n", n_pass, n_fail, fake ? " (fake backend: pixels NOT verified)" : "");
   return n_fail || !n_pass ? 1 : 0;
}
