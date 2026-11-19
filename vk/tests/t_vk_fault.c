/* SPDX-License-Identifier: MIT */
/* Fault-injection sweep over a frame-like workload on the ICD + fake backend (MALI_FAKE_FAIL, see src/mali_backend_fake.c).
 *   t_vk_fault --direct <icd.so> [--spv-dir dir] [--op name[,name..]] [--max N] [--one op:N]
 * For every op and every N = 1, 2, ... a child process (fresh dlopen of the ICD, so a crash, hang or leak is attributed to one injection
 * point) runs the workload with the N-th call of that backend op failing, until N is beyond the workload (no "mali-fake INJECT" line).
 * The child fails when
 *   - a Vulkan call returns something other than the allowed results for the injection (SUCCESS / OUT_OF_HOST_MEMORY / OUT_OF_DEVICE_MEMORY /
 *     INITIALIZATION_FAILED / DEVICE_LOST, plus TIMEOUT/NOT_READY where the call allows them),
 *   - after VK_ERROR_DEVICE_LOST any later vkQueueSubmit / vkWaitForFences / vkQueueWaitIdle / vkDeviceWaitIdle succeeds in a way that makes
 *     the loss disappear (a lost device must keep reporting loss),
 *   - an injected GPU fault (fault, wait, stuck, cs=EIO) is never reported as VK_ERROR_DEVICE_LOST,
 *   - it crashes, hangs (alarm watchdog) or, under ASan/LSan, leaks.
 * Plus a "blocked" workload: a thread sits in vkWaitForFences(UINT64_MAX) on a fence that never signals ("stuck"); the fault must wake it with
 * DEVICE_LOST and later submits must fail with DEVICE_LOST. Run it under ASan/UBSan: LD_PRELOAD=libasan.so ./t_vk_fault ... */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define FNS \
   F(DestroyDevice) F(GetDeviceQueue) F(AllocateMemory) F(FreeMemory) F(MapMemory) F(UnmapMemory) \
   F(CreateFence) F(DestroyFence) F(WaitForFences) F(ResetFences) F(QueueSubmit) F(QueueWaitIdle) F(DeviceWaitIdle) \
   F(CreateSemaphore) F(DestroySemaphore) \
   F(CreateBuffer) F(DestroyBuffer) F(GetBufferMemoryRequirements) F(BindBufferMemory) \
   F(CreateImage) F(DestroyImage) F(GetImageMemoryRequirements) F(BindImageMemory) \
   F(CreateCommandPool) F(DestroyCommandPool) F(AllocateCommandBuffers) \
   F(BeginCommandBuffer) F(EndCommandBuffer) F(CmdFillBuffer) F(CmdCopyBuffer) F(CmdBindPipeline) F(CmdDispatch) F(CmdPushConstants) \
   F(CreateShaderModule) F(DestroyShaderModule) F(CreatePipelineLayout) F(DestroyPipelineLayout) \
   F(CreateComputePipelines) F(DestroyPipeline) F(CreateQueryPool) F(DestroyQueryPool) F(CmdResetQueryPool) F(CmdWriteTimestamp) F(GetQueryPoolResults)

static struct {
   PFN_vkGetDeviceProcAddr gdpa;
#define F(n) PFN_vk##n n;
   FNS
#undef F
} f;

static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memprops;
static uint32_t g_code[8192];
static size_t g_code_size;
static int g_expect_lost;       /* the injection is a GPU fault: some call must have said DEVICE_LOST */
static int g_saw_lost, g_viol;

static void
viol(const char *what, int line, VkResult r)
{
   printf("{\"violation\":\"%s\",\"line\":%d,\"vk\":%d}\n", what, line, (int)r);
   g_viol = 1;
}

/* Result contract: only these may come out of an injected fault; once lost, always lost. */
static VkResult
chk(VkResult r, const char *what, int line, int may_timeout)
{
   if (r == VK_ERROR_DEVICE_LOST) {
      g_saw_lost = 1;
      return r;
   }
   if (r == VK_SUCCESS || r == VK_ERROR_OUT_OF_HOST_MEMORY || r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_INITIALIZATION_FAILED ||
       r == VK_ERROR_TOO_MANY_OBJECTS || (may_timeout && (r == VK_TIMEOUT || r == VK_NOT_READY)))
      return r;
   viol(what, line, r);
   return r;
}
#define C(x) chk((x), #x, __LINE__, 0)
#define CT(x) chk((x), #x, __LINE__, 1)
#define STEP(x) do { if (C(x) != VK_SUCCESS) goto done; } while (0)

/* A lost device must stay lost for the calls that report it. */
static void
check_stays_lost(void)
{
   if (!g_saw_lost || !dev)
      return;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fe;
   if (f.CreateFence(dev, &fi, NULL, &fe) == VK_SUCCESS) {
      VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
      VkResult r = f.QueueSubmit(queue, 1, &si, fe);
      if (r != VK_ERROR_DEVICE_LOST)
         viol("QueueSubmit after device lost did not return DEVICE_LOST", __LINE__, r);
      r = f.QueueWaitIdle(queue);
      if (r != VK_ERROR_DEVICE_LOST)
         viol("QueueWaitIdle after device lost did not return DEVICE_LOST", __LINE__, r);
      r = f.DeviceWaitIdle(dev);
      if (r != VK_ERROR_DEVICE_LOST)
         viol("DeviceWaitIdle after device lost did not return DEVICE_LOST", __LINE__, r);
      f.DestroyFence(dev, fe, NULL);
   }
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
open_device(void *h)
{
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
   if (!gipa) gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
   PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   if (C(ci(&ici, NULL, &inst)) != VK_SUCCESS)
      return 0;
   uint32_t n = 1;
   VkResult r = ((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &phys);
   if (C(r) < 0 || n < 1)
      return 0;
   ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties"))(phys, &memprops);
   VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1};
   float pr = 1;
   q.pQueuePriorities = &pr;
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q};
   if (C(((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(phys, &dci, NULL, &dev)) != VK_SUCCESS) {
      dev = NULL;
      return 0;
   }
   f.gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
#define F(nm) f.nm = (PFN_vk##nm)f.gdpa(dev, "vk" #nm); if (!f.nm) { printf("{\"fail\":\"missing vk" #nm "\"}\n"); exit(1); }
   FNS
#undef F
   f.GetDeviceQueue(dev, 0, 0, &queue);
   return 1;
}

/* One "frame": resources, pipeline, record (fill, dispatch, copy, timestamp), submit with fence, wait, semaphore-chained present-style submit. */
static void
frame_workload(void)
{
   VkBuffer b0 = 0, b1 = 0;
   VkDeviceMemory m0 = 0;
   VkImage im = 0;
   VkDeviceMemory mi = 0;
   VkShaderModule sm = 0;
   VkPipelineLayout pl = 0;
   VkPipeline pipe = 0;
   VkCommandPool pool = 0;
   VkFence fe = 0;
   VkSemaphore sem = 0;
   VkQueryPool qp = 0;
   void *map = NULL;
   int mapped = 0;

   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 65536, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
   STEP(f.CreateBuffer(dev, &bci, NULL, &b0));
   STEP(f.CreateBuffer(dev, &bci, NULL, &b1));
   VkMemoryRequirements mr;
   f.GetBufferMemoryRequirements(dev, b0, &mr);
   int mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (mt < 0)
      mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size * 2, .memoryTypeIndex = (uint32_t)mt};
   STEP(f.AllocateMemory(dev, &mai, NULL, &m0));
   STEP(f.BindBufferMemory(dev, b0, m0, 0));
   STEP(f.BindBufferMemory(dev, b1, m0, mr.size));
   STEP(f.MapMemory(dev, m0, 0, VK_WHOLE_SIZE, 0, &map));
   mapped = 1;
   memset(map, 0x5a, 4096);

   VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                           .extent = {256, 256, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                           .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   STEP(f.CreateImage(dev, &ii, NULL, &im));
   VkMemoryRequirements ir;
   f.GetImageMemoryRequirements(dev, im, &ir);
   int it = memtype(ir.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (it < 0)
      it = 0;
   VkMemoryAllocateInfo iai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = ir.size, .memoryTypeIndex = (uint32_t)it};
   STEP(f.AllocateMemory(dev, &iai, NULL, &mi));
   STEP(f.BindImageMemory(dev, im, mi, 0));

   VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = g_code_size, .pCode = g_code};
   STEP(f.CreateShaderModule(dev, &smi, NULL, &sm));
   VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   STEP(f.CreatePipelineLayout(dev, &pli, NULL, &pl));
   VkComputePipelineCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"}};
   STEP(f.CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &pipe));
   VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 4};
   STEP(f.CreateQueryPool(dev, &qi, NULL, &qp));

   VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   STEP(f.CreateCommandPool(dev, &cpi, NULL, &pool));
   VkCommandBuffer cb;
   VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   STEP(f.AllocateCommandBuffers(dev, &ai, &cb));
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   STEP(f.BeginCommandBuffer(cb, &bi));
   f.CmdResetQueryPool(cb, qp, 0, 4);
   f.CmdFillBuffer(cb, b0, 0, 4096, 0x11223344);
   f.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   uint32_t pc[4] = {1, 2, 3, 4};
   f.CmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
   f.CmdDispatch(cb, 4, 1, 1);
   VkBufferCopy bc = {0, 0, 4096};
   f.CmdCopyBuffer(cb, b0, b1, 1, &bc);
   f.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 0);
   STEP(f.EndCommandBuffer(cb));

   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   STEP(f.CreateFence(dev, &fci, NULL, &fe));
   VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   STEP(f.CreateSemaphore(dev, &sci, NULL, &sem));
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &sem};
   STEP(f.QueueSubmit(queue, 1, &si, fe));
   STEP(f.WaitForFences(dev, 1, &fe, VK_TRUE, 5000000000ull));
   uint64_t ts;
   CT(f.GetQueryPoolResults(dev, qp, 0, 1, sizeof ts, &ts, 8, VK_QUERY_RESULT_64_BIT));
   STEP(f.ResetFences(dev, 1, &fe));
   VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   VkSubmitInfo sp = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem, .pWaitDstStageMask = &ws};
   STEP(f.QueueSubmit(queue, 1, &sp, fe));
   STEP(f.QueueWaitIdle(queue));
   STEP(f.WaitForFences(dev, 1, &fe, VK_TRUE, 5000000000ull));
   STEP(f.DeviceWaitIdle(dev));
done:
   /* teardown must always work, also on a lost device */
   if (dev) {
      if (g_saw_lost)
         check_stays_lost();
      if (mapped)
         f.UnmapMemory(dev, m0);
      f.DestroyFence(dev, fe, NULL);
      f.DestroySemaphore(dev, sem, NULL);
      f.DestroyCommandPool(dev, pool, NULL);
      f.DestroyQueryPool(dev, qp, NULL);
      f.DestroyPipeline(dev, pipe, NULL);
      f.DestroyPipelineLayout(dev, pl, NULL);
      f.DestroyShaderModule(dev, sm, NULL);
      f.DestroyImage(dev, im, NULL);
      f.FreeMemory(dev, mi, NULL);
      f.DestroyBuffer(dev, b0, NULL);
      f.DestroyBuffer(dev, b1, NULL);
      f.FreeMemory(dev, m0, NULL);
   }
}

static void *
blocked_waiter(void *arg)
{
   VkFence fe = *(VkFence *)arg;
   VkResult r = f.WaitForFences(dev, 1, &fe, VK_TRUE, UINT64_MAX);
   if (r != VK_ERROR_DEVICE_LOST)
      viol("blocked vkWaitForFences(UINT64_MAX) was not woken with DEVICE_LOST", __LINE__, r);
   g_saw_lost = 1;
   return NULL;
}

/* A fence that never signals (stuck:1): the waiter blocks forever unless the fault wakes it. */
static void
blocked_workload(void)
{
   VkFence fe = 0;
   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if (C(f.CreateFence(dev, &fci, NULL, &fe)) != VK_SUCCESS)
      return;
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
   if (C(f.QueueSubmit(queue, 1, &si, fe)) == VK_SUCCESS) {
      pthread_t t;
      pthread_create(&t, NULL, blocked_waiter, &fe);
      pthread_join(t, NULL);
      check_stays_lost();
   }
   f.DestroyFence(dev, fe, NULL);
}

static int
child(const char *icd, const char *spv, const char *op, int blocked)
{
   alarm(30);   /* watchdog: SIGALRM kills a hang, the parent reports it */
   char path[600];
   snprintf(path, sizeof path, "%s/comp.comp.spv", spv);
   FILE *fp = fopen(path, "rb");
   if (!fp) { printf("{\"fail\":\"open %s\"}\n", path); return 1; }
   g_code_size = fread(g_code, 1, sizeof g_code, fp);
   fclose(fp);
   void *h = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
   g_expect_lost = op && (!strncmp(op, "fault", 5) || !strncmp(op, "wait", 4) || !strncmp(op, "stuck", 5));
   if (open_device(h)) {
      if (blocked)
         blocked_workload();
      else
         frame_workload();
      f.DestroyDevice(dev, NULL);
   }
   if (inst) {
      PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
      if (!gipa) gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
      ((PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance"))(inst, NULL);
   }
   if (g_saw_lost)
      printf("{\"lost_seen\":1}\n");
   fflush(stdout);
   if (g_viol)
      return 3;
   return 0;
}

static int g_fail;

/* Runs one injection in a child with stdout+stderr captured in a temp file. Returns 1 when the injection fired. */
static int
run_one(const char *icd, const char *spv, const char *spec, int blocked, int verbose)
{
   char tmpl[] = "/tmp/t_vk_fault.XXXXXX";
   int fd = mkstemp(tmpl);
   if (fd < 0) { perror("mkstemp"); exit(2); }
   fflush(NULL);
   pid_t p = fork();
   if (p == 0) {
      dup2(fd, 1);
      dup2(fd, 2);
      setenv("MALI_BACKEND", "fake", 1);
      setenv("MALI_FAKE_FAIL", spec, 1);
      char op[32];
      snprintf(op, sizeof op, "%s", spec);
      char *c = strchr(op, ':');
      if (c) *c = 0;
      exit(child(icd, spv, op, blocked));   /* exit(), not _exit(): LSan runs its leak check at exit */
   }
   int st = 0;
   waitpid(p, &st, 0);
   lseek(fd, 0, SEEK_SET);
   char buf[16384];
   ssize_t n = read(fd, buf, sizeof buf - 1);
   buf[n < 0 ? 0 : n] = 0;
   close(fd);
   unlink(tmpl);
   int fired = strstr(buf, "mali-fake INJECT") != NULL;
   int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
   const char *why = "";
   /* an injected GPU fault that the workload reached must surface as VK_ERROR_DEVICE_LOST somewhere */
   if (ok && fired && !strstr(buf, "lost_seen") && (!strncmp(spec, "fault", 5) || !strncmp(spec, "wait", 4) || !strncmp(spec, "stuck", 5))) {
      ok = 0;
      why = "GPU fault injected but no call returned VK_ERROR_DEVICE_LOST (fault swallowed)";
   }
   if (!ok) {
      g_fail++;
      printf("FAIL %s%s: ", blocked ? "blocked " : "", spec);
      if (WIFSIGNALED(st))
         printf("signal %d%s\n", WTERMSIG(st), WTERMSIG(st) == SIGALRM ? " (HANG: watchdog)" : "");
      else
         printf("exit %d %s\n", WEXITSTATUS(st), why);
      printf("%s\n", buf);
   } else if (verbose) {
      printf("ok   %s%s fired=%d\n", blocked ? "blocked " : "", spec, fired);
   }
   return fired;
}

static void
rm_supp(void)
{
   const char *sp = getenv("T_VK_FAULT_REEXEC");
   if (sp && *sp == '/')
      unlink(sp);
}

int
main(int argc, char **argv)
{
   atexit(rm_supp);
   /* Sanitizer options only take effect at runtime start-up, so set them and re-exec once. The suppression is for an upstream Mesa runtime
    * bug (vk_common_QueueWaitIdle, vk_queue.c): when vk_queue_signal_sync() fails (device lost / submit error) the sync it just created is
    * not destroyed. Not in our code; suppressed so the sweep can see our own leaks. Drop it when the vendored Mesa has the fix. */
   if (!getenv("T_VK_FAULT_REEXEC")) {
      char sp[] = "/tmp/t_vk_fault_lsan.XXXXXX";
      int sfd = mkstemp(sp);
      if (sfd >= 0) {
         static const char sup[] = "leak:vk_common_QueueWaitIdle\n";
         if (write(sfd, sup, sizeof sup - 1) < 0) {}
         close(sfd);
         char opt[128];
         snprintf(opt, sizeof opt, "suppressions=%s", sp);
         setenv("LSAN_OPTIONS", opt, 1);
         setenv("ASAN_OPTIONS", "fast_unwind_on_malloc=0", 0);   /* the suppression needs the full stack */
         setenv("T_VK_FAULT_REEXEC", sp, 1);
         execv("/proc/self/exe", argv);
         unlink(sp);
      }
   }
   const char *icd = NULL, *spv = "shader", *ops = "alloc,heap,cs,submit,fault,wait,stuck", *one = NULL;
   int max = 300, verbose = 0;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) spv = argv[++i];
      else if (!strcmp(argv[i], "--op") && i + 1 < argc) ops = argv[++i];
      else if (!strcmp(argv[i], "--max") && i + 1 < argc) max = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--one") && i + 1 < argc) one = argv[++i];
      else if (!strcmp(argv[i], "-v")) verbose = 1;
   }
   if (!icd) { fprintf(stderr, "usage: t_vk_fault --direct icd.so [--spv-dir d] [--op a,b] [--max N] [--one op:N] [-v]\n"); return 2; }
   if (one) {
      run_one(icd, spv, one, 0, 1);
      return g_fail ? 1 : 0;
   }
   char list[256];
   snprintf(list, sizeof list, "%s", ops);
   unsigned total = 0;
   for (char *op = strtok(list, ","); op; op = strtok(NULL, ",")) {
      int n = 1;
      for (; n <= max; n++) {
         char spec[64];
         snprintf(spec, sizeof spec, "%s:%d", op, n);
         if (!run_one(icd, spv, spec, 0, verbose))
            break;
         total++;
      }
      printf("op %-8s swept %d injection points\n", op, n - 1);
      if (n > max) { printf("FAIL %s: injection still firing at N=%d (runaway)\n", op, max); g_fail++; }
   }
   /* GPU fault wakes a blocked infinite wait */
   if (run_one(icd, spv, "stuck:1", 1, verbose) == 0) { printf("FAIL blocked: injection did not fire\n"); g_fail++; }
   printf("{\"fault_sweep\":{\"points\":%u},\"pass\":%s}\n", total, g_fail ? "false" : "true");
   return g_fail ? 1 : 0;
}
