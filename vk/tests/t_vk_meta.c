/* SPDX-License-Identifier: MIT */
/* M12/M13 test: meta copies/blits/clears/resolve, events, timestamps through the ICD. Loader-free: dlopens the
 * ICD like t_queue --direct. One JSON line per case; exit 0 = all pass.
 *
 *   t_vk_compute --direct <icd.so> [--loop N] [--only buf|fill|img|copyimg|blit|clearc|cleard|resolve|event|ts]
 *             [--spv-dir dir]
 * Pixel checks are skipped on the fake backend (MALI_BACKEND=fake): it renders nothing.
 * --loop N repeats the triangle case N times; --loop-reset reuses one command buffer via
 * vkResetCommandPool instead of allocating a fresh one each time.
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define W 64
#define H 64

static int g_fail, g_fake, g_loop = 1;
static const char *g_only;


#define CASE_ON(n) (!g_only || !strcmp(g_only, n))
#define CHECK(c, ...)                                                                              \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("{\"fail\":\"%s:%d\",\"expr\":\"%s\",\"detail\":\"", __FILE__, __LINE__, #c);     \
         printf(__VA_ARGS__);                                                                      \
         printf("\"}\n");                                                                          \
         g_fail = 1;                                                                               \
         return 0;                                                                                 \
      }                                                                                            \
   } while (0)

#define DEV_FNS(X)                                                                                 \
   X(DestroyDevice) X(GetDeviceQueue) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory)  \
   X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(QueueSubmit) X(DeviceWaitIdle)  \
   X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory)            \
   X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory)                 \
   X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(FreeCommandBuffers)      \
   X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier)                                 \
   X(CmdCopyBuffer) X(CmdFillBuffer) X(CmdUpdateBuffer) X(CmdCopyBufferToImage)                    \
   X(CmdCopyImageToBuffer) X(CmdCopyImage) X(CmdBlitImage) X(CmdClearColorImage)                   \
   X(CmdClearDepthStencilImage) X(CmdResolveImage)                                                 \
   X(CreateEvent) X(DestroyEvent) X(GetEventStatus) X(SetEvent) X(ResetEvent)                      \
   X(CmdSetEvent) X(CmdResetEvent) X(CmdWaitEvents)                                                \
   X(CreateQueryPool) X(DestroyQueryPool) X(GetQueryPoolResults) X(CmdResetQueryPool)              \
   X(CmdWriteTimestamp) X(CmdCopyQueryPoolResults)

#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memprops;

__attribute__((unused)) static double
now_us(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static int
memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
         return (int)i;
   return -1;
}

struct buf { VkBuffer b; VkDeviceMemory m; void *p; size_t size; };

static int
mkbuf(struct buf *o, size_t size, VkBufferUsageFlags usage)
{
   o->size = size;
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   CHECK(CreateBuffer(dev, &bi, NULL, &o->b) == VK_SUCCESS, "CreateBuffer");
   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(dev, o->b, &mr);
   int mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   CHECK(mt >= 0, "no host visible type");
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
   CHECK(AllocateMemory(dev, &ai, NULL, &o->m) == VK_SUCCESS, "AllocateMemory");
   CHECK(BindBufferMemory(dev, o->b, o->m, 0) == VK_SUCCESS, "BindBufferMemory");
   CHECK(MapMemory(dev, o->m, 0, VK_WHOLE_SIZE, 0, &o->p) == VK_SUCCESS, "MapMemory");
   return 1;
}

static void
rmbuf(struct buf *o)
{
   if (o->b) DestroyBuffer(dev, o->b, NULL);
   if (o->m) FreeMemory(dev, o->m, NULL);
}

/* ---- cases ---- */
static VkCommandPool pool;
static VkFence fence;

static VkCommandBuffer
begin_cb(void)
{
   VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                     .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VkCommandBuffer cb;
   if (AllocateCommandBuffers(dev, &ai, &cb) != VK_SUCCESS) return NULL;
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   BeginCommandBuffer(cb, &bi);
   return cb;
}

static int
submit_wait(VkCommandBuffer cb)
{
   if (EndCommandBuffer(cb) != VK_SUCCESS) return 0;
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   ResetFences(dev, 1, &fence);
   if (QueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) return 0;
   if (WaitForFences(dev, 1, &fence, VK_TRUE, 20ull * 1000000000ull) != VK_SUCCESS) return 0;
   FreeCommandBuffers(dev, pool, 1, &cb);
   return 1;
}

static void
barrier_all(VkCommandBuffer cb)
{
   VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                         .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void
layout_tr(VkCommandBuffer cb, VkImage im, VkImageLayout from, VkImageLayout to, VkImageAspectFlags asp)
{
   VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                             .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = from,
                             .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                             .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im,
                             .subresourceRange = {asp, 0, 1, 0, 1}};
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

struct img { VkImage i; VkDeviceMemory m; };

static int
mkimg(struct img *o, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkSampleCountFlagBits s)
{
   VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                           .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = s,
                           .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   CHECK(CreateImage(dev, &ci, NULL, &o->i) == VK_SUCCESS, "CreateImage fmt %d", fmt);
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, o->i, &mr);
   int mt = memtype(mr.memoryTypeBits, 0);
   CHECK(mt >= 0, "no img mem type");
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
   CHECK(AllocateMemory(dev, &ai, NULL, &o->m) == VK_SUCCESS, "img alloc");
   CHECK(BindImageMemory(dev, o->i, o->m, 0) == VK_SUCCESS, "img bind");
   return 1;
}

static void
rmimg(struct img *o)
{
   if (o->i) DestroyImage(dev, o->i, NULL);
   if (o->m) FreeMemory(dev, o->m, NULL);
}

#define TRY(x) do { if (!(x)) return 0; } while (0)

static void
report(const char *name, const char *skip)
{
   printf("{\"test\":\"%s\",\"n\":%d,\"ok\":true%s%s%s}\n", name, g_loop, skip ? ",\"skipped\":\"" : "", skip ? skip : "", skip ? "\"" : "");
}

static int
case_buf(void)
{
   struct buf a = {0}, b = {0};
   TRY(mkbuf(&a, 4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT));
   TRY(mkbuf(&b, 4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT));
   for (int l = 0; l < g_loop; l++) {
      uint8_t *pa = a.p, *pb = b.p;
      for (int i = 0; i < 4096; i++) { pa[i] = (uint8_t)(i * 7 + l); pb[i] = 0xEE; }
      VkCommandBuffer cb = begin_cb();
      CHECK(cb, "cb");
      /* aligned, unaligned-offset, odd-size and overlapping-free regions */
      VkBufferCopy r[3] = {{0, 0, 1024}, {1027, 2051, 333}, {2000, 3000, 1}};
      CmdCopyBuffer(cb, a.b, b.b, 3, r);
      CHECK(submit_wait(cb), "copy buffer submit");
      if (!g_fake) {
         for (int i = 0; i < 1024; i++) CHECK(pb[i] == (uint8_t)(i * 7 + l), "r0 byte %d = %d", i, pb[i]);
         for (int i = 0; i < 333; i++) CHECK(pb[2051 + i] == (uint8_t)((1027 + i) * 7 + l), "r1 byte %d = %d", i, pb[2051 + i]);
         CHECK(pb[3000] == (uint8_t)(2000 * 7 + l), "r2");
         CHECK(pb[1024] == 0xEE && pb[2050] == 0xEE && pb[2384] == 0xEE && pb[3001] == 0xEE, "overrun");
      }
   }
   rmbuf(&a); rmbuf(&b);
   report("buf", g_fake ? "results: fake backend" : NULL);
   return 1;
}

static int
case_fill(void)
{
   struct buf a = {0};
   TRY(mkbuf(&a, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT));
   for (int l = 0; l < g_loop; l++) {
      uint32_t *p = a.p;
      memset(a.p, 0x11, 4096);
      VkCommandBuffer cb = begin_cb();
      CHECK(cb, "cb");
      CmdFillBuffer(cb, a.b, 256, 1024, 0xDEADBEEFu + l);
      barrier_all(cb);
      uint32_t upd[8];
      for (int i = 0; i < 8; i++) upd[i] = 0x1000u + i + l;
      CmdUpdateBuffer(cb, a.b, 2048, sizeof upd, upd);
      CHECK(submit_wait(cb), "submit");
      if (!g_fake) {
         for (int i = 0; i < 256; i++) CHECK(p[64 + i] == 0xDEADBEEFu + l, "fill word %d = %08x", i, p[64 + i]);
         for (int i = 0; i < 8; i++) CHECK(p[512 + i] == 0x1000u + i + l, "update word %d = %08x", i, p[512 + i]);
         CHECK(p[63] == 0x11111111u && p[320] == 0x11111111u && p[520] == 0x11111111u, "overrun");
      }
   }
   rmbuf(&a);
   report("fill", g_fake ? "results: fake backend" : NULL);
   return 1;
}

/* buffer -> image -> image -> buffer */
static int
case_img(int copyimg)
{
   const uint32_t w = 37, h = 21; /* deliberately awkward */
   struct buf src = {0}, dst = {0};
   struct img i1 = {0}, i2 = {0};
   size_t sz = w * h * 4;
   TRY(mkbuf(&src, sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
   TRY(mkbuf(&dst, sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT));
   VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   TRY(mkimg(&i1, VK_FORMAT_R8G8B8A8_UNORM, w, h, u, VK_SAMPLE_COUNT_1_BIT));
   if (copyimg) TRY(mkimg(&i2, VK_FORMAT_R8G8B8A8_UNORM, w, h, u, VK_SAMPLE_COUNT_1_BIT));
   VkImageSubresourceLayers sl = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
   for (int l = 0; l < g_loop; l++) {
      uint32_t *ps = src.p, *pd = dst.p;
      for (uint32_t i = 0; i < w * h; i++) { ps[i] = 0x01020304u * (i + 1) + l; pd[i] = 0xEEEEEEEEu; }
      VkCommandBuffer cb = begin_cb();
      CHECK(cb, "cb");
      layout_tr(cb, i1.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
      VkBufferImageCopy up = {.imageSubresource = sl, .imageExtent = {w, h, 1}};
      CmdCopyBufferToImage(cb, src.b, i1.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
      VkImage last = i1.i;
      if (copyimg) {
         layout_tr(cb, i1.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
         layout_tr(cb, i2.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
         VkImageCopy ic = {.srcSubresource = sl, .dstSubresource = sl, .extent = {w, h, 1}};
         CmdCopyImage(cb, i1.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, i2.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
         layout_tr(cb, i2.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
         last = i2.i;
      } else {
         layout_tr(cb, i1.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
      }
      VkBufferImageCopy dn = {.imageSubresource = sl, .imageExtent = {w, h, 1}};
      CmdCopyImageToBuffer(cb, last, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.b, 1, &dn);
      CHECK(submit_wait(cb), "submit");
      if (!g_fake)
         for (uint32_t i = 0; i < w * h; i++) CHECK(pd[i] == ps[i], "texel %u: %08x != %08x", i, pd[i], ps[i]);
   }
   rmimg(&i1); rmimg(&i2); rmbuf(&src); rmbuf(&dst);
   report(copyimg ? "copyimg" : "img", g_fake ? "results: fake backend" : NULL);
   return 1;
}

/* 1:1 blit and 2x minify blit of a gradient, checked against expected values within tolerance */
static int
case_blit(void)
{
   const uint32_t w = 32, h = 32;
   struct buf src = {0}, dst = {0};
   struct img i1 = {0}, i2 = {0};
   TRY(mkbuf(&src, w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
   TRY(mkbuf(&dst, w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT));
   VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   TRY(mkimg(&i1, VK_FORMAT_R8G8B8A8_UNORM, w, h, u, VK_SAMPLE_COUNT_1_BIT));
   TRY(mkimg(&i2, VK_FORMAT_R8G8B8A8_UNORM, w, h, u, VK_SAMPLE_COUNT_1_BIT));
   VkImageSubresourceLayers sl = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
   for (int l = 0; l < g_loop; l++) {
      uint8_t *ps = src.p, *pd = dst.p;
      for (uint32_t y = 0; y < h; y++)
         for (uint32_t x = 0; x < w; x++) {
            uint8_t *t = ps + (y * w + x) * 4;
            t[0] = x * 8; t[1] = y * 8; t[2] = 100; t[3] = 255;
         }
      memset(pd, 0xEE, w * h * 4);
      VkCommandBuffer cb = begin_cb();
      CHECK(cb, "cb");
      layout_tr(cb, i1.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
      layout_tr(cb, i2.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
      VkBufferImageCopy up = {.imageSubresource = sl, .imageExtent = {w, h, 1}};
      CmdCopyBufferToImage(cb, src.b, i1.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
      layout_tr(cb, i1.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
      /* minify whole 32x32 into the top-left 16x16, nearest */
      VkImageBlit bl = {.srcSubresource = sl, .srcOffsets = {{0, 0, 0}, {w, h, 1}},
                        .dstSubresource = sl, .dstOffsets = {{0, 0, 0}, {16, 16, 1}}};
      CmdBlitImage(cb, i1.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, i2.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
      layout_tr(cb, i2.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
      VkBufferImageCopy dn = {.imageSubresource = sl, .imageExtent = {w, h, 1}};
      CmdCopyImageToBuffer(cb, i2.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.b, 1, &dn);
      CHECK(submit_wait(cb), "submit");
      if (!g_fake)
         for (uint32_t y = 0; y < 16; y++)
            for (uint32_t x = 0; x < 16; x++) {
               uint8_t *t = pd + (y * w + x) * 4;
               int ex = x * 16 + 8, ey = y * 16 + 8; /* centre sample of the 2x2 footprint */
               CHECK(abs(t[0] - ex) <= 9 && abs(t[1] - ey) <= 9 && t[2] == 100 && t[3] == 255,
                     "blit (%u,%u) = %d,%d,%d,%d want ~%d,%d", x, y, t[0], t[1], t[2], t[3], ex, ey);
            }
   }
   rmimg(&i1); rmimg(&i2); rmbuf(&src); rmbuf(&dst);
   report("blit", g_fake ? "results: fake backend" : NULL);
   return 1;
}

static int
case_clearc(void)
{
   const uint32_t w = 40, h = 24;
   struct buf dst = {0};
   struct img im = {0};
   TRY(mkbuf(&dst, w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT));
   TRY(mkimg(&im, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT));
   VkImageSubresourceLayers sl = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
   for (int l = 0; l < g_loop; l++) {
      uint32_t *pd = dst.p;
      memset(pd, 0xEE, w * h * 4);
      VkCommandBuffer cb = begin_cb();
      CHECK(cb, "cb");
      layout_tr(cb, im.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
      VkClearColorValue c = {.float32 = {1.0f, 0.5f, 0.0f, 1.0f}};
      VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      CmdClearColorImage(cb, im.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rg);
      layout_tr(cb, im.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
      VkBufferImageCopy dn = {.imageSubresource = sl, .imageExtent = {w, h, 1}};
      CmdCopyImageToBuffer(cb, im.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.b, 1, &dn);
      CHECK(submit_wait(cb), "submit");
      if (!g_fake)
         for (uint32_t i = 0; i < w * h; i++) {
            uint8_t *t = (uint8_t *)&pd[i];
            CHECK(t[0] == 255 && abs(t[1] - 128) <= 1 && t[2] == 0 && t[3] == 255, "texel %u = %d,%d,%d,%d", i, t[0], t[1], t[2], t[3]);
         }
   }
   rmimg(&im); rmbuf(&dst);
   report("clearc", g_fake ? "results: fake backend" : NULL);
   return 1;
}

static int
case_cleard(void)
{
   const uint32_t w = 32, h = 16;
   struct buf dst = {0};
   struct img im = {0};
   TRY(mkbuf(&dst, w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT));
   TRY(mkimg(&im, VK_FORMAT_D32_SFLOAT, w, h, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT));
   VkImageSubresourceLayers sl = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
   for (int l = 0; l < g_loop; l++) {
      float *pd = dst.p;
      memset(pd, 0xEE, w * h * 4);
      VkCommandBuffer cb = begin_cb();
      CHECK(cb, "cb");
      layout_tr(cb, im.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
      VkClearDepthStencilValue c = {.depth = 0.25f};
      VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
      CmdClearDepthStencilImage(cb, im.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rg);
      layout_tr(cb, im.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
      VkBufferImageCopy dn = {.imageSubresource = sl, .imageExtent = {w, h, 1}};
      CmdCopyImageToBuffer(cb, im.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.b, 1, &dn);
      CHECK(submit_wait(cb), "submit");
      if (!g_fake)
         for (uint32_t i = 0; i < w * h; i++) CHECK(fabsf(pd[i] - 0.25f) < 1e-6f, "depth %u = %f", i, pd[i]);
   }
   rmimg(&im); rmbuf(&dst);
   report("cleard", g_fake ? "results: fake backend" : NULL);
   return 1;
}

/* 4x MSAA clear then resolve */
static int
case_resolve(void)
{
   const uint32_t w = 32, h = 32;
   struct buf dst = {0};
   struct img ms = {0}, rs = {0};
   TRY(mkbuf(&dst, w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT));
   TRY(mkimg(&ms, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_4_BIT));
   TRY(mkimg(&rs, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT));
   VkImageSubresourceLayers sl = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
   for (int l = 0; l < g_loop; l++) {
      uint32_t *pd = dst.p;
      memset(pd, 0xEE, w * h * 4);
      VkCommandBuffer cb = begin_cb();
      CHECK(cb, "cb");
      layout_tr(cb, ms.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
      layout_tr(cb, rs.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
      VkClearColorValue c = {.float32 = {0.0f, 1.0f, 0.0f, 1.0f}};
      VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      CmdClearColorImage(cb, ms.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rg);
      layout_tr(cb, ms.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
      VkImageResolve rr = {.srcSubresource = sl, .dstSubresource = sl, .extent = {w, h, 1}};
      CmdResolveImage(cb, ms.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rs.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rr);
      layout_tr(cb, rs.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
      VkBufferImageCopy dn = {.imageSubresource = sl, .imageExtent = {w, h, 1}};
      CmdCopyImageToBuffer(cb, rs.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.b, 1, &dn);
      CHECK(submit_wait(cb), "submit");
      if (!g_fake)
         for (uint32_t i = 0; i < w * h; i++) {
            uint8_t *t = (uint8_t *)&pd[i];
            CHECK(t[0] == 0 && t[1] == 255 && t[2] == 0 && t[3] == 255, "texel %u = %d,%d,%d,%d", i, t[0], t[1], t[2], t[3]);
         }
   }
   rmimg(&ms); rmimg(&rs); rmbuf(&dst);
   report("resolve", g_fake ? "results: fake backend" : NULL);
   return 1;
}

/* host SetEvent/ResetEvent/GetEventStatus + device set -> wait -> copy ordering + device reset */
static int
case_event(void)
{
   struct buf a = {0}, b = {0};
   TRY(mkbuf(&a, 256, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT));
   TRY(mkbuf(&b, 256, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT));
   VkEventCreateInfo ei = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
   VkEvent ev, ev2;
   CHECK(CreateEvent(dev, &ei, NULL, &ev) == VK_SUCCESS, "CreateEvent");
   CHECK(CreateEvent(dev, &ei, NULL, &ev2) == VK_SUCCESS, "CreateEvent2");
   CHECK(GetEventStatus(dev, ev) == VK_EVENT_RESET, "initial status");
   CHECK(SetEvent(dev, ev) == VK_SUCCESS, "SetEvent");
   CHECK(GetEventStatus(dev, ev) == VK_EVENT_SET, "status after SetEvent");
   CHECK(ResetEvent(dev, ev) == VK_SUCCESS, "ResetEvent");
   CHECK(GetEventStatus(dev, ev) == VK_EVENT_RESET, "status after ResetEvent");
   for (int l = 0; l < g_loop; l++) {
      memset(a.p, 0x5A, 256);
      memset(b.p, 0, 256);
      VkCommandBuffer cb = begin_cb();
      CHECK(cb, "cb");
      VkBufferCopy r = {0, 0, 64};
      CmdCopyBuffer(cb, a.b, b.b, 1, &r);
      CmdSetEvent(cb, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
      VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT};
      CmdWaitEvents(cb, 1, &ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 1, &mb, 0, NULL, 0, NULL);
      VkBufferCopy r2 = {0, 128, 64};
      CmdCopyBuffer(cb, b.b, b.b, 1, &r2);
      CmdResetEvent(cb, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
      CmdSetEvent(cb, ev2, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
      CHECK(submit_wait(cb), "submit");
      if (!g_fake) {
      CHECK(GetEventStatus(dev, ev) == VK_EVENT_RESET, "device reset");
      CHECK(GetEventStatus(dev, ev2) == VK_EVENT_SET, "device set");
      }
      CHECK(ResetEvent(dev, ev2) == VK_SUCCESS, "reset ev2");
      /* host-set event consumed by a device wait */
      CHECK(SetEvent(dev, ev) == VK_SUCCESS, "host set");
      cb = begin_cb();
      CmdWaitEvents(cb, 1, &ev, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, NULL, 0, NULL, 0, NULL);
      CmdResetEvent(cb, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
      CHECK(submit_wait(cb), "submit2");
      if (!g_fake) CHECK(GetEventStatus(dev, ev) == VK_EVENT_RESET, "device reset after host wait");
      if (!g_fake) {
         uint8_t *pb = b.p;
         for (int i = 0; i < 64; i++) CHECK(pb[i] == 0x5A && pb[128 + i] == 0x5A, "copy %d: %02x %02x", i, pb[i], pb[128 + i]);
      }
   }
   DestroyEvent(dev, ev, NULL);
   DestroyEvent(dev, ev2, NULL);
   rmbuf(&a); rmbuf(&b);
   report("event", g_fake ? "results: fake backend" : NULL);
   return 1;
}

/* timestamps: two writes around a fill; results monotonic, WAIT + 64-bit, availability, copy results */
static int
case_ts(void)
{
   struct buf a = {0}, out = {0};
   TRY(mkbuf(&a, 1 << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT));
   TRY(mkbuf(&out, 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT));
   VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 4};
   VkQueryPool qp;
   CHECK(CreateQueryPool(dev, &qi, NULL, &qp) == VK_SUCCESS, "CreateQueryPool");
   for (int l = 0; l < g_loop; l++) {
      VkCommandBuffer cb = begin_cb();
      CHECK(cb, "cb");
      CmdResetQueryPool(cb, qp, 0, 4);
      CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
      CmdFillBuffer(cb, a.b, 0, 1 << 20, 0x77u);
      barrier_all(cb);
      CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
      CmdCopyQueryPoolResults(cb, qp, 0, 2, out.b, 0, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      CHECK(submit_wait(cb), "submit");
      uint64_t v[4] = {0};
      if (!g_fake) CHECK(GetQueryPoolResults(dev, qp, 0, 2, sizeof v, v, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS, "GetQueryPoolResults");
      uint64_t ct[2];
      memcpy(ct, out.p, 16);
      if (!g_fake) {
         CHECK(v[0] != 0 && v[1] != 0, "zero timestamp %llu %llu", (unsigned long long)v[0], (unsigned long long)v[1]);
         CHECK(v[1] >= v[0], "non-monotonic %llu then %llu", (unsigned long long)v[0], (unsigned long long)v[1]);
         CHECK(ct[0] == v[0] && ct[1] == v[1], "copy results differ: host %llu %llu copy %llu %llu", (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)ct[0], (unsigned long long)ct[1]);
         if (l == 0) printf("{\"ts_delta\":%llu}\n", (unsigned long long)(v[1] - v[0]));
      }
      /* unwritten query must report not ready without WAIT */
      uint32_t x[2];
      VkResult r = GetQueryPoolResults(dev, qp, 2, 1, 8, x, 8, VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
      CHECK(r == VK_NOT_READY, "unwritten query: %d", r);
   }
   DestroyQueryPool(dev, qp, NULL);
   rmbuf(&a); rmbuf(&out);
   report("ts", g_fake ? "results: fake backend" : NULL);
   return 1;
}

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--loop") && i + 1 < argc) g_loop = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--only") && i + 1 < argc) g_only = argv[++i];
      else { fprintf(stderr, "usage: t_vk_meta --direct icd.so [--loop N] [--only case]\n"); return 2; }
   }
   if (!icd) { fprintf(stderr, "--direct required\n"); return 2; }
   const char *be = getenv("MALI_BACKEND");
   g_fake = be && !strcmp(be, "fake");

   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("{\"fail\":\"dlopen: %s\"}\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   VkResult (*negotiate)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   uint32_t iv = 5;
   if (!gipa || !negotiate || negotiate(&iv)) { printf("{\"fail\":\"icd entry\"}\n"); return 1; }

   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   const char *iext[] = {"VK_KHR_get_physical_device_properties2", "VK_KHR_external_memory_capabilities",
                         "VK_KHR_external_semaphore_capabilities", "VK_KHR_external_fence_capabilities"};
   ici.enabledExtensionCount = 4;
   ici.ppEnabledExtensionNames = iext;
   VkInstance inst;
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("{\"fail\":\"CreateInstance\"}\n"); return 1; }
   PFN_vkEnumeratePhysicalDevices EPD = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceMemoryProperties GMP = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
   PFN_vkGetPhysicalDeviceProperties GPP = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
   PFN_vkCreateDevice CreateDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr GDPA = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   PFN_vkDestroyInstance DestroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
   VkPhysicalDevice pd;
   uint32_t cnt = 1;
   VkResult r = EPD(inst, &cnt, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || cnt < 1) { printf("{\"fail\":\"no device\"}\n"); return 1; }
   VkPhysicalDeviceProperties pp;
   GPP(pd, &pp);
   GMP(pd, &memprops);
   printf("{\"device\":\"%s\"}\n", pp.deviceName);

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
   if (CreateDevice(pd, &di, NULL, &dev) != VK_SUCCESS) { printf("{\"fail\":\"CreateDevice\"}\n"); return 1; }
#define LOAD(n) n = (PFN_vk##n)GDPA(dev, "vk" #n); if (!n) { printf("{\"fail\":\"missing vk" #n "\"}\n"); return 1; }
   DEV_FNS(LOAD)
   GetDeviceQueue(dev, 0, 0, &queue);
   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if (CreateCommandPool(dev, &pci, NULL, &pool) != VK_SUCCESS || CreateFence(dev, &fci, NULL, &fence) != VK_SUCCESS) {
      printf("{\"fail\":\"pool/fence\"}\n");
      return 1;
   }

   int ok = 1;
   if (CASE_ON("buf")) ok &= case_buf();
   if (ok && CASE_ON("fill")) ok &= case_fill();
   if (ok && CASE_ON("img")) ok &= case_img(0);
   if (ok && CASE_ON("copyimg")) ok &= case_img(1);
   if (ok && CASE_ON("blit")) ok &= case_blit();
   if (ok && CASE_ON("clearc")) ok &= case_clearc();
   if (ok && CASE_ON("cleard")) ok &= case_cleard();
   if (ok && CASE_ON("resolve")) ok &= case_resolve();
   if (ok && CASE_ON("event")) ok &= case_event();
   if (ok && CASE_ON("ts")) ok &= case_ts();

   DeviceWaitIdle(dev);
   DestroyFence(dev, fence, NULL);
   DestroyCommandPool(dev, pool, NULL);
   DestroyDevice(dev, NULL);
   DestroyInstance(inst, NULL);
   dlclose(lib);
   printf("{\"pass\":%s}\n", (ok && !g_fail) ? "true" : "false");
   return (ok && !g_fail) ? 0 : 1;
}
