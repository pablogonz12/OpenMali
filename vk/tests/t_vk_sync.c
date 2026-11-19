/* SPDX-License-Identifier: MIT */
/* Fence / sync_file edge cases through the ICD (loader-free, fake backend): the paths of mali_sync.c the happy-path tests never reach.
 *   MALI_FAKE_SLOW_US=300000 t_vk_sync --direct <icd.so>
 * 1. a submit whose fence retires late (slow fake GPU): a short wait is VK_TIMEOUT and GetFenceStatus VK_NOT_READY, a long wait VK_SUCCESS;
 * 2. a fence that was never submitted: zero-timeout and short waits are VK_TIMEOUT, not a hang;
 * 3. vkResetFences returns a signalled fence to unsignalled (wait times out again);
 * 4. vkImportFenceFdKHR of a sync_file that is not readable yet: the fence stays pending until the fd fires; importing -1 signals it;
 * 5. vkGetFenceFdKHR on a submitted fence returns a pollable fd that fires when the GPU finishes. */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <signal.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c, ...)                                                                              \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c);                                       \
         printf(__VA_ARGS__);                                                                      \
         printf("\n");                                                                             \
         g_fail = 1;                                                                               \
      }                                                                                            \
   } while (0)

#define DEV_FNS(X)                                                                                 \
   X(DestroyDevice) X(GetDeviceQueue) X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) \
   X(GetFenceStatus) X(QueueSubmit) X(ImportFenceFdKHR) X(GetFenceFdKHR)

#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)

static VkDevice dev;
static VkQueue q;

static VkFence
mkfence(int exportable, int signalled)
{
   VkExportFenceCreateInfo ex = {.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO, .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
   VkFenceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = exportable ? &ex : NULL,
                           .flags = signalled ? VK_FENCE_CREATE_SIGNALED_BIT : 0};
   VkFence f = VK_NULL_HANDLE;
   VkResult r = CreateFence(dev, &ci, NULL, &f);
   CHECK(r == VK_SUCCESS, "create fence -> %d", r);
   return f;
}

static void
submit(VkFence f)
{
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
   VkResult r = QueueSubmit(q, 1, &si, f);
   CHECK(r == VK_SUCCESS, "submit -> %d", r);
}

static VkResult
wait_ms(VkFence f, uint64_t ms)
{
   return WaitForFences(dev, 1, &f, VK_TRUE, ms * 1000000ull);
}

static void
on_alarm(int sig)
{
   static const char m[] = "FAIL: t_vk_sync watchdog expired (a fence wait never returned)\n";
   (void)sig;
   if (write(2, m, sizeof m - 1)) {}
   _exit(1);
}

int
main(int argc, char **argv)
{
   signal(SIGALRM, on_alarm);
   alarm(60);   /* a broken timeout check must fail the test, not hang it */
   const char *icd = NULL;
   for (int i = 1; i < argc; i++)
      if (!strcmp(argv[i], "--direct") && i + 1 < argc)
         icd = argv[++i];
   if (!icd) { fprintf(stderr, "usage: t_vk_sync --direct icd.so\n"); return 2; }

   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   VkResult (*negotiate)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   uint32_t iv = 5;
   if (!gipa || !negotiate || negotiate(&iv)) { printf("icd entry\n"); return 1; }

   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst;
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("CreateInstance\n"); return 1; }
   PFN_vkEnumeratePhysicalDevices EPD = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   PFN_vkCreateDevice CreateDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr GDPA = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   PFN_vkDestroyInstance DestroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
   VkPhysicalDevice pd;
   uint32_t cnt = 1;
   VkResult r = EPD(inst, &cnt, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || cnt < 1) { printf("no device\n"); return 1; }

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .synchronization2 = VK_TRUE};
   VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13, .hostQueryReset = VK_TRUE};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f12, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
                          .enabledExtensionCount = 2, .ppEnabledExtensionNames = (const char *[]){"VK_KHR_external_fence", "VK_KHR_external_fence_fd"}};
   if (CreateDevice(pd, &di, NULL, &dev) != VK_SUCCESS) { printf("CreateDevice\n"); return 1; }
#define LOAD(n) n = (PFN_vk##n)GDPA(dev, "vk" #n); if (!n) { printf("missing vk" #n "\n"); return 1; }
#define LOAD(n) n = (PFN_vk##n)GDPA(dev, "vk" #n); if (!n) { printf("missing vk" #n "\n"); return 1; }
   DEV_FNS(LOAD)
   GetDeviceQueue(dev, 0, 0, &q);
   const int slow = getenv("MALI_FAKE_SLOW_US") != NULL;

   /* 1. late-retiring submit */
   if (slow) {
      VkFence f = mkfence(0, 0);
      submit(f);
      VkResult r = wait_ms(f, 1);
      CHECK(r == VK_TIMEOUT, "short wait on slow fence -> %d (want VK_TIMEOUT)", r);
      r = GetFenceStatus(dev, f);
      CHECK(r == VK_NOT_READY, "status on slow fence -> %d (want VK_NOT_READY)", r);
      r = wait_ms(f, 10000);
      CHECK(r == VK_SUCCESS, "long wait on slow fence -> %d", r);
      CHECK(GetFenceStatus(dev, f) == VK_SUCCESS, "status after wait");
      /* 3. reset */
      CHECK(ResetFences(dev, 1, &f) == VK_SUCCESS, "reset");
      CHECK(GetFenceStatus(dev, f) == VK_NOT_READY, "status after reset");
      CHECK(wait_ms(f, 0) == VK_TIMEOUT, "zero-timeout wait after reset");
      DestroyFence(dev, f, NULL);
   }

   /* 2. never submitted */
   {
      VkFence f = mkfence(0, 0);
      CHECK(wait_ms(f, 0) == VK_TIMEOUT, "zero wait on unsubmitted fence");
      CHECK(wait_ms(f, 20) == VK_TIMEOUT, "20 ms wait on unsubmitted fence");
      DestroyFence(dev, f, NULL);
      f = mkfence(0, 1);
      CHECK(wait_ms(f, 0) == VK_SUCCESS, "created-signalled fence");
      CHECK(ResetFences(dev, 1, &f) == VK_SUCCESS, "reset signalled");
      CHECK(wait_ms(f, 0) == VK_TIMEOUT, "wait after reset of created-signalled");
      DestroyFence(dev, f, NULL);
   }

   /* 4. import */
   {
      VkFence f = mkfence(1, 0);
      int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
      VkImportFenceFdInfoKHR ii = {.sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR, .fence = f, .flags = VK_FENCE_IMPORT_TEMPORARY_BIT,
                                   .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT, .fd = dup(efd)};   /* Vulkan takes ownership of the fd on import: hand over a dup and keep efd to fire it */
      VkResult r = ImportFenceFdKHR(dev, &ii);
      CHECK(r == VK_SUCCESS, "import eventfd -> %d", r);
      CHECK(wait_ms(f, 1) == VK_TIMEOUT, "wait on unreadable imported fd");
      uint64_t one = 1;
      CHECK(write(efd, &one, sizeof(one)) == sizeof(one), "fire eventfd");
      CHECK(wait_ms(f, 5000) == VK_SUCCESS, "wait after the imported fd fired");
      close(efd);
      DestroyFence(dev, f, NULL);

      f = mkfence(1, 0);
      ii.fence = f;
      ii.fd = -1;
      r = ImportFenceFdKHR(dev, &ii);
      CHECK(r == VK_SUCCESS, "import -1 -> %d", r);
      CHECK(wait_ms(f, 0) == VK_SUCCESS, "fence imported from -1 must be signalled");
      DestroyFence(dev, f, NULL);
   }

   /* 5. export */
   if (slow) {
      VkFence f = mkfence(1, 0);
      submit(f);
      int fd = -2;
      VkFenceGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, .fence = f, .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
      VkResult r = GetFenceFdKHR(dev, &gi, &fd);
      CHECK(r == VK_SUCCESS, "export -> %d", r);
      CHECK(fd >= 0, "export of a pending fence must give a real fd, got %d", fd);
      if (fd >= 0) {
         struct pollfd pf = {fd, POLLIN, 0};
         CHECK(poll(&pf, 1, 0) == 0, "exported fd readable before the GPU finished");
         CHECK(poll(&pf, 1, 10000) == 1, "exported fd never fired");
         close(fd);
      }
      DestroyFence(dev, f, NULL);
   }

   DestroyDevice(dev, NULL);
   DestroyInstance(inst, NULL);
   if (g_fail) { printf("t_vk_sync: FAILED\n"); return 1; }
   printf("t_vk_sync: ok\n");
   return 0;
}
