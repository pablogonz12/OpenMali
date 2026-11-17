/* SPDX-License-Identifier: MIT */
/* Resource-leak regression test (dEQP aborted with VK_ERROR_OUT_OF_DEVICE_MEMORY after ~70 cases).
 * Loader-free: dlopens the ICD. Phases, each measured as growth of (open fds, /proc/self/maps lines, VmRSS KiB)
 * between the second half-way point and the end (the first iterations warm caches):
 *   dev    create+destroy VkDevice N times (with one queue submit each)
 *   mem    allocate+map+free VkDeviceMemory N times (mixed sizes)
 *   submit N frames: record fill-buffer cmd buffer (fresh each time, freed/ reset), submit, fence wait
 *   pool   create+destroy command pool/buffers/buffer+pipeline objects N times
 * Fails when a phase leaks more than --max-maps maps lines / fds, or any call returns an error.
 *   t_vk_leak --direct <icd.so> [--n N] [--only dev|mem|submit|pool|multi|second] [--max-maps K]
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
static const char *g_only;
static int g_n = 200, g_maxmaps = 8;

#define CASE_ON(n) (!g_only || !strcmp(g_only, n))
#define CHECKV(c, r) do { VkResult _r = (r); if (_r != VK_SUCCESS) { printf("{\"fail\":\"%s:%d\",\"vk\":%d,\"what\":\"%s\"}\n", __FILE__, __LINE__, (int)_r, #c); g_fail = 1; return 0; } } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice phys;

struct fns {
   PFN_vkGetDeviceProcAddr gdpa;
#define F(n) PFN_vk##n n;
#include "t_vk_leak_fns.h"
#undef F
};

static struct fns f;
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memprops;

struct meas { long fds, maps, rss; };

static long
count_dir(const char *p)
{
   DIR *d = opendir(p);
   long n = 0;
   if (!d)
      return -1;
   while (readdir(d))
      n++;
   closedir(d);
   return n;
}

static struct meas
measure(void)
{
   struct meas m = {.fds = count_dir("/proc/self/fd")};
   FILE *fp = fopen("/proc/self/maps", "r");
   char line[512];
   while (fp && fgets(line, sizeof line, fp))
      m.maps++;
   if (fp)
      fclose(fp);
   fp = fopen("/proc/self/status", "r");
   while (fp && fgets(line, sizeof line, fp))
      if (!strncmp(line, "VmRSS:", 6))
         m.rss = atol(line + 6);
   if (fp)
      fclose(fp);
   return m;
}

static int
memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
         return (int)i;
   return -1;
}

static int
load_dev(void)
{
   VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1};
   float pr = 1;
   q.pQueuePriorities = &pr;
   VkDeviceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q};
   PFN_vkCreateDevice cd = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   CHECKV(CreateDevice, cd(phys, &ci, NULL, &dev));
   f.gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
#define F(n) f.n = (PFN_vk##n)f.gdpa(dev, "vk" #n);
#include "t_vk_leak_fns.h"
#undef F
   f.GetDeviceQueue(dev, 0, 0, &queue);
   return 1;
}

/* one fill-buffer submit with a fresh pool + cmd buffer + fence */
static int
one_submit(VkBuffer buf, uint32_t val)
{
   VkCommandPool pool;
   VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   CHECKV(CreateCommandPool, f.CreateCommandPool(dev, &pi, NULL, &pool));
   VkCommandBuffer cb;
   VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                     .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   CHECKV(AllocateCommandBuffers, f.AllocateCommandBuffers(dev, &ai, &cb));
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   CHECKV(Begin, f.BeginCommandBuffer(cb, &bi));
   f.CmdFillBuffer(cb, buf, 0, 4096, val);
   CHECKV(End, f.EndCommandBuffer(cb));
   VkFence fence;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECKV(CreateFence, f.CreateFence(dev, &fi, NULL, &fence));
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   CHECKV(QueueSubmit, f.QueueSubmit(queue, 1, &si, fence));
   CHECKV(WaitForFences, f.WaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000000000));
   f.DestroyFence(dev, fence, NULL);
   f.DestroyCommandPool(dev, pool, NULL);
   return 1;
}

static int
make_buf(VkBuffer *b, VkDeviceMemory *m)
{
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
   CHECKV(CreateBuffer, f.CreateBuffer(dev, &bci, NULL, b));
   VkMemoryRequirements mr;
   f.GetBufferMemoryRequirements(dev, *b, &mr);
   int t = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)t};
   CHECKV(AllocateMemory, f.AllocateMemory(dev, &mai, NULL, m));
   CHECKV(Bind, f.BindBufferMemory(dev, *b, *m, 0));
   return 1;
}

static void
report(const char *name, int ok, struct meas a, struct meas b)
{
   long dm = b.maps - a.maps, df = b.fds - a.fds, dr = b.rss - a.rss;
   int bad = !ok || dm > g_maxmaps || df > 2;
   printf("{\"case\":\"%s\",\"n\":%d,\"d_maps\":%ld,\"d_fds\":%ld,\"d_rss_kib\":%ld,\"pass\":%s}\n", name, g_n, dm, df, dr, bad ? "false" : "true");
   if (bad)
      g_fail = 1;
}

static int
phase_dev(void)
{
   struct meas a = {0};
   for (int i = 0; i < g_n; i++) {
      if (i == g_n / 2)
         a = measure();
      if (!load_dev())
         return 0;
      VkBuffer b; VkDeviceMemory m;
      if (!make_buf(&b, &m) || !one_submit(b, i))
         return 0;
      f.DestroyBuffer(dev, b, NULL);
      f.FreeMemory(dev, m, NULL);
      f.DestroyDevice(dev, NULL);
   }
   report("dev", 1, a, measure());
   return 1;
}

static int
phase_mem(void)
{
   if (!load_dev())
      return 0;
   struct meas a = {0};
   for (int i = 0; i < g_n; i++) {
      if (i == g_n / 2)
         a = measure();
      VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = (size_t)4096 << (i % 12), .memoryTypeIndex = 0};
      VkMemoryRequirements dummy;
      (void)dummy;
      VkDeviceMemory m;
      for (uint32_t t = 0; t < memprops.memoryTypeCount; t++) {
         mai.memoryTypeIndex = t;
         CHECKV(AllocateMemory, f.AllocateMemory(dev, &mai, NULL, &m));
         void *p = NULL;
         if (memprops.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            CHECKV(MapMemory, f.MapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, &p));
            memset(p, 0x5a, 4096);
            f.UnmapMemory(dev, m);
         }
         f.FreeMemory(dev, m, NULL);
      }
   }
   report("mem", 1, a, measure());
   f.DestroyDevice(dev, NULL);
   return 1;
}

static int
phase_submit(void)
{
   if (!load_dev())
      return 0;
   VkBuffer b; VkDeviceMemory m;
   if (!make_buf(&b, &m))
      return 0;
   struct meas a = {0};
   for (int i = 0; i < g_n; i++) {
      if (i == g_n / 2)
         a = measure();
      if (!one_submit(b, i))
         return 0;
   }
   report("submit", 1, a, measure());
   f.DestroyBuffer(dev, b, NULL);
   f.FreeMemory(dev, m, NULL);
   f.DestroyDevice(dev, NULL);
   return 1;
}

static int
phase_pool(void)
{
   if (!load_dev())
      return 0;
   struct meas a = {0};
   for (int i = 0; i < g_n; i++) {
      if (i == g_n / 2)
         a = measure();
      VkBuffer b; VkDeviceMemory m;
      if (!make_buf(&b, &m))
         return 0;
      VkCommandPool pool;
      VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      CHECKV(CreateCommandPool, f.CreateCommandPool(dev, &pi, NULL, &pool));
      VkCommandBuffer cb[4];
      VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 4};
      CHECKV(AllocateCommandBuffers, f.AllocateCommandBuffers(dev, &ai, cb));
      VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      for (int k = 0; k < 4; k++) {
         CHECKV(Begin, f.BeginCommandBuffer(cb[k], &bi));
         f.CmdFillBuffer(cb[k], b, 0, 4096, k);
         CHECKV(End, f.EndCommandBuffer(cb[k]));
      }
      CHECKV(ResetCommandPool, f.ResetCommandPool(dev, pool, 0));
      f.FreeCommandBuffers(dev, pool, 2, cb);
      f.DestroyCommandPool(dev, pool, NULL);
      f.DestroyBuffer(dev, b, NULL);
      f.FreeMemory(dev, m, NULL);
   }
   report("pool", 1, a, measure());
   f.DestroyDevice(dev, NULL);
   return 1;
}

/* Many devices alive at once (dEQP keeps a context device plus per-case ones), then destroy all and create again. */
static int
phase_multi(void)
{
   enum { MAXD = 64 };
   VkDevice devs[MAXD];
   int n = 0;
   VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1};
   float pr = 1;
   q.pQueuePriorities = &pr;
   VkDeviceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q};
   PFN_vkCreateDevice cd = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   VkResult r = VK_SUCCESS;
   while (n < MAXD && (r = cd(phys, &ci, NULL, &devs[n])) == VK_SUCCESS)
      n++;
   int first = n;
   PFN_vkDestroyDevice dd = (PFN_vkDestroyDevice)gipa(inst, "vkDestroyDevice");
   for (int i = 0; i < n; i++)
      dd(devs[i], NULL);
   n = 0;
   while (n < MAXD && (r = cd(phys, &ci, NULL, &devs[n])) == VK_SUCCESS)
      n++;
   for (int i = 0; i < n; i++)
      dd(devs[i], NULL);
   /* the pass criterion: at least 8 live devices, and the same count again after destroying all */
   int bad = first < 8 || n != first;
   printf("{\"case\":\"multi\",\"first\":%d,\"second\":%d,\"pass\":%s}\n", first, n, bad ? "false" : "true");
   if (bad)
      g_fail = 1;
   return 1;
}

/* dEQP pattern: the main device stays alive while per-case devices come and go; the survivor must keep working
 * (vkCreateEvent and meta ops used process-global bindings that the destroy of ANY device cleared). */
static int
phase_second(void)
{
   if (!load_dev())
      return 0;
   VkDevice main_dev = dev;
   struct fns mf = f;
   VkQueue mq = queue;
   VkBuffer b; VkDeviceMemory m;
   if (!make_buf(&b, &m))
      return 0;
   int ok = 1;
   for (int i = 0; i < 20 && ok; i++) {
      if (!load_dev())
         return 0;
      f.DestroyDevice(dev, NULL);   /* the extra device dies */
      dev = main_dev; f = mf; queue = mq;
      VkEvent ev;
      VkEventCreateInfo ei = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
      VkResult r = f.CreateEvent(dev, &ei, NULL, &ev);
      if (r != VK_SUCCESS) {
         printf("{\"fail\":\"second\",\"iter\":%d,\"CreateEvent\":%d}\n", i, (int)r);
         ok = 0;
         break;
      }
      f.DestroyEvent(dev, ev, NULL);
      if (!one_submit(b, i)) {
         ok = 0;
      }
   }
   printf("{\"case\":\"second\",\"pass\":%s}\n", ok ? "true" : "false");
   if (!ok)
      g_fail = 1;
   f.DestroyBuffer(dev, b, NULL);
   f.FreeMemory(dev, m, NULL);
   f.DestroyDevice(dev, NULL);
   return 1;
}

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--n") && i + 1 < argc) g_n = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--only") && i + 1 < argc) g_only = argv[++i];
      else if (!strcmp(argv[i], "--max-maps") && i + 1 < argc) g_maxmaps = atoi(argv[++i]);
   }
   if (!icd) {
      fprintf(stderr, "usage: t_vk_leak --direct icd.so [--n N] [--only dev|mem|submit|pool|multi|second]\n");
      return 2;
   }
   void *h = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
   if (!gipa) gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
   PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   if (ci(&ici, NULL, &inst) != VK_SUCCESS) { printf("{\"fail\":\"CreateInstance\"}\n"); return 1; }
   PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   uint32_t n = 1;
   if (en(inst, &n, &phys) < 0 || n < 1) { printf("{\"fail\":\"no device\"}\n"); return 1; }
   ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties"))(phys, &memprops);

   if (CASE_ON("dev")) phase_dev();
   if (CASE_ON("mem")) phase_mem();
   if (CASE_ON("submit")) phase_submit();
   if (CASE_ON("pool")) phase_pool();
   if (CASE_ON("multi")) phase_multi();
   if (CASE_ON("second")) phase_second();
   printf("{\"summary\":\"%s\"}\n", g_fail ? "FAIL" : "ok");
   return g_fail;
}
