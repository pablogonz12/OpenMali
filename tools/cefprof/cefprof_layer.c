/* SPDX-License-Identifier: MIT
 * cefprof: host-only Vulkan layer that counts what an application asks of the driver, per presented frame.
 * Frame = one vkQueuePresentKHR. After CEFPROF_SKIP frames it snapshots every counter, CEFPROF_FRAMES later it writes
 * CEFPROF_OUT.<pid> (JSON) with window totals, and creates CEFPROF_OUT.done. Record-time counters are attributed to the
 * window in which the command was recorded. Not wired into check.sh. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "cefprof_enum.h"

struct dev {
   void *key;
   VkDevice device;
   PFN_vkGetDeviceProcAddr gdpa;
   PFN_vkVoidFunction next[C_COUNT];
};
#define MAXDEV 32
static struct dev g_devs[MAXDEV];
static unsigned g_ndev, g_dev_created, g_inst_created;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t g_calls[C_COUNT], g_ns[C_COUNT];
static unsigned g_frame;
static unsigned g_skip = 150, g_window = 300;
static bool g_in_window, g_done;
static uint64_t g_t_start, g_t_end;

static uint64_t
now(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void
count_cmd(int c, uint64_t dt)
{
   __atomic_add_fetch(&g_calls[c], 1, __ATOMIC_RELAXED);
   __atomic_add_fetch(&g_ns[c], dt, __ATOMIC_RELAXED);
}

static struct dev *
dev_of(const void *handle)
{
   void *key = *(void *const *)handle;
   for (unsigned i = 0; i < g_ndev; i++)
      if (g_devs[i].key == key)
         return &g_devs[i];
   fprintf(stderr, "cefprof: unknown dispatch key\n");
   abort();
}

/* ---- string-keyed counters ---- */
#define KCAP (1u << 17)
struct kv {
   char *k;
   uint64_t all, win, first_frame, last_frame;
};
static struct kv g_kv[KCAP];
static unsigned g_nkv;

static struct kv *
kget(const char *k)
{
   uint64_t h = 1469598103934665603ull;
   for (const char *p = k; *p; p++)
      h = (h ^ (unsigned char)*p) * 1099511628211ull;
   for (unsigned i = (unsigned)h & (KCAP - 1);; i = (i + 1) & (KCAP - 1)) {
      if (!g_kv[i].k) {
         if (g_nkv > KCAP - KCAP / 8)
            return NULL;
         g_nkv++;
         g_kv[i].k = strdup(k);
         g_kv[i].first_frame = g_frame;
         return &g_kv[i];
      }
      if (!strcmp(g_kv[i].k, k))
         return &g_kv[i];
   }
}

static void __attribute__((format(printf, 2, 3)))
addf(uint64_t n, const char *fmt, ...)
{
   char buf[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof buf, fmt, ap);
   va_end(ap);
   struct kv *e = kget(buf);
   if (!e)
      return;
   e->all += n;
   e->last_frame = g_frame;
   if (g_in_window)
      e->win += n;
}

/* ---- handle -> object info ---- */
#define OCAP (1u << 18)
struct obj {
   uint64_t h;
   int type;
   void *p;
};
static struct obj g_obj[OCAP];

static unsigned
oslot(uint64_t h, int type)
{
   unsigned i = (unsigned)((h * 0x9E3779B97F4A7C15ull) >> 40) & (OCAP - 1);
   while (g_obj[i].h && !(g_obj[i].h == h && g_obj[i].type == type))
      i = (i + 1) & (OCAP - 1);
   return i;
}
static void
oset(uint64_t h, int type, void *p)
{
   if (!h)
      return;
   unsigned i = oslot(h, type);
   free(g_obj[i].p); /* handle reuse: the old record is dead */
   g_obj[i] = (struct obj){h, type, p};
}
static void *
oget(uint64_t h, int type)
{
   if (!h)
      return NULL;
   return g_obj[oslot(h, type)].p;
}
#define U(x) ((uint64_t)(uintptr_t)(x))
enum { O_IMAGE, O_VIEW, O_PASS, O_FB, O_CMD, O_BUFFER, O_PIPE, O_MEM };

struct img {
   VkImageCreateInfo ci;
};
struct view {
   uint64_t image;
   VkFormat fmt;
};
struct pass {
   unsigned natt, ncolor, nsub;
   VkAttachmentDescription att[10];
   bool is_color[10];
};
struct fb {
   uint32_t w, h, n;
   uint64_t views[10];
};
struct cmd {
   bool in_pass;
   unsigned draws;        /* draws of the open pass (primary: includes executed secondaries) */
   unsigned total_draws;  /* draws recorded into this buffer since vkBeginCommandBuffer */
   char desc[256];
   bool load_color, load_zs;
};
struct pipe {
   unsigned created_frame;
   bool bound;
};

static struct cmd *
cmd_of(VkCommandBuffer cb)
{
   struct cmd *c = oget(U(cb), O_CMD);
   if (!c) {
      c = calloc(1, sizeof(*c));
      oset(U(cb), O_CMD, c);
   }
   return c;
}

static void
feat(FILE *o, const char *name, int *first)
{
   fprintf(o, "%s%s", *first ? "" : " ", name);
   *first = 0;
}
#include "cefprof_names.h"

static const char *
img_str(uint64_t image, char *buf, size_t n)
{
   struct img *im = oget(image, O_IMAGE);
   if (!im)
      snprintf(buf, n, "fmt=? img=swapchain-or-unknown");
   else
      snprintf(buf, n, "fmt=%s img=%ux%u mips=%u samples=%u tiling=%d usage=0x%x", fmt_name(im->ci.format), im->ci.extent.width, im->ci.extent.height,
               im->ci.mipLevels, im->ci.samples, im->ci.tiling, im->ci.usage);
   return buf;
}

static unsigned
fmt_bytes(VkFormat f)
{
   switch ((int)f) {
   case VK_FORMAT_R8_UNORM: return 1;
   case VK_FORMAT_R8G8_UNORM: return 2;
   case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
   default: return 4;
   }
}

static const char *
size_bucket(uint64_t bytes)
{
   static const char *n[] = {"<=64B", "<=256B", "<=1KiB", "<=4KiB", "<=16KiB", "<=64KiB", "<=256KiB", "<=1MiB", "<=4MiB", "<=16MiB", ">16MiB"};
   uint64_t lim = 64;
   for (int i = 0; i < 10; i++, lim *= 4)
      if (bytes <= lim)
         return n[i];
   return n[10];
}

#include "cefprof_protos.h"
#include "cefprof_gen.h"

#define H(n)

/* ---- submission, sync, present ---- */
static void report(void);

H(QueueSubmit)
static void hook_QueueSubmit(struct dev *d, uint64_t dt, VkResult r, VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence)
{
   unsigned cbs = 0, waits = 0, sigs = 0;
   for (uint32_t i = 0; i < submitCount; i++) {
      cbs += pSubmits[i].commandBufferCount;
      waits += pSubmits[i].waitSemaphoreCount;
      sigs += pSubmits[i].signalSemaphoreCount;
   }
   addf(submitCount, "sub.submitinfos");
   addf(cbs, "sub.cmdbufs");
   addf(waits, "sub.waitsems");
   addf(sigs, "sub.signalsems");
   addf(1, "sub.shape dev=%d cbs=%u waits=%u signals=%u fence=%d", (int)(d - g_devs), cbs, waits, sigs, fence != VK_NULL_HANDLE);
}

H(QueueSubmit2)
static void hook_QueueSubmit2(struct dev *d, uint64_t dt, VkResult r, VkQueue queue, uint32_t submitCount, const VkSubmitInfo2 *pSubmits, VkFence fence)
{
   unsigned cbs = 0;
   for (uint32_t i = 0; i < submitCount; i++)
      cbs += pSubmits[i].commandBufferInfoCount;
   addf(submitCount, "sub.submitinfos");
   addf(cbs, "sub.cmdbufs");
   addf(1, "sub.shape2 dev=%d cbs=%u fence=%d", (int)(d - g_devs), cbs, fence != VK_NULL_HANDLE);
}

H(QueuePresentKHR)
static void hook_QueuePresentKHR(struct dev *d, uint64_t dt, VkResult r, VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
   addf(1, "present result=%d dev=%d waits=%u", r, (int)(d - g_devs), pPresentInfo->waitSemaphoreCount);
   for (const VkBaseInStructure *s = pPresentInfo->pNext; s; s = s->pNext)
      addf(1, "present.pnext sType=%d", s->sType);
   g_frame++;
   if (!g_done && g_frame == g_skip) {
      extern void cefprof_snapshot_start(void);
      cefprof_snapshot_start();
   } else if (!g_done && g_in_window && g_frame == g_skip + g_window) {
      report();
   }
}

H(WaitForFences)
static void hook_WaitForFences(struct dev *d, uint64_t dt, VkResult r, VkDevice device, uint32_t fenceCount, const VkFence *pFences, VkBool32 waitAll, uint64_t timeout)
{
   addf(1, "wait.fences timeout=%s result=%d", timeout == 0 ? "0" : timeout == UINT64_MAX ? "inf" : "finite", r);
}
H(GetFenceStatus)
static void hook_GetFenceStatus(struct dev *d, uint64_t dt, VkResult r, VkDevice device, VkFence fence)
{
   addf(1, "wait.fencestatus result=%d", r);
}

H(CreateSwapchainKHR)
static void hook_CreateSwapchainKHR(struct dev *d, uint64_t dt, VkResult r, VkDevice device, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *out)
{
   addf(1, "swapchain dev=%d presentMode=%d minImageCount=%u fmt=%s extent=%ux%u usage=0x%x result=%d old=%d", (int)(d - g_devs), ci->presentMode, ci->minImageCount,
        fmt_name(ci->imageFormat), ci->imageExtent.width, ci->imageExtent.height, ci->imageUsage, r, ci->oldSwapchain != VK_NULL_HANDLE);
}

H(GetDeviceQueue)
static void hook_GetDeviceQueue(struct dev *d, uint64_t dt, VkDevice device, uint32_t family, uint32_t index, VkQueue *q)
{
   addf(1, "queue.get dev=%d family=%u index=%u", (int)(d - g_devs), family, index);
}
H(GetDeviceQueue2)
static void hook_GetDeviceQueue2(struct dev *d, uint64_t dt, VkDevice device, const VkDeviceQueueInfo2 *i, VkQueue *q)
{
   addf(1, "queue.get2 dev=%d family=%u index=%u", (int)(d - g_devs), i->queueFamilyIndex, i->queueIndex);
}
H(DestroyDevice)
static void hook_DestroyDevice(struct dev *d, uint64_t dt, VkDevice device, const VkAllocationCallbacks *a)
{
   addf(1, "device.destroy dev=%d", (int)(d - g_devs));
}

/* ---- memory and objects ---- */
H(AllocateMemory)
static void hook_AllocateMemory(struct dev *d, uint64_t dt, VkResult r, VkDevice device, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out)
{
   int dedicated = 0, exported = 0, imported = 0;
   for (const VkBaseInStructure *s = ai->pNext; s; s = s->pNext) {
      dedicated |= s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
      exported |= s->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
      imported |= s->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
   }
   addf(1, "mem.alloc size%s type=%u dedicated=%d export=%d import=%d", size_bucket(ai->allocationSize), ai->memoryTypeIndex, dedicated, exported, imported);
   addf(ai->allocationSize, "mem.alloc_bytes");
}
H(MapMemory)
static void hook_MapMemory(struct dev *d, uint64_t dt, VkResult r, VkDevice device, VkDeviceMemory m, VkDeviceSize off, VkDeviceSize size, VkMemoryMapFlags f, void **pp)
{
   addf(1, "mem.map size=%s", size == VK_WHOLE_SIZE ? "whole" : size_bucket(size));
}
H(FlushMappedMemoryRanges)
static void hook_FlushMappedMemoryRanges(struct dev *d, uint64_t dt, VkResult r, VkDevice device, uint32_t n, const VkMappedMemoryRange *p)
{
   addf(n, "mem.flush_ranges");
}
H(InvalidateMappedMemoryRanges)
static void hook_InvalidateMappedMemoryRanges(struct dev *d, uint64_t dt, VkResult r, VkDevice device, uint32_t n, const VkMappedMemoryRange *p)
{
   addf(n, "mem.invalidate_ranges");
}

H(CreateImage)
static void hook_CreateImage(struct dev *d, uint64_t dt, VkResult r, VkDevice device, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out)
{
   if (r)
      return;
   struct img *im = calloc(1, sizeof(*im));
   im->ci = *ci;
   oset(U(*out), O_IMAGE, im);
   addf(1, "image.create dev=%d fmt=%s %ux%u mips=%u layers=%u samples=%u tiling=%d usage=0x%x flags=0x%x", (int)(d - g_devs), fmt_name(ci->format), ci->extent.width,
        ci->extent.height, ci->mipLevels, ci->arrayLayers, ci->samples, ci->tiling, ci->usage, ci->flags);
}
H(CreateImageView)
static void hook_CreateImageView(struct dev *d, uint64_t dt, VkResult r, VkDevice device, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out)
{
   if (r)
      return;
   struct view *v = calloc(1, sizeof(*v));
   v->image = U(ci->image);
   v->fmt = ci->format;
   oset(U(*out), O_VIEW, v);
}
H(CreateBuffer)
static void hook_CreateBuffer(struct dev *d, uint64_t dt, VkResult r, VkDevice device, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out)
{
   addf(1, "buffer.create size%s usage=0x%x", size_bucket(ci->size), ci->usage);
}

/* ---- render passes ---- */
static void
store_pass(VkRenderPass h, struct pass *p)
{
   oset(U(h), O_PASS, p);
}
H(CreateRenderPass)
static void hook_CreateRenderPass(struct dev *d, uint64_t dt, VkResult r, VkDevice device, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *a, VkRenderPass *out)
{
   if (r)
      return;
   struct pass *p = calloc(1, sizeof(*p));
   p->natt = ci->attachmentCount < 10 ? ci->attachmentCount : 10;
   p->nsub = ci->subpassCount;
   memcpy(p->att, ci->pAttachments, p->natt * sizeof(p->att[0]));
   for (uint32_t s = 0; s < ci->subpassCount; s++)
      for (uint32_t i = 0; i < ci->pSubpasses[s].colorAttachmentCount; i++) {
         uint32_t x = ci->pSubpasses[s].pColorAttachments[i].attachment;
         if (x < 10)
            p->is_color[x] = true;
      }
   store_pass(*out, p);
}
static void
pass2(VkResult r, const VkRenderPassCreateInfo2 *ci, VkRenderPass *out)
{
   if (r)
      return;
   struct pass *p = calloc(1, sizeof(*p));
   p->natt = ci->attachmentCount < 10 ? ci->attachmentCount : 10;
   p->nsub = ci->subpassCount;
   for (unsigned i = 0; i < p->natt; i++) {
      const VkAttachmentDescription2 *s = &ci->pAttachments[i];
      p->att[i] = (VkAttachmentDescription){.format = s->format, .samples = s->samples, .loadOp = s->loadOp, .storeOp = s->storeOp,
                                            .stencilLoadOp = s->stencilLoadOp, .stencilStoreOp = s->stencilStoreOp};
   }
   for (uint32_t s = 0; s < ci->subpassCount; s++)
      for (uint32_t i = 0; i < ci->pSubpasses[s].colorAttachmentCount; i++) {
         uint32_t x = ci->pSubpasses[s].pColorAttachments[i].attachment;
         if (x < 10)
            p->is_color[x] = true;
      }
   store_pass(*out, p);
}
H(CreateRenderPass2)
static void hook_CreateRenderPass2(struct dev *d, uint64_t dt, VkResult r, VkDevice device, const VkRenderPassCreateInfo2 *ci, const VkAllocationCallbacks *a, VkRenderPass *out)
{
   pass2(r, ci, out);
}
H(CreateRenderPass2KHR)
static void hook_CreateRenderPass2KHR(struct dev *d, uint64_t dt, VkResult r, VkDevice device, const VkRenderPassCreateInfo2 *ci, const VkAllocationCallbacks *a, VkRenderPass *out)
{
   pass2(r, ci, out);
}
H(CreateFramebuffer)
static void hook_CreateFramebuffer(struct dev *d, uint64_t dt, VkResult r, VkDevice device, const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *a, VkFramebuffer *out)
{
   if (r)
      return;
   struct fb *f = calloc(1, sizeof(*f));
   f->w = ci->width;
   f->h = ci->height;
   f->n = ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT ? 0 : ci->attachmentCount < 10 ? ci->attachmentCount : 10;
   for (unsigned i = 0; i < f->n; i++)
      f->views[i] = U(ci->pAttachments[i]);
   oset(U(*out), O_FB, f);
}

static const char *op_name(int op) { return op == 0 ? "LOAD" : op == 1 ? "CLEAR" : op == 2 ? "DONT_CARE" : "NONE"; }

static void
begin_pass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi)
{
   struct cmd *c = cmd_of(cb);
   struct pass *p = oget(U(bi->renderPass), O_PASS);
   struct fb *f = oget(U(bi->framebuffer), O_FB);
   c->in_pass = true;
   c->draws = 0;
   c->load_color = c->load_zs = false;
   int n = snprintf(c->desc, sizeof c->desc, "fb=%ux%u area=%ux%u@%d,%d", f ? f->w : 0, f ? f->h : 0, bi->renderArea.extent.width, bi->renderArea.extent.height,
                    bi->renderArea.offset.x, bi->renderArea.offset.y);
   for (unsigned i = 0; p && i < p->natt && n < (int)sizeof c->desc - 60; i++) {
      bool zs = p->att[i].format >= VK_FORMAT_D16_UNORM && p->att[i].format <= VK_FORMAT_D32_SFLOAT_S8_UINT;
      if (zs) {
         c->load_zs |= p->att[i].loadOp == 0 || p->att[i].stencilLoadOp == 0;
         n += snprintf(c->desc + n, sizeof c->desc - n, " [zs %s d=%s s=%s x%u]", fmt_name(p->att[i].format), op_name(p->att[i].loadOp),
                       op_name(p->att[i].stencilLoadOp), p->att[i].samples);
      } else {
         c->load_color |= p->att[i].loadOp == 0 && p->is_color[i];
         n += snprintf(c->desc + n, sizeof c->desc - n, " [%s %s %s/%s x%u]", p->is_color[i] ? "c" : "res", fmt_name(p->att[i].format), op_name(p->att[i].loadOp),
                       p->att[i].storeOp == 0 ? "STORE" : "DONT_CARE", p->att[i].samples);
      }
   }
   if (p && p->nsub > 1)
      addf(1, "pass.multi_subpass n=%u", p->nsub);
}
static void
end_pass(VkCommandBuffer cb)
{
   struct cmd *c = cmd_of(cb);
   if (!c->in_pass)
      return;
   c->in_pass = false;
   addf(1, "pass.total");
   addf(c->draws, "pass.draws_attributed");
   if (c->load_color)
      addf(1, "pass.load_color");
   if (c->load_zs)
      addf(1, "pass.load_zs");
   if (c->load_color || c->load_zs)
      addf(1, "pass.load_any");
   if (!c->draws)
      addf(1, "pass.zero_draw");
   if (c->draws == 1)
      addf(1, "pass.one_draw");
   addf(1, "pass.kind %s draws=%s", c->desc, c->draws == 0 ? "0" : c->draws == 1 ? "1" : c->draws <= 4 ? "2-4" : c->draws <= 16 ? "5-16" : c->draws <= 64 ? "17-64" : ">64");
}
H(CmdBeginRenderPass)
static void hook_CmdBeginRenderPass(struct dev *d, uint64_t dt, VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents contents)
{
   begin_pass(cb, bi);
}
H(CmdBeginRenderPass2)
static void hook_CmdBeginRenderPass2(struct dev *d, uint64_t dt, VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, const VkSubpassBeginInfo *s)
{
   begin_pass(cb, bi);
}
H(CmdBeginRenderPass2KHR)
static void hook_CmdBeginRenderPass2KHR(struct dev *d, uint64_t dt, VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, const VkSubpassBeginInfo *s)
{
   begin_pass(cb, bi);
}
H(CmdEndRenderPass)
static void hook_CmdEndRenderPass(struct dev *d, uint64_t dt, VkCommandBuffer cb) { end_pass(cb); }
H(CmdEndRenderPass2)
static void hook_CmdEndRenderPass2(struct dev *d, uint64_t dt, VkCommandBuffer cb, const VkSubpassEndInfo *s) { end_pass(cb); }
H(CmdEndRenderPass2KHR)
static void hook_CmdEndRenderPass2KHR(struct dev *d, uint64_t dt, VkCommandBuffer cb, const VkSubpassEndInfo *s) { end_pass(cb); }

static void
begin_rendering(VkCommandBuffer cb, const VkRenderingInfo *ri)
{
   struct cmd *c = cmd_of(cb);
   c->in_pass = true;
   c->draws = 0;
   c->load_color = c->load_zs = false;
   for (uint32_t i = 0; i < ri->colorAttachmentCount; i++)
      c->load_color |= ri->pColorAttachments[i].imageView && ri->pColorAttachments[i].loadOp == 0;
   snprintf(c->desc, sizeof c->desc, "dynamic area=%ux%u colors=%u", ri->renderArea.extent.width, ri->renderArea.extent.height, ri->colorAttachmentCount);
}
H(CmdBeginRendering)
static void hook_CmdBeginRendering(struct dev *d, uint64_t dt, VkCommandBuffer cb, const VkRenderingInfo *ri) { begin_rendering(cb, ri); }
H(CmdBeginRenderingKHR)
static void hook_CmdBeginRenderingKHR(struct dev *d, uint64_t dt, VkCommandBuffer cb, const VkRenderingInfo *ri) { begin_rendering(cb, ri); }
H(CmdEndRendering)
static void hook_CmdEndRendering(struct dev *d, uint64_t dt, VkCommandBuffer cb) { end_pass(cb); }
H(CmdEndRenderingKHR)
static void hook_CmdEndRenderingKHR(struct dev *d, uint64_t dt, VkCommandBuffer cb) { end_pass(cb); }

H(BeginCommandBuffer)
static void hook_BeginCommandBuffer(struct dev *d, uint64_t dt, VkResult r, VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
   struct cmd *c = cmd_of(cb);
   memset(c, 0, sizeof(*c));
}
/* ANGLE records each render pass body into a VkCommandBuffer of level SECONDARY: its draws belong to the primary's open pass. */
H(CmdExecuteCommands)
static void hook_CmdExecuteCommands(struct dev *d, uint64_t dt, VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *s)
{
   struct cmd *c = cmd_of(cb);
   for (uint32_t i = 0; i < n; i++) {
      unsigned k = cmd_of(s[i])->total_draws;
      c->draws += k;
      c->total_draws += k;
      addf(k, "draw.via_secondary");
   }
}

/* ---- draws and state ---- */
static void
draw(VkCommandBuffer cb, const char *kind, uint32_t n, uint32_t inst)
{
   struct cmd *c = cmd_of(cb);
   c->draws++;
   c->total_draws++;
   addf(1, "draw.total");
   addf(1, "draw.kind %s verts=%s inst=%s", kind, n <= 4 ? "<=4" : n <= 6 ? "5-6" : n <= 60 ? "7-60" : n <= 600 ? "61-600" : ">600", inst <= 1 ? "1" : ">1");
}
H(CmdDraw)
static void hook_CmdDraw(struct dev *d, uint64_t dt, VkCommandBuffer cb, uint32_t v, uint32_t i, uint32_t fv, uint32_t fi) { draw(cb, "draw", v, i); }
H(CmdDrawIndexed)
static void hook_CmdDrawIndexed(struct dev *d, uint64_t dt, VkCommandBuffer cb, uint32_t n, uint32_t i, uint32_t fx, int32_t vo, uint32_t fi) { draw(cb, "indexed", n, i); }
H(CmdDrawIndirect)
static void hook_CmdDrawIndirect(struct dev *d, uint64_t dt, VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) { draw(cb, "indirect", 0, 1); }
H(CmdDrawIndexedIndirect)
static void hook_CmdDrawIndexedIndirect(struct dev *d, uint64_t dt, VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) { draw(cb, "indexed-indirect", 0, 1); }

H(CmdBindPipeline)
static void hook_CmdBindPipeline(struct dev *d, uint64_t dt, VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
   struct pipe *x = oget(U(p), O_PIPE);
   if (x && !x->bound) {
      x->bound = true;
      addf(1, "pipe.distinct_bound");
      addf(1, "pipe.first_bound frame=%05u", g_frame);
   }
}
H(CmdBindDescriptorSets)
static void hook_CmdBindDescriptorSets(struct dev *d, uint64_t dt, VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *s, uint32_t nd, const uint32_t *dyn)
{
   addf(n, "bind.descriptor_sets");
   addf(nd, "bind.dynamic_offsets");
}
H(CmdPushConstants)
static void hook_CmdPushConstants(struct dev *d, uint64_t dt, VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags f, uint32_t off, uint32_t size, const void *v)
{
   addf(size, "push.bytes");
}
static void
pipes(uint32_t n, VkPipeline *out, VkResult r, uint64_t dt, const char *kind)
{
   for (uint32_t i = 0; i < n; i++)
      if (!r && out[i]) {
         struct pipe *x = calloc(1, sizeof(*x));
         x->created_frame = g_frame;
         oset(U(out[i]), O_PIPE, x);
      }
   addf(n, "pipe.created_%s", kind);
   addf(n, "pipe.create_frame=%05u", g_frame);
   addf(dt / 1000, "pipe.create_us");
}
H(CreateGraphicsPipelines)
static void hook_CreateGraphicsPipelines(struct dev *d, uint64_t dt, VkResult r, VkDevice device, VkPipelineCache pc, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out)
{
   pipes(n, out, r, dt, "gfx");
}
H(CreateComputePipelines)
static void hook_CreateComputePipelines(struct dev *d, uint64_t dt, VkResult r, VkDevice device, VkPipelineCache pc, uint32_t n, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out)
{
   pipes(n, out, r, dt, "compute");
}
H(UpdateDescriptorSets)
static void hook_UpdateDescriptorSets(struct dev *d, uint64_t dt, VkDevice device, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c)
{
   addf(nw, "desc.writes");
   addf(nc, "desc.copies");
   for (uint32_t i = 0; i < nw; i++)
      addf(w[i].descriptorCount, "desc.write type=%d", w[i].descriptorType);
}
H(AllocateDescriptorSets)
static void hook_AllocateDescriptorSets(struct dev *d, uint64_t dt, VkResult r, VkDevice device, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out)
{
   addf(ai->descriptorSetCount, "desc.sets_allocated");
}

/* ---- barriers ---- */
H(CmdPipelineBarrier)
static void hook_CmdPipelineBarrier(struct dev *d, uint64_t dt, VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags f, uint32_t nm,
                                    const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im)
{
   addf(nm, "barrier.memory");
   addf(nb, "barrier.buffer");
   addf(ni, "barrier.image");
   addf(1, "barrier.in_pass=%d src=0x%x dst=0x%x", cmd_of(cb)->in_pass, src, dst);
}

/* ---- transfers ---- */
static void
copy_region(const char *op, uint64_t image, VkOffset3D off, VkExtent3D ext, int64_t buf_off, uint32_t row_len, uint32_t buf_h, unsigned mip)
{
   char ib[256];
   struct img *im = oget(image, O_IMAGE);
   unsigned bpp = im ? fmt_bytes(im->ci.format) : 4;
   uint64_t bytes = (uint64_t)ext.width * ext.height * bpp;
   const char *fn = im ? fmt_name(im->ci.format) : "swapchain/?";
   addf(1, "copy.%s.count", op);
   addf(bytes, "copy.%s.bytes", op);
   addf(1, "copy.%s fmt=%s size%s", op, fn, size_bucket(bytes));
   if (ext.width % 4 || off.x % 4)
      addf(1, "copy.%s.unaligned_x fmt=%s", op, fn);
   /* full upload tuple: what the icon-corruption lane needs */
   addf(1, "region op=%s %s ext=%ux%u off=%d,%d mip=%u bufOff=%" PRId64 " bufOff%%4=%d bufOff%%64=%d rowLen=%u imgH=%u", op, img_str(image, ib, sizeof ib), ext.width,
        ext.height, off.x, off.y, mip, im && im->ci.format == VK_FORMAT_R8_UNORM ? buf_off : -1, buf_off < 0 ? -1 : (int)(buf_off % 4),
        buf_off < 0 ? -1 : (int)(buf_off % 64), row_len, buf_h);
}
#define B2I(name, type)                                                                                                   \
   static void name(VkImage img, uint32_t n, const type *r, const char *op)                                               \
   {                                                                                                                      \
      for (uint32_t i = 0; i < n; i++)                                                                                    \
         copy_region(op, U(img), r[i].imageOffset, r[i].imageExtent, (int64_t)r[i].bufferOffset, r[i].bufferRowLength,    \
                     r[i].bufferImageHeight, r[i].imageSubresource.mipLevel);                                             \
   }
B2I(b2i, VkBufferImageCopy)
B2I(b2i2, VkBufferImageCopy2)
H(CmdCopyBufferToImage)
static void hook_CmdCopyBufferToImage(struct dev *d, uint64_t dt, VkCommandBuffer cb, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) { b2i(i, n, r, "buf2img"); }
H(CmdCopyBufferToImage2)
static void hook_CmdCopyBufferToImage2(struct dev *d, uint64_t dt, VkCommandBuffer cb, const VkCopyBufferToImageInfo2 *i) { b2i2(i->dstImage, i->regionCount, i->pRegions, "buf2img"); }
H(CmdCopyBufferToImage2KHR)
static void hook_CmdCopyBufferToImage2KHR(struct dev *d, uint64_t dt, VkCommandBuffer cb, const VkCopyBufferToImageInfo2 *i) { b2i2(i->dstImage, i->regionCount, i->pRegions, "buf2img"); }
H(CmdCopyImageToBuffer)
static void hook_CmdCopyImageToBuffer(struct dev *d, uint64_t dt, VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *r) { b2i(i, n, r, "img2buf"); }
H(CmdCopyImageToBuffer2)
static void hook_CmdCopyImageToBuffer2(struct dev *d, uint64_t dt, VkCommandBuffer cb, const VkCopyImageToBufferInfo2 *i) { b2i2(i->srcImage, i->regionCount, i->pRegions, "img2buf"); }
H(CmdCopyImageToBuffer2KHR)
static void hook_CmdCopyImageToBuffer2KHR(struct dev *d, uint64_t dt, VkCommandBuffer cb, const VkCopyImageToBufferInfo2 *i) { b2i2(i->srcImage, i->regionCount, i->pRegions, "img2buf"); }
H(CmdCopyImage)
static void hook_CmdCopyImage(struct dev *d, uint64_t dt, VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy *r)
{
   char a[256];
   for (uint32_t i = 0; i < n; i++) {
      copy_region("img2img", U(dst), r[i].dstOffset, r[i].extent, -1, 0, 0, r[i].dstSubresource.mipLevel);
      addf(1, "img2img.src %s srcOff=%d,%d ext=%ux%u", img_str(U(s), a, sizeof a), r[i].srcOffset.x, r[i].srcOffset.y, r[i].extent.width, r[i].extent.height);
   }
}
H(CmdBlitImage)
static void hook_CmdBlitImage(struct dev *d, uint64_t dt, VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter filter)
{
   for (uint32_t i = 0; i < n; i++) {
      VkOffset3D o = r[i].dstOffsets[0];
      VkExtent3D e = {(uint32_t)abs(r[i].dstOffsets[1].x - o.x), (uint32_t)abs(r[i].dstOffsets[1].y - o.y), 1};
      copy_region("blit", U(dst), o, e, -1, 0, 0, r[i].dstSubresource.mipLevel);
   }
}
H(CmdResolveImage)
static void hook_CmdResolveImage(struct dev *d, uint64_t dt, VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageResolve *r)
{
   for (uint32_t i = 0; i < n; i++)
      copy_region("resolve", U(dst), r[i].dstOffset, r[i].extent, -1, 0, 0, 0);
}
H(CmdCopyBuffer)
static void hook_CmdCopyBuffer(struct dev *d, uint64_t dt, VkCommandBuffer cb, VkBuffer s, VkBuffer dst, uint32_t n, const VkBufferCopy *r)
{
   for (uint32_t i = 0; i < n; i++) {
      addf(1, "copy.buf2buf.count");
      addf(r[i].size, "copy.buf2buf.bytes");
      addf(1, "copy.buf2buf size%s", size_bucket(r[i].size));
   }
}
H(CmdClearColorImage)
static void hook_CmdClearColorImage(struct dev *d, uint64_t dt, VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearColorValue *c, uint32_t n, const VkImageSubresourceRange *r)
{
   char a[256];
   addf(1, "clear.color_image %s", img_str(U(img), a, sizeof a));
}
H(CmdClearDepthStencilImage)
static void hook_CmdClearDepthStencilImage(struct dev *d, uint64_t dt, VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearDepthStencilValue *c, uint32_t n, const VkImageSubresourceRange *r)
{
   char a[256];
   addf(1, "clear.zs_image %s", img_str(U(img), a, sizeof a));
}
H(CmdClearAttachments)
static void hook_CmdClearAttachments(struct dev *d, uint64_t dt, VkCommandBuffer cb, uint32_t na, const VkClearAttachment *a, uint32_t nr, const VkClearRect *r)
{
   for (uint32_t i = 0; i < nr; i++)
      addf(1, "clear.attachments aspect=0x%x rect=%ux%u after_draws=%d", na ? a[0].aspectMask : 0, r[i].rect.extent.width, r[i].rect.extent.height, cmd_of(cb)->draws > 0);
}
H(CmdUpdateBuffer)
static void hook_CmdUpdateBuffer(struct dev *d, uint64_t dt, VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkDeviceSize size, const void *data)
{
   addf(size, "update_buffer.bytes");
}

/* ---- queries ---- */
H(CreateQueryPool)
static void hook_CreateQueryPool(struct dev *d, uint64_t dt, VkResult r, VkDevice device, const VkQueryPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkQueryPool *out)
{
   addf(1, "query.pool type=%d count=%u", ci->queryType, ci->queryCount);
}

/* ---- snapshot and report ---- */
static uint64_t g_calls0[C_COUNT], g_ns0[C_COUNT];
#define NKS 520
static volatile uint64_t *g_kstats; /* written by kbstats.c in the kbase-on-fake-kernel driver build */
static uint64_t g_kstats0[NKS];

static void
map_kstats(void)
{
   const char *p = getenv("CEFPROF_KSTATS");
   if (g_kstats || !p)
      return;
   int fd = open(p, O_RDWR | O_CREAT, 0600);
   if (fd < 0)
      return;
   if (!ftruncate(fd, NKS * 8)) {
      void *m = mmap(NULL, NKS * 8, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (m != MAP_FAILED)
         g_kstats = m;
   }
   close(fd);
}

void
cefprof_snapshot_start(void)
{
   memcpy(g_calls0, g_calls, sizeof g_calls);
   memcpy(g_ns0, g_ns, sizeof g_ns);
   map_kstats();
   for (int i = 0; g_kstats && i < NKS; i++)
      g_kstats0[i] = g_kstats[i];
   g_t_start = now();
   g_in_window = true;
}

static void
jstr(FILE *f, const char *s)
{
   fputc('"', f);
   for (; *s; s++) {
      if (*s == '"' || *s == '\\')
         fputc('\\', f);
      fputc(*s, f);
   }
   fputc('"', f);
}

static void
report(void)
{
   const char *out = getenv("CEFPROF_OUT");
   if (!out || g_done)
      return;
   g_done = true;
   g_t_end = now();
   unsigned frames = g_frame - g_skip;
   g_in_window = false;
   char path[512];
   snprintf(path, sizeof path, "%s.%d", out, getpid());
   FILE *f = fopen(path, "w");
   if (!f)
      return;
   fprintf(f, "{\"frames\":%u,\"skip\":%u,\"seconds\":%.3f,\"instances\":%u,\"devices_created\":%u,\n\"cmds\":{", frames, g_skip, (g_t_end - g_t_start) * 1e-9,
           g_inst_created, g_dev_created);
   for (int i = 0, first = 1; i < C_COUNT; i++) {
      if (!g_calls[i])
         continue;
      fprintf(f, "%s\n\"%s\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]", first ? "" : ",", cmd_names[i], g_calls[i] - g_calls0[i], g_ns[i] - g_ns0[i],
              g_calls[i], g_ns[i]);
      first = 0;
   }
   fprintf(f, "},\n\"kv\":[");
   for (unsigned i = 0, first = 1; i < KCAP; i++) {
      if (!g_kv[i].k)
         continue;
      fprintf(f, "%s\n[", first ? "" : ",");
      jstr(f, g_kv[i].k);
      fprintf(f, ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]", g_kv[i].win, g_kv[i].all, g_kv[i].first_frame, g_kv[i].last_frame);
      first = 0;
   }
   fprintf(f, "],\n\"kstats_window\":[");
   for (int i = 0; g_kstats && i < NKS; i++)
      fprintf(f, "%s%" PRIu64, i ? "," : "", g_kstats[i] - g_kstats0[i]);
   fprintf(f, "],\n\"kstats_total\":[");
   for (int i = 0; g_kstats && i < NKS; i++)
      fprintf(f, "%s%" PRIu64, i ? "," : "", g_kstats[i]);
   fprintf(f, "]}\n");
   fclose(f);
   snprintf(path, sizeof path, "%s.done", out);
   FILE *m = fopen(path, "w");
   if (m)
      fclose(m);
   fprintf(stderr, "cefprof: report written after %u frames\n", g_frame);
}

/* ---- layer plumbing ---- */
static PFN_vkGetInstanceProcAddr g_next_gipa;
static VkInstance g_instance;

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out)
{
   VkLayerInstanceCreateInfo *li = (void *)ci->pNext;
   while (li && !(li->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && li->function == VK_LAYER_LINK_INFO))
      li = (void *)li->pNext;
   if (!li)
      return VK_ERROR_INITIALIZATION_FAILED;
   PFN_vkGetInstanceProcAddr gipa = li->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   li->u.pLayerInfo = li->u.pLayerInfo->pNext;
   PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkResult r = create(ci, a, out);
   if (!r) {
      g_next_gipa = gipa;
      g_instance = *out;
      g_inst_created++;
      const char *s = getenv("CEFPROF_SKIP"), *w = getenv("CEFPROF_FRAMES");
      if (s)
         g_skip = (unsigned)atoi(s);
      if (w)
         g_window = (unsigned)atoi(w);
      map_kstats();
   }
   return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out)
{
   VkLayerDeviceCreateInfo *li = (void *)ci->pNext;
   while (li && !(li->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && li->function == VK_LAYER_LINK_INFO))
      li = (void *)li->pNext;
   if (!li)
      return VK_ERROR_INITIALIZATION_FAILED;
   PFN_vkGetInstanceProcAddr gipa = li->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   PFN_vkGetDeviceProcAddr gdpa = li->u.pLayerInfo->pfnNextGetDeviceProcAddr;
   li->u.pLayerInfo = li->u.pLayerInfo->pNext;
   PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(g_instance, "vkCreateDevice");
   VkResult r = create(pd, ci, a, out);
   pthread_mutex_lock(&g_lock);
   const char *o = getenv("CEFPROF_OUT");
   if (o) {
      char path[512];
      snprintf(path, sizeof path, "%s.%d.device%u.txt", o, getpid(), g_dev_created);
      FILE *f = fopen(path, "w");
      if (f) {
         fprintf(f, "result=%d\nqueues:", r);
         for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++)
            fprintf(f, " family=%u count=%u", ci->pQueueCreateInfos[i].queueFamilyIndex, ci->pQueueCreateInfos[i].queueCount);
         fprintf(f, "\nextensions(%u):", ci->enabledExtensionCount);
         for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
            fprintf(f, " %s", ci->ppEnabledExtensionNames[i]);
         fprintf(f, "\nfeatures: ");
         int first = 1;
         if (ci->pEnabledFeatures)
            dump_core(f, ci->pEnabledFeatures, &first);
         dump_features(f, ci->pNext, &first);
         fprintf(f, "\n");
         fclose(f);
      }
   }
   if (!r) {
      /* a destroyed device's slot may be reused by the loader with the same key */
      void *key = *(void **)*out;
      unsigned i = 0;
      while (i < g_ndev && g_devs[i].key != key)
         i++;
      if (i < MAXDEV) {
         struct dev *d = &g_devs[i];
         d->key = key;
         d->device = *out;
         d->gdpa = gdpa;
         for (int c = 0; c < C_COUNT; c++) {
            char nm[96];
            snprintf(nm, sizeof nm, "vk%s", cmd_names[c]);
            d->next[c] = gdpa(*out, nm);
         }
         if (i == g_ndev)
            g_ndev++;
      }
      g_dev_created++;
   }
   pthread_mutex_unlock(&g_lock);
   return r;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetDeviceProcAddr(VkDevice device, const char *name)
{
   if (!strcmp(name, "vkGetDeviceProcAddr"))
      return (PFN_vkVoidFunction)layer_GetDeviceProcAddr;
   struct dev *d = dev_of(device);
   PFN_vkVoidFunction n = d->gdpa(device, name);
   if (!n)
      return NULL;
   for (int c = 0; c < C_COUNT; c++)
      if (!strcmp(wrappers[c].name, name))
         return wrappers[c].fn;
   return n;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetInstanceProcAddr(VkInstance instance, const char *name)
{
   if (!strcmp(name, "vkGetInstanceProcAddr"))
      return (PFN_vkVoidFunction)layer_GetInstanceProcAddr;
   if (!strcmp(name, "vkCreateInstance"))
      return (PFN_vkVoidFunction)layer_CreateInstance;
   if (!strcmp(name, "vkCreateDevice"))
      return (PFN_vkVoidFunction)layer_CreateDevice;
   if (!strcmp(name, "vkGetDeviceProcAddr"))
      return (PFN_vkVoidFunction)layer_GetDeviceProcAddr;
   PFN_vkVoidFunction n = instance && g_next_gipa ? g_next_gipa(instance, name) : NULL;
   if (n)
      for (int c = 0; c < C_COUNT; c++)
         if (!strcmp(wrappers[c].name, name))
            return wrappers[c].fn;
   return n;
}

__attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
   v->loaderLayerInterfaceVersion = 2;
   v->pfnGetInstanceProcAddr = layer_GetInstanceProcAddr;
   v->pfnGetDeviceProcAddr = layer_GetDeviceProcAddr;
   v->pfnGetPhysicalDeviceProcAddr = NULL;
   return VK_SUCCESS;
}
