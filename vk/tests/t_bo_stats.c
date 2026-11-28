/* SPDX-License-Identifier: MIT */
/* BO allocation statistics and the suballocator, end to end on the fake backend (white-box: linked statically with the driver
 * objects like t_resources, reads mali_bo_stats_get).
 *   1. a known sequence (device memory, descriptor pool, image view, query pool + event, one buffer->image copy) moves exactly
 *      the expected per-site counters: one BO per object with MALI_BO_SUBALLOC=0, none outside the chunk site otherwise;
 *   2. steady state: 2,000 small buffer->image copies (recorded, submitted, waited, command buffer reset) and 2,000 image-view
 *      create/destroy cycles, in frames of 10 + 10. With the suballocator no BO is allocated or freed after the warm-up frames.
 * Run: MALI_BACKEND=fake MALI_FAKE_GPU=v11|v13 [MALI_BO_SUBALLOC=0] t_bo_stats. Exit 0 = pass; the last line is JSON. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mali_private.h"
#include "mali_suballoc.h"
#include "vk_device.h"

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char *);

#define CHECK(c, ...)                                                                                                    \
   do {                                                                                                                  \
      if (!(c)) {                                                                                                        \
         printf("{\"fail\":\"%s:%d\",\"expr\":\"%s\",\"detail\":\"", __FILE__, __LINE__, #c);                           \
         printf("" __VA_ARGS__);                                                                                         \
         printf("\"}\n");                                                                                                \
         return 1;                                                                                                       \
      }                                                                                                                  \
   } while (0)

static VkInstance inst;
static VkDevice dev;
static VkQueue queue;
static PFN_vkVoidFunction (*gdpa)(VkDevice, const char *);
#define FNS(X)                                                                                                          \
   X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(CreateImage) X(DestroyImage)                       \
   X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(BindImageMemory) X(MapMemory)     \
   X(CreateImageView) X(DestroyImageView) X(CreateDescriptorPool) X(DestroyDescriptorPool) X(CreateQueryPool)            \
   X(DestroyQueryPool) X(CreateEvent) X(DestroyEvent) X(CreateCommandPool) X(DestroyCommandPool)                        \
   X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) X(CmdCopyBufferToImage)    \
   X(CmdPipelineBarrier) X(QueueSubmit) X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences)                  \
   X(GetDeviceQueue) X(DeviceWaitIdle) X(DestroyDevice)
#define DECL(n) static PFN_vk##n p_##n;
FNS(DECL)
#define LOAD(n) do { p_##n = (PFN_vk##n)gdpa(dev, "vk" #n); CHECK(p_##n, "missing vk" #n); } while (0)

static struct mali_bo_stats s0;

static void snap(void) { mali_bo_stats_get(&s0); }
/* BO allocations / frees of one site since snap() */
static long long da(enum mali_bo_site s) { struct mali_bo_stats c; mali_bo_stats_get(&c); return (long long)(c.allocs[s] - s0.allocs[s]); }
static long long df(enum mali_bo_site s) { struct mali_bo_stats c; mali_bo_stats_get(&c); return (long long)(c.frees[s] - s0.frees[s]); }
static long long
dall(bool frees)
{
   struct mali_bo_stats c;
   mali_bo_stats_get(&c);
   long long n = 0;
   for (unsigned s = 0; s < MALI_SITE_N; s++)
      n += frees ? (long long)(c.frees[s] - s0.frees[s]) : (long long)(c.allocs[s] - s0.allocs[s]);
   return n;
}

static VkImage img;
static VkBuffer buf;
static VkCommandPool cpool;
static VkCommandBuffer cb;
static VkFence fence;

static int
view_cycle(void)
{
   VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkImageView v;
   CHECK(p_CreateImageView(dev, &vci, NULL, &v) == VK_SUCCESS, "view");
   p_DestroyImageView(dev, v, NULL);
   return 0;
}

/* One frame: `n` 16x16 buffer->image copies in one command buffer, submitted and waited, then the command buffer reset
 * (which releases its meta memory). */
static int
copy_frame(unsigned n)
{
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CHECK(p_BeginCommandBuffer(cb, &bi) == VK_SUCCESS, "begin");
   VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                             .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                             .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                             .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   p_CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   for (unsigned i = 0; i < n; i++) {
      VkBufferImageCopy r = {.bufferOffset = (i % 4) * 1024, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                             .imageOffset = {(int)(i % 4) * 16, (int)(i / 4 % 4) * 16, 0}, .imageExtent = {16, 16, 1}};
      p_CmdCopyBufferToImage(cb, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
   }
   CHECK(p_EndCommandBuffer(cb) == VK_SUCCESS, "end (a copy failed to record)");
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   CHECK(p_ResetFences(dev, 1, &fence) == VK_SUCCESS, "reset fence");
   CHECK(p_QueueSubmit(queue, 1, &si, fence) == VK_SUCCESS, "submit");
   CHECK(p_WaitForFences(dev, 1, &fence, VK_TRUE, 20ull * 1000000000ull) == VK_SUCCESS, "wait");
   CHECK(p_ResetCommandBuffer(cb, 0) == VK_SUCCESS, "reset cb");
   return 0;
}

static int
run(void)
{
   const char *e = getenv("MALI_BO_SUBALLOC");
   const bool on = !(e && e[0] == '0');
   PFN_vkCreateInstance ci = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
   CHECK(ci(&ici, NULL, &inst) == VK_SUCCESS, "instance");
   PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)vk_icdGetInstanceProcAddr(inst, "vkEnumeratePhysicalDevices");
   uint32_t n = 1;
   VkPhysicalDevice pd;
   CHECK(en(inst, &n, &pd) == VK_SUCCESS && n == 1, "no device (MALI_BACKEND=fake?)");
   PFN_vkCreateDevice cd = (PFN_vkCreateDevice)vk_icdGetInstanceProcAddr(inst, "vkCreateDevice");
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
   CHECK(cd(pd, &dci, NULL, &dev) == VK_SUCCESS, "device");
   gdpa = (PFN_vkVoidFunction(*)(VkDevice, const char *))vk_icdGetInstanceProcAddr(inst, "vkGetDeviceProcAddr");
#define L(n) LOAD(n);
   FNS(L)
   p_GetDeviceQueue(dev, 0, 0, &queue);

   /* ---- 1. known sequence ---- */
   snap();
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
   CHECK(p_CreateBuffer(dev, &bci, NULL, &buf) == VK_SUCCESS, "buffer");
   VkMemoryRequirements br, ir;
   p_GetBufferMemoryRequirements(dev, buf, &br);
   VkImageCreateInfo imci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                             .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   CHECK(p_CreateImage(dev, &imci, NULL, &img) == VK_SUCCESS, "image");
   p_GetImageMemoryRequirements(dev, img, &ir);
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = br.size, .memoryTypeIndex = 1};
   VkDeviceMemory bmem, imem;
   CHECK(p_AllocateMemory(dev, &mai, NULL, &bmem) == VK_SUCCESS, "buffer memory");
   mai.allocationSize = ir.size;
   mai.memoryTypeIndex = 0;
   CHECK(p_AllocateMemory(dev, &mai, NULL, &imem) == VK_SUCCESS, "image memory");
   CHECK(p_BindBufferMemory(dev, buf, bmem, 0) == VK_SUCCESS && p_BindImageMemory(dev, img, imem, 0) == VK_SUCCESS, "bind");
   CHECK(da(MALI_SITE_USER) == 2 && dall(false) == 2, "two VkDeviceMemory = two user BOs, nothing else: user %lld all %lld",
         da(MALI_SITE_USER), dall(false));

   snap();
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
   VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4, .poolSizeCount = 1, .pPoolSizes = &ps};
   VkDescriptorPool dp;
   CHECK(p_CreateDescriptorPool(dev, &dpci, NULL, &dp) == VK_SUCCESS, "descriptor pool");
   CHECK(da(MALI_SITE_DPOOL) == (on ? 0 : 1), "descriptor pool BOs %lld", da(MALI_SITE_DPOOL));
   p_DestroyDescriptorPool(dev, dp, NULL);
   CHECK(df(MALI_SITE_DPOOL) == (on ? 0 : 1), "descriptor pool frees %lld", df(MALI_SITE_DPOOL));

   if (view_cycle())
      return 1;
   CHECK(da(MALI_SITE_VIEW) == (on ? 0 : 1) && df(MALI_SITE_VIEW) == da(MALI_SITE_VIEW), "view BOs %lld/%lld", da(MALI_SITE_VIEW), df(MALI_SITE_VIEW));

   VkQueryPoolCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 8};
   VkEventCreateInfo eci = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
   VkQueryPool qp;
   VkEvent ev;
   CHECK(p_CreateQueryPool(dev, &qci, NULL, &qp) == VK_SUCCESS && p_CreateEvent(dev, &eci, NULL, &ev) == VK_SUCCESS, "query pool + event");
   CHECK(da(MALI_SITE_QEV) == (on ? 0 : 2), "query/event BOs %lld", da(MALI_SITE_QEV));
   p_DestroyQueryPool(dev, qp, NULL);
   p_DestroyEvent(dev, ev, NULL);
   CHECK(df(MALI_SITE_QEV) == (on ? 0 : 2), "query/event frees %lld", df(MALI_SITE_QEV));

   VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   CHECK(p_CreateCommandPool(dev, &cpci, NULL, &cpool) == VK_SUCCESS, "command pool");
   VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   CHECK(p_AllocateCommandBuffers(dev, &cai, &cb) == VK_SUCCESS, "command buffer");
   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(p_CreateFence(dev, &fci, NULL, &fence) == VK_SUCCESS, "fence");
   if (copy_frame(1))
      return 1;
   const long long meta_per_copy = da(MALI_SITE_META);
   CHECK(on ? meta_per_copy == 0 : meta_per_copy >= 1, "meta BOs for one copy: %lld", meta_per_copy);
   CHECK(df(MALI_SITE_META) == meta_per_copy, "meta BOs released at command-buffer reset: %lld of %lld", df(MALI_SITE_META), meta_per_copy);
   if (!on)   /* every non-chunk site is one BO per object; the suballocator never runs */
      CHECK(da(MALI_SITE_CHUNK) == 0, "chunk BOs with MALI_BO_SUBALLOC=0: %lld", da(MALI_SITE_CHUNK));

   /* ---- 2. steady state: 200 frames of 10 copies + 10 view cycles ---- */
   const unsigned frames = 200, per = 10, warm = 10;
   long long warm_allocs = 0;
   for (unsigned f = 0; f < frames; f++) {
      if (f == warm) {
         warm_allocs = dall(false);
         snap();
      }
      if (copy_frame(per))
         return 1;
      for (unsigned i = 0; i < per; i++)
         if (view_cycle())
            return 1;
   }
   const long long allocs = dall(false), frees = dall(true);
   const double per_frame = (double)allocs / (frames - warm);
   printf("{\"test\":\"bo_stats\",\"suballoc\":%d,\"meta_bos_per_copy\":%lld,\"warmup_allocs\":%lld,\"steady_allocs\":%lld,"
          "\"steady_frees\":%lld,\"allocs_per_frame\":%.2f,\"frame\":\"%u copies + %u view cycles\",\"meta\":%lld,\"view\":%lld}\n",
          on, meta_per_copy, warm_allocs, allocs, frees, per_frame, per, per, da(MALI_SITE_META), da(MALI_SITE_VIEW));
   if (on)
      CHECK(allocs == 0 && frees == 0, "suballocator on: %lld BO allocations and %lld frees after warm-up", allocs, frees);
   else
      CHECK(da(MALI_SITE_VIEW) == (long long)((frames - warm) * per) && da(MALI_SITE_META) == (long long)((frames - warm) * per) * meta_per_copy,
            "one BO per view and %lld per copy: view %lld meta %lld", meta_per_copy, da(MALI_SITE_VIEW), da(MALI_SITE_META));

   p_DestroyFence(dev, fence, NULL);
   p_DestroyCommandPool(dev, cpool, NULL);
   p_DestroyImage(dev, img, NULL);
   p_DestroyBuffer(dev, buf, NULL);
   p_FreeMemory(dev, imem, NULL);
   p_FreeMemory(dev, bmem, NULL);
   p_DestroyDevice(dev, NULL);
   return 0;
}

int
main(void)
{
   int r = run();
   if (!r)
      printf("{\"test\":\"bo_stats\",\"ok\":true}\n");
   return r;
}
