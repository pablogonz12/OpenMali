/* SPDX-License-Identifier: MIT */
/* M15 test: frame profiling plumbing (mali_fprof.c) on the fake backend. Runs frames and present-shaped submits
 * through the ICD with MALI_FRAME_PROF pointing at a private file, then checks the lines: one per measured
 * submission, every field present and in range, gpu_ms >= 0 and <= frame_ms, seq strictly increasing,
 * present_ms > 0 once two presents have gone through, and the summary line.
 *
 *   t_frameprof --direct <icd.so> [--frames N] [--off]
 * One JSON line per case; exit 0 = all pass. The fields themselves are estimates (see vk/src/mali_fprof.h): this
 * test covers the plumbing, not the accuracy. --off leaves MALI_FRAME_PROF unset and runs the same frames: the
 * unset path must produce no output at all (nothing on stderr, no file).
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;

#define CHECK(c, ...)                                                                              \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("{\"fail\":\"%s:%d\",\"expr\":\"%s\",\"detail\":\"", __FILE__, __LINE__, #c);     \
         printf(__VA_ARGS__);                                                                      \
         printf("\"}\n");                                                                          \
         g_fail = 1;                                                                               \
         return EXIT_FAILURE;                                                                      \
      }                                                                                            \
   } while (0)

static VkDevice dev;
static VkQueue queue;
static PFN_vkDestroyDevice DestroyDevice;
static PFN_vkCreateCommandPool CreateCommandPool;
static PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
static PFN_vkBeginCommandBuffer BeginCommandBuffer;
static PFN_vkEndCommandBuffer EndCommandBuffer;
static PFN_vkQueueSubmit QueueSubmit;
static PFN_vkQueueWaitIdle QueueWaitIdle;
static PFN_vkCreateSemaphore CreateSemaphore;
static PFN_vkDestroySemaphore DestroySemaphore;
static PFN_vkGetFenceStatus GetFenceStatus;
static PFN_vkCreateBuffer CreateBuffer;
static PFN_vkDestroyBuffer DestroyBuffer;
static PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
static PFN_vkBindBufferMemory BindBufferMemory;
static PFN_vkAllocateMemory AllocateMemory;
static PFN_vkFreeMemory FreeMemory;
static PFN_vkCmdFillBuffer CmdFillBuffer;

/* ------------------------------------------------------------------ line parsing */
static char *
read_all(const char *path, size_t *len)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   char *buf = calloc(1, (size_t)n + 1);
   if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
      free(buf);
      buf = NULL;
   }
   fclose(f);
   if (len)
      *len = (size_t)n;
   return buf;
}

static int
field_d(const char *line, const char *key, double *out)
{
   char pat[64];
   snprintf(pat, sizeof pat, "%s=", key);
   const char *p = strstr(line, pat);
   if (!p)
      return 0;
   char *end;
   double v = strtod(p + strlen(pat), &end);
   if (end == p + strlen(pat))
      return 0;
   *out = v;
   return 1;
}

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   int frames = 24, off = 0;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--off")) off = 1;
      else { fprintf(stderr, "usage: t_frameprof --direct icd.so [--frames N]\n"); return 2; }
   }
   if (!icd) { fprintf(stderr, "--direct required\n"); return 2; }

   /* The driver reads this in mali_fprof_init (device creation), so it must be set before vkCreateDevice. */
   char path[256];
   snprintf(path, sizeof path, "/tmp/t_frameprof.%d.log", (int)getpid());
   unlink(path);
   if (off) {
      unsetenv("MALI_FRAME_PROF");
      /* stderr to a file of its own: "no output when unset" is part of the contract. */
      char errp[256];
      snprintf(errp, sizeof errp, "/tmp/t_frameprof.%d.err", (int)getpid());
      FILE *ef = freopen(errp, "w", stderr);
      (void)ef;
   } else
      setenv("MALI_FRAME_PROF", path, 1);

   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("{\"fail\":\"dlopen: %s\"}\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   VkResult (*negotiate)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   uint32_t iv = 5;
   if (!gipa || !negotiate || negotiate(&iv)) { printf("{\"fail\":\"icd entry\"}\n"); return 1; }

   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst;
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("{\"fail\":\"CreateInstance\"}\n"); return 1; }
   PFN_vkEnumeratePhysicalDevices EPD = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   PFN_vkCreateDevice CreateDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   PFN_vkGetPhysicalDeviceMemoryProperties GMP = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
   PFN_vkGetDeviceProcAddr GDPA = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   VkPhysicalDevice pd;
   uint32_t cnt = 1;
   VkResult r = EPD(inst, &cnt, &pd);
   CHECK((r == VK_SUCCESS || r == VK_INCOMPLETE) && cnt >= 1, "no device");
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
   CHECK(CreateDevice(pd, &di, NULL, &dev) == VK_SUCCESS, "CreateDevice");
#define LOAD(n) n = (PFN_vk##n)GDPA(dev, "vk" #n); if (!n) { printf("{\"fail\":\"missing vk" #n "\"}\n"); return 1; }
   LOAD(DestroyDevice) LOAD(CreateCommandPool) LOAD(AllocateCommandBuffers) LOAD(BeginCommandBuffer)
   LOAD(EndCommandBuffer) LOAD(QueueSubmit) LOAD(QueueWaitIdle) LOAD(CreateSemaphore) LOAD(DestroySemaphore)
   LOAD(GetFenceStatus) LOAD(CreateBuffer) LOAD(DestroyBuffer) LOAD(GetBufferMemoryRequirements)
   LOAD(BindBufferMemory) LOAD(AllocateMemory) LOAD(FreeMemory) LOAD(CmdFillBuffer)
   PFN_vkGetDeviceQueue GetDeviceQueue = (PFN_vkGetDeviceQueue)GDPA(dev, "vkGetDeviceQueue");
   CHECK(GetDeviceQueue && (GetDeviceQueue(dev, 0, 0, &queue), queue != VK_NULL_HANDLE), "queue");

   /* A command buffer with one meta fill: an empty one produces no frame at all (mali_cmd_submit submits
    * nothing when nall == 0), and the profiler measures submissions. The fake backend executes nothing. */
   VkPhysicalDeviceMemoryProperties mp;
   GMP(pd, &mp);
   VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   VkCommandPool pool;
   CHECK(CreateCommandPool(dev, &cpi, NULL, &pool) == VK_SUCCESS, "pool");
   VkCommandBufferAllocateInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VkCommandBuffer cb;
   CHECK(AllocateCommandBuffers(dev, &cbi, &cb) == VK_SUCCESS, "cb");
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   VkBuffer buf;
   CHECK(CreateBuffer(dev, &bci, NULL, &buf) == VK_SUCCESS, "buffer");
   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(dev, buf, &mr);
   uint32_t mt = ~0u;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
         mt = i;
         break;
      }
   CHECK(mt != ~0u, "no host-visible memory type");
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
   VkDeviceMemory mem;
   CHECK(AllocateMemory(dev, &mai, NULL, &mem) == VK_SUCCESS, "memory");
   CHECK(BindBufferMemory(dev, buf, mem, 0) == VK_SUCCESS, "bind");
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   CHECK(BeginCommandBuffer(cb, &bi) == VK_SUCCESS, "begin");
   CmdFillBuffer(cb, buf, 0, 4096, 0x5a5a5a5au);
   CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "end");

   /* Frames: submit, let up to 3 be in flight (the driver's retire list) so both estimator paths (GPU idle at the
    * kick and GPU still saturated) are exercised on the fake backend. */
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   /* Present-shaped submits: no command buffers, one wait and one signal, exactly what vk_common's
    * QueuePresentKHR submits through driver_submit (a fence-only submit has no waits and is not counted). One after
    * every frame, as an app presents, so the next frame's line carries the interval. */
   VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkSemaphore s[2];
   CHECK(CreateSemaphore(dev, &sci, NULL, &s[0]) == VK_SUCCESS && CreateSemaphore(dev, &sci, NULL, &s[1]) == VK_SUCCESS, "sem");
   VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   VkSubmitInfo seed = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = 1, .pSignalSemaphores = &s[0]};
   CHECK(QueueSubmit(queue, 1, &seed, VK_NULL_HANDLE) == VK_SUCCESS, "seed semaphore");
   for (int i = 0; i < frames; i++) {
      CHECK(QueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS, "submit %d", i);
      VkSemaphore wait = s[i & 1], signal = s[!(i & 1)];
      VkSubmitInfo pres = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
                           .pWaitDstStageMask = &st, .signalSemaphoreCount = 1, .pSignalSemaphores = &signal};
      CHECK(QueueSubmit(queue, 1, &pres, VK_NULL_HANDLE) == VK_SUCCESS, "present-shaped %d", i);
      if ((i & 3) == 3)
         CHECK(QueueWaitIdle(queue) == VK_SUCCESS, "wait idle %d", i);   /* the driver's retire list is 3 deep */
   }
   CHECK(QueueWaitIdle(queue) == VK_SUCCESS, "wait idle (end)");

   /* Teardown prints the summary line and flushes the file. */
   DestroyBuffer(dev, buf, NULL);
   FreeMemory(dev, mem, NULL);
   DestroySemaphore(dev, s[0], NULL);
   DestroySemaphore(dev, s[1], NULL);
   DestroyDevice(dev, NULL);

   if (off) {
      char errp[256];
      snprintf(errp, sizeof errp, "/tmp/t_frameprof.%d.err", (int)getpid());
      size_t elen = 0;
      char *err = read_all(errp, &elen);
      int leaked = err && strstr(err, "FRAMEPROF");
      free(err);
      unlink(errp);
      CHECK(access(path, F_OK) != 0, "MALI_FRAME_PROF unset still wrote %s", path);
      CHECK(!leaked, "MALI_FRAME_PROF unset still printed FRAMEPROF lines");
      printf("{\"test\":\"frameprof_off\",\"frames\":%d,\"output\":\"none\",\"ok\":true}\n", frames);
      dlclose(lib);
      return g_fail;
   }

   size_t len = 0;
   char *log = read_all(path, &len);
   CHECK(log && len > 0, "no profiling output at %s (is MALI_FRAME_PROF honoured?)", path);
   unsigned nframes = 0, nsummary = 0, nbad = 0;
   double prev_seq = 0, max_seq = 0, present_ms_max = 0;
   const char *p = log;
   while ((p = strstr(p, "FRAMEPROF ")) != NULL) {
      const char *eol = strchr(p, '\n');
      size_t n = eol ? (size_t)(eol - p) : strlen(p);
      char line[512];
      if (n >= sizeof line)
         n = sizeof line - 1;
      memcpy(line, p, n);
      line[n] = 0;
      p = eol ? eol : p + n;
      if (strstr(line, "FRAMEPROF summary ")) {
         nsummary++;
         double fr = 0;
         CHECK(field_d(line, "frames", &fr), "summary without frames: %s", line);
         CHECK((unsigned)fr == nframes, "summary frames %.0f but %u lines", fr, nframes);
         continue;
      }
      if (!strstr(line, "FRAMEPROF f=")) { /* gpu_clock=... diagnostic */
         nbad++;
         continue;
      }
      double seq = 0, submit_us = -1, gpu_ms = -1, present_ms = -1, frame_ms = -1;
      CHECK(field_d(line, "f", &seq) && field_d(line, "submit_us", &submit_us) && field_d(line, "gpu_ms", &gpu_ms) &&
            field_d(line, "present_ms", &present_ms) && field_d(line, "frame_ms", &frame_ms), "fields: %s", line);
      CHECK(strstr(line, "est=sat") || strstr(line, "est=idle"), "no est tag: %s", line);
      CHECK(strstr(line, "bound=gpu") || strstr(line, "bound=cpu"), "no bound tag: %s", line);
      CHECK(seq == prev_seq + 1, "seq %0.f after %0.f", seq, prev_seq);
      CHECK(submit_us >= 0, "submit_us %f", submit_us);
      CHECK(gpu_ms >= 0, "gpu_ms %f", gpu_ms);
      CHECK(frame_ms >= 0 && gpu_ms <= frame_ms + 1e-6, "gpu_ms %f > frame_ms %f", gpu_ms, frame_ms);
      CHECK(present_ms >= 0, "present_ms %f", present_ms);
      prev_seq = seq;
      max_seq = seq;
      if (present_ms > present_ms_max)
         present_ms_max = present_ms;
      nframes++;
   }
   free(log);
   unlink(path);

   CHECK(nbad == 0, "%u FRAMEPROF lines were diagnostics, not samples", nbad);
   CHECK(nframes >= (unsigned)frames, "only %u frame lines for %d frames", nframes, frames);
   CHECK(nsummary == 1, "%u summary lines", nsummary);
   CHECK(nframes == (unsigned)max_seq, "frames %u but last seq %.0f", nframes, max_seq);
   CHECK(present_ms_max > 0, "no present interval measured (present-shaped submits not detected)");

   printf("{\"test\":\"frameprof\",\"frames\":%u,\"present_ms_max\":%.3f,\"ok\":true}\n", nframes, present_ms_max);
   dlclose(lib);
   return g_fail;
}
