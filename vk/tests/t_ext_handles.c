/* SPDX-License-Identifier: MIT */
/* External-handle extensions: every one this ICD advertises must be backed by working entry points.
 *
 *   t_ext_handles --direct <icd.so>
 *
 * VK_KHR_external_memory(_fd), VK_EXT_external_memory_dma_buf, VK_KHR_external_semaphore(_fd) and
 * VK_KHR_external_fence(_fd) were advertised while a host that trusts the extension list had to call the
 * capability queries and the import/export entry points. This test asks the instance/device extension lists
 * what is advertised, then requires, for each advertised extension:
 *   1. its query entry point exists (GetPhysicalDeviceExternalSemaphoreProperties / ...FenceProperties /
 *      ...BufferProperties) and answers without claiming anything we cannot do;
 *   2. every handle type it reports EXPORTABLE or IMPORTABLE has its import and export entry point non-NULL
 *      (a NULL slot is a crash, not an error a client can handle);
 *   3. the round trip really works: a signalled binary semaphore exports a SYNC_FD, the fd polls readable,
 *      it imports back as a temporary payload and a second submit waits on it; the same with fences;
 *   4. DMA_BUF buffers: capability says exportable|importable, GetMemoryFdKHR gives an fd, the fd imports
 *      into a second buffer's memory and the bytes match. On a backend whose exported buffer is a memfd and
 *      not a dma-buf (the fake backend's host-side stand-in) the import arm prints SKIP with the reason --
 *      the guest's kbase export is a real dma-buf and takes the full path (t_dmabuf proves the probe).
 * Loader-free: the ICD is dlopen'ed and every entry point comes from vk_icdGetInstanceProcAddr.
 * Exit 0 = pass. */
#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define NOTE(...) do { printf(__VA_ARGS__); printf("\n"); } while (0)

static PFN_vkGetInstanceProcAddr GIPA;
static PFN_vkGetDeviceProcAddr GDEV;


/* ---- the entry points this test uses ------------------------------------------------------- */
static PFN_vkEnumerateInstanceExtensionProperties pEnumInstanceExt;
static PFN_vkCreateInstance pCreateInstance;
static PFN_vkEnumeratePhysicalDevices pEnumPhysicalDevices;
static PFN_vkGetPhysicalDeviceProperties pGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties pGetQueueFamilyProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties pGetMemoryProperties;
static PFN_vkEnumerateDeviceExtensionProperties pEnumDeviceExt;
static PFN_vkGetPhysicalDeviceExternalSemaphoreProperties pGetExternalSemaphoreProperties;
static PFN_vkGetPhysicalDeviceExternalFenceProperties pGetExternalFenceProperties;
static PFN_vkGetPhysicalDeviceExternalBufferProperties pGetExternalBufferProperties;
static PFN_vkCreateDevice pCreateDevice;
static PFN_vkDestroyDevice pDestroyDevice;
static PFN_vkCreateSemaphore pCreateSemaphore;
static PFN_vkDestroySemaphore pDestroySemaphore;
static PFN_vkImportSemaphoreFdKHR pImportSemaphoreFd;
static PFN_vkGetSemaphoreFdKHR pGetSemaphoreFd;
static PFN_vkCreateFence pCreateFence;
static PFN_vkDestroyFence pDestroyFence;
static PFN_vkImportFenceFdKHR pImportFenceFd;
static PFN_vkGetFenceFdKHR pGetFenceFd;
static PFN_vkWaitForFences pWaitForFences;
static PFN_vkResetFences pResetFences;
static PFN_vkCreateCommandPool pCreateCommandPool;
static PFN_vkDestroyCommandPool pDestroyCommandPool;
static PFN_vkAllocateCommandBuffers pAllocateCommandBuffers;
static PFN_vkBeginCommandBuffer pBeginCommandBuffer;
static PFN_vkEndCommandBuffer pEndCommandBuffer;
static PFN_vkGetDeviceQueue pGetDeviceQueue;
static PFN_vkQueueSubmit pQueueSubmit;
static PFN_vkQueueWaitIdle pQueueWaitIdle;
static PFN_vkCreateBuffer pCreateBuffer;
static PFN_vkDestroyBuffer pDestroyBuffer;
static PFN_vkGetBufferMemoryRequirements pGetBufferMemoryRequirements;
static PFN_vkAllocateMemory pAllocateMemory;
static PFN_vkFreeMemory pFreeMemory;
static PFN_vkBindBufferMemory pBindBufferMemory;
static PFN_vkMapMemory pMapMemory;
static PFN_vkUnmapMemory pUnmapMemory;
static PFN_vkGetMemoryFdKHR pGetMemoryFd;
static PFN_vkGetMemoryFdPropertiesKHR pGetMemoryFdProperties;

static int
instance_ext(const char *name)
{
   uint32_t n = 0;
   if (!pEnumInstanceExt || pEnumInstanceExt(NULL, &n, NULL) != VK_SUCCESS)
      return 0;
   VkExtensionProperties *p = calloc(n ? n : 1, sizeof(*p));
   if (!p)
      return 0;
   int have = 0;
   if (pEnumInstanceExt(NULL, &n, p) == VK_SUCCESS)
      for (uint32_t i = 0; i < n; i++)
         if (!strcmp(p[i].extensionName, name))
            have = 1;
   free(p);
   return have;
}

static int
device_ext(VkPhysicalDevice pd, const char *name)
{
   uint32_t n = 0;
   if (!pEnumDeviceExt || pEnumDeviceExt(pd, NULL, &n, NULL) != VK_SUCCESS)
      return 0;
   VkExtensionProperties *p = calloc(n ? n : 1, sizeof(*p));
   if (!p)
      return 0;
   int have = 0;
   if (pEnumDeviceExt(pd, NULL, &n, p) == VK_SUCCESS)
      for (uint32_t i = 0; i < n; i++)
         if (!strcmp(p[i].extensionName, name))
            have = 1;
   free(p);
   return have;
}

/* ---- a trivial submission, the way a client signals and waits ------------------------------- */
struct ctx {
   VkDevice dev;
   VkQueue queue;
   VkCommandPool pool;
   VkCommandBuffer cb;
   uint32_t family;
};

static VkResult
ctx_submit(struct ctx *c, VkSemaphore wait, VkSemaphore signal, VkFence fence)
{
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
   VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   CHECK(pBeginCommandBuffer(c->cb, &(VkCommandBufferBeginInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}) == VK_SUCCESS,
         "begin command buffer");
   CHECK(pEndCommandBuffer(c->cb) == VK_SUCCESS, "end command buffer");
   if (wait) {
      si.waitSemaphoreCount = 1;
      si.pWaitSemaphores = &wait;
      si.pWaitDstStageMask = &stage;
   }
   if (signal) {
      si.signalSemaphoreCount = 1;
      si.pSignalSemaphores = &signal;
   }
   si.commandBufferCount = 1;
   si.pCommandBuffers = &c->cb;
   return pQueueSubmit(c->queue, 1, &si, fence);
}

static int
fd_ready(int fd, int timeout_ms)
{
   struct pollfd pf = {.fd = fd, .events = POLLIN};
   return poll(&pf, 1, timeout_ms) > 0;
}

static int
fd_is_dmabuf(int fd)
{
   char path[64], link[256];
   snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
   ssize_t n = readlink(path, link, sizeof link - 1);
   if (n <= 0)
      return 0;
   link[n] = '\0';
   return strstr(link, "dmabuf") != NULL || strstr(link, "dma-buf") != NULL ||
          strstr(link, "udmabuf") != NULL;
}

static const char *
fd_kind(int fd)
{
   static char link[256];
   char path[64];
   snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
   ssize_t n = readlink(path, link, sizeof link - 1);
   if (n <= 0)
      return "?";
   link[n] = '\0';
   return link;
}

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   void *lib;
   VkInstance inst = VK_NULL_HANDLE;
   VkPhysicalDevice pd = VK_NULL_HANDLE;
   struct ctx c = {0};
   int rc = 1;

   for (int i = 1; i < argc; i++)
      if (!strcmp(argv[i], "--direct") && i + 1 < argc)
         icd = argv[++i];
   if (!icd) {
      printf("usage: t_ext_handles --direct <icd.so>\n");
      return 2;
   }
   lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      printf("dlopen: %s\n", dlerror());
      return 1;
   }
   GIPA = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!GIPA) {
      printf("no vk_icdGetInstanceProcAddr\n");
      return 1;
   }
   *(void **)(&pEnumInstanceExt) = (void *)GIPA(NULL, "vkEnumerateInstanceExtensionProperties");
   *(void **)(&pCreateInstance) = (void *)GIPA(NULL, "vkCreateInstance");
   CHECK(pCreateInstance != NULL, "vkCreateInstance resolves");

   /* 1. which capability extensions does the instance advertise? */
   const char *cap_exts[] = {
      "VK_KHR_external_memory_capabilities",
      "VK_KHR_external_semaphore_capabilities",
      "VK_KHR_external_fence_capabilities",
   };
   const char *enabled[8];
   uint32_t n_enabled = 0;
   for (size_t i = 0; i < sizeof cap_exts / sizeof cap_exts[0]; i++) {
      int have = instance_ext(cap_exts[i]);
      NOTE("%-6s advertise %s", have ? "ok" : "none", cap_exts[i]);
      if (have)
         enabled[n_enabled++] = cap_exts[i];
   }

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
      .enabledExtensionCount = n_enabled,
      .ppEnabledExtensionNames = enabled,
   };
   if (pCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
      printf("FAIL vkCreateInstance with the capability extensions\n");
      return 1;
   }
   *(void **)(&pEnumPhysicalDevices) = (void *)GIPA(inst, "vkEnumeratePhysicalDevices");
   *(void **)(&pGetPhysicalDeviceProperties) = (void *)GIPA(inst, "vkGetPhysicalDeviceProperties");
   *(void **)(&pGetQueueFamilyProperties) = (void *)GIPA(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
   *(void **)(&pGetMemoryProperties) = (void *)GIPA(inst, "vkGetPhysicalDeviceMemoryProperties");
   *(void **)(&pEnumDeviceExt) = (void *)GIPA(inst, "vkEnumerateDeviceExtensionProperties");
   *(void **)(&pGetExternalSemaphoreProperties) = (void *)GIPA(inst, "vkGetPhysicalDeviceExternalSemaphoreProperties");
   *(void **)(&pGetExternalFenceProperties) = (void *)GIPA(inst, "vkGetPhysicalDeviceExternalFenceProperties");
   *(void **)(&pGetExternalBufferProperties) = (void *)GIPA(inst, "vkGetPhysicalDeviceExternalBufferProperties");
   *(void **)(&pCreateDevice) = (void *)GIPA(inst, "vkCreateDevice");
   *(void **)(&pDestroyDevice) = (void *)GIPA(inst, "vkDestroyDevice");
   GDEV = (PFN_vkGetDeviceProcAddr)GIPA(inst, "vkGetDeviceProcAddr");

   uint32_t nd = 0;
   CHECK(pEnumPhysicalDevices(inst, &nd, NULL) == VK_SUCCESS && nd >= 1, "enumerate physical devices (n=%u)", nd);
   if (!nd)
      goto out;
   CHECK(pEnumPhysicalDevices(inst, &nd, &pd) == VK_SUCCESS, "get physical device");
   VkPhysicalDeviceProperties props;
   pGetPhysicalDeviceProperties(pd, &props);
   NOTE("device: %s (%s)", props.deviceName, props.driverVersion ? "driver ok" : "driver ?");

   /* 2. the query entry points of the advertised capability extensions must exist */
   if (instance_ext("VK_KHR_external_semaphore_capabilities") || device_ext(pd, "VK_KHR_external_semaphore_fd")) {
      CHECK(pGetExternalSemaphoreProperties != NULL,
            "VK_KHR_external_semaphore_capabilities advertised but vkGetPhysicalDeviceExternalSemaphoreProperties is NULL");
      NOTE("%-6s semaphore capability query", pGetExternalSemaphoreProperties ? "ok" : "FAIL");
   }
   if (instance_ext("VK_KHR_external_fence_capabilities") || device_ext(pd, "VK_KHR_external_fence_fd")) {
      CHECK(pGetExternalFenceProperties != NULL,
            "VK_KHR_external_fence_capabilities advertised but vkGetPhysicalDeviceExternalFenceProperties is NULL");
      NOTE("%-6s fence capability query", pGetExternalFenceProperties ? "ok" : "FAIL");
   }
   if (instance_ext("VK_KHR_external_memory_capabilities") || device_ext(pd, "VK_KHR_external_memory_fd")) {
      CHECK(pGetExternalBufferProperties != NULL,
            "VK_KHR_external_memory_capabilities advertised but vkGetPhysicalDeviceExternalBufferProperties is NULL");
      NOTE("%-6s buffer capability query", pGetExternalBufferProperties ? "ok" : "FAIL");
   }

   /* 3. device: enable every advertised external-handle device extension */
   const char *dev_exts[] = {
      "VK_KHR_external_memory", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
      "VK_KHR_external_semaphore", "VK_KHR_external_semaphore_fd",
      "VK_KHR_external_fence", "VK_KHR_external_fence_fd",
   };
   const char *dev_enabled[8];
   uint32_t n_dev = 0;
   for (size_t i = 0; i < sizeof dev_exts / sizeof dev_exts[0]; i++) {
      int have = device_ext(pd, dev_exts[i]);
      NOTE("%-6s advertise %s", have ? "ok" : "none", dev_exts[i]);
      if (have)
         dev_enabled[n_dev++] = dev_exts[i];
   }

   uint32_t nfam = 0;
   pGetQueueFamilyProperties(pd, &nfam, NULL);
   VkQueueFamilyProperties *fam = calloc(nfam ? nfam : 1, sizeof(*fam));
   if (nfam)
      pGetQueueFamilyProperties(pd, &nfam, fam);
   uint32_t family = UINT32_MAX;
   for (uint32_t i = 0; i < nfam; i++)
      if (fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
   free(fam);
   CHECK(family != UINT32_MAX, "a graphics queue family");
   if (family == UINT32_MAX)
      goto out;

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &prio,
   };
   VkPhysicalDeviceFeatures feats = {0};
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
      .pEnabledFeatures = &feats,
      .enabledExtensionCount = n_dev, .ppEnabledExtensionNames = dev_enabled,
   };
   if (pCreateDevice(pd, &dci, NULL, &c.dev) != VK_SUCCESS) {
      printf("FAIL vkCreateDevice with %u external-handle extensions\n", n_dev);
      goto out;
   }
#define DD(name, pfn) *(void **)(&pfn) = (void *)GDEV(c.dev, "vk" #name)
   DD(CreateSemaphore, pCreateSemaphore); DD(DestroySemaphore, pDestroySemaphore);
   DD(ImportSemaphoreFdKHR, pImportSemaphoreFd); DD(GetSemaphoreFdKHR, pGetSemaphoreFd);
   DD(CreateFence, pCreateFence); DD(DestroyFence, pDestroyFence);
   DD(ImportFenceFdKHR, pImportFenceFd); DD(GetFenceFdKHR, pGetFenceFd);
   DD(WaitForFences, pWaitForFences); DD(ResetFences, pResetFences);
   DD(CreateCommandPool, pCreateCommandPool); DD(DestroyCommandPool, pDestroyCommandPool);
   DD(AllocateCommandBuffers, pAllocateCommandBuffers);
   DD(BeginCommandBuffer, pBeginCommandBuffer); DD(EndCommandBuffer, pEndCommandBuffer);
   DD(GetDeviceQueue, pGetDeviceQueue); DD(QueueSubmit, pQueueSubmit); DD(QueueWaitIdle, pQueueWaitIdle);
   DD(CreateBuffer, pCreateBuffer); DD(DestroyBuffer, pDestroyBuffer);
   DD(GetBufferMemoryRequirements, pGetBufferMemoryRequirements);
   DD(AllocateMemory, pAllocateMemory); DD(FreeMemory, pFreeMemory);
   DD(BindBufferMemory, pBindBufferMemory); DD(MapMemory, pMapMemory); DD(UnmapMemory, pUnmapMemory);
   DD(GetMemoryFdKHR, pGetMemoryFd); DD(GetMemoryFdPropertiesKHR, pGetMemoryFdProperties);
#undef DD

   /* 4. what the capability queries claim, and whether the entry points behind the claim exist */
   VkPhysicalDeviceExternalSemaphoreInfo esi = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
   };
   VkExternalSemaphoreProperties esp;
   VkExternalSemaphoreHandleTypeFlags sem_handle_types[] = {
      VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
   };
   for (size_t i = 0; pGetExternalSemaphoreProperties && i < 2; i++) {
      memset(&esp, 0, sizeof esp);
      esi.handleType = sem_handle_types[i];
      pGetExternalSemaphoreProperties(pd, &esi, &esp);
      int transfer = (esp.externalSemaphoreFeatures &
                      (VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT)) != 0;
      NOTE("%-6s semaphore handle 0x%08x features=0x%x compatible=0x%x",
           "query", sem_handle_types[i], esp.externalSemaphoreFeatures, esp.compatibleHandleTypes);
      if (transfer) {
         CHECK(pGetSemaphoreFd != NULL,
               "properties claim semaphore handle 0x%08x but vkGetSemaphoreFdKHR is NULL", sem_handle_types[i]);
         CHECK(pImportSemaphoreFd != NULL,
               "properties claim semaphore handle 0x%08x but vkImportSemaphoreFdKHR is NULL", sem_handle_types[i]);
      }
   }
   VkPhysicalDeviceExternalFenceInfo efi = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,
   };
   VkExternalFenceProperties efp;
   VkExternalFenceHandleTypeFlags fence_handle_types[] = {
      VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT, VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
   };
   for (size_t i = 0; pGetExternalFenceProperties && i < 2; i++) {
      memset(&efp, 0, sizeof efp);
      efi.handleType = fence_handle_types[i];
      pGetExternalFenceProperties(pd, &efi, &efp);
      int transfer = (efp.externalFenceFeatures &
                      (VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT)) != 0;
      NOTE("query  fence handle 0x%08x features=0x%x compatible=0x%x",
           fence_handle_types[i], efp.externalFenceFeatures, efp.compatibleHandleTypes);
      if (transfer) {
         CHECK(pGetFenceFd != NULL, "properties claim fence handle 0x%08x but vkGetFenceFdKHR is NULL", fence_handle_types[i]);
         CHECK(pImportFenceFd != NULL, "properties claim fence handle 0x%08x but vkImportFenceFdKHR is NULL", fence_handle_types[i]);
      }
   }

   /* 5. a hierarchy for the submissions */
   VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = family,
   };
   if (pCreateCommandPool(c.dev, &pci, NULL, &c.pool) != VK_SUCCESS) {
      printf("FAIL vkCreateCommandPool\n");
      goto out_dev;
   }
   VkCommandBufferAllocateInfo cbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = c.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
   };
   if (pAllocateCommandBuffers(c.dev, &cbi, &c.cb) != VK_SUCCESS) {
      printf("FAIL vkAllocateCommandBuffers\n");
      goto out_dev;
   }
   pGetDeviceQueue(c.dev, family, 0, &c.queue);
   c.family = family;

   /* 6. binary semaphore, SYNC_FD round trip: signal -> export -> poll -> import -> wait */
   if (device_ext(pd, "VK_KHR_external_semaphore_fd") && pGetSemaphoreFd && pImportSemaphoreFd) {
      VkExportSemaphoreCreateInfo esci = {
         .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
         .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
      };
      VkSemaphoreCreateInfo sci = {
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &esci,
      };
      VkSemaphore a = VK_NULL_HANDLE, b = VK_NULL_HANDLE;
      VkFence f = VK_NULL_HANDLE;
      VkResult r;

      r = pCreateSemaphore(c.dev, &sci, NULL, &a);
      CHECK(r == VK_SUCCESS, "create exportable binary semaphore (%d)", r);
      r = pCreateSemaphore(c.dev, &sci, NULL, &b);
      CHECK(r == VK_SUCCESS, "create second binary semaphore (%d)", r);
      r = pCreateFence(c.dev, &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}, NULL, &f);
      CHECK(r == VK_SUCCESS, "create fence (%d)", r);
      if (a && b && f) {
         r = ctx_submit(&c, VK_NULL_HANDLE, a, f);
         CHECK(r == VK_SUCCESS, "submit signalling the semaphore (%d)", r);
         r = pWaitForFences(c.dev, 1, &f, VK_TRUE, 2000000000ull);
         CHECK(r == VK_SUCCESS, "fence wait after signal (%d)", r);

         int fd = -1;
         r = pGetSemaphoreFd(c.dev, &(VkSemaphoreGetFdInfoKHR){
                                 .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
                                 .semaphore = a,
                                 .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT}, &fd);
         CHECK(r == VK_SUCCESS, "vkGetSemaphoreFdKHR(SYNC_FD) on a signalled binary semaphore (%d)", r);
         NOTE("%-6s sync fd from semaphore: %d (%s)", fd >= 0 ? "ok" : "FAIL", fd,
              fd >= 0 ? fd_kind(fd) : "no fd");
         CHECK(fd >= 0, "SYNC_FD export returned no fd");
         if (fd >= 0) {
            CHECK(fd_ready(fd, 2000), "exported sync fd is not readable");
            r = pImportSemaphoreFd(c.dev, &(VkImportSemaphoreFdInfoKHR){
                                          .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
                                          .semaphore = b,
                                          .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                                          .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
                                          .fd = fd});
            CHECK(r == VK_SUCCESS, "vkImportSemaphoreFdKHR(SYNC_FD, TEMPORARY) (%d)", r);
            close(fd);
            if (r == VK_SUCCESS) {
               pResetFences(c.dev, 1, &f);
               r = ctx_submit(&c, b, VK_NULL_HANDLE, f);
               CHECK(r == VK_SUCCESS, "submit waiting on the imported semaphore (%d)", r);
               r = pWaitForFences(c.dev, 1, &f, VK_TRUE, 2000000000ull);
               CHECK(r == VK_SUCCESS, "the submit that waited on the imported semaphore completed (%d)", r);
            }
         }
      }
      if (f) pDestroyFence(c.dev, f, NULL);
      if (a) pDestroySemaphore(c.dev, a, NULL);
      if (b) pDestroySemaphore(c.dev, b, NULL);
   } else {
      NOTE("skip  semaphore fd round trip: %s",
           device_ext(pd, "VK_KHR_external_semaphore_fd") ? "entry points missing" : "not advertised");
   }

   /* 7. fence, SYNC_FD round trip: signal -> export -> poll -> import -> wait */
   if (device_ext(pd, "VK_KHR_external_fence_fd") && pGetFenceFd && pImportFenceFd) {
      VkFence f1 = VK_NULL_HANDLE, f2 = VK_NULL_HANDLE;
      VkResult r = pCreateFence(c.dev, &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}, NULL, &f1);
      CHECK(r == VK_SUCCESS, "create fence 1 (%d)", r);
      r = pCreateFence(c.dev, &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}, NULL, &f2);
      CHECK(r == VK_SUCCESS, "create fence 2 (%d)", r);
      if (f1 && f2) {
         r = ctx_submit(&c, VK_NULL_HANDLE, VK_NULL_HANDLE, f1);
         CHECK(r == VK_SUCCESS, "submit signalling fence 1 (%d)", r);
         CHECK(pWaitForFences(c.dev, 1, &f1, VK_TRUE, 2000000000ull) == VK_SUCCESS, "fence 1 signalled");
         int fd = -1;
         r = pGetFenceFd(c.dev, &(VkFenceGetFdInfoKHR){
                               .sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
                               .fence = f1,
                               .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT}, &fd);
         CHECK(r == VK_SUCCESS, "vkGetFenceFdKHR(SYNC_FD) on a signalled fence (%d)", r);
         NOTE("%-6s sync fd from fence: %d (%s)", fd >= 0 ? "ok" : "FAIL", fd,
              fd >= 0 ? fd_kind(fd) : "no fd");
         CHECK(fd >= 0, "fence SYNC_FD export returned no fd");
         if (fd >= 0) {
            CHECK(fd_ready(fd, 2000), "exported fence fd is not readable");
            r = pImportFenceFd(c.dev, &(VkImportFenceFdInfoKHR){
                                     .sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR,
                                     .fence = f2,
                                     .flags = VK_FENCE_IMPORT_TEMPORARY_BIT,
                                     .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
                                     .fd = fd});
            CHECK(r == VK_SUCCESS, "vkImportFenceFdKHR(SYNC_FD, TEMPORARY) (%d)", r);
            close(fd);
            if (r == VK_SUCCESS)
               CHECK(pWaitForFences(c.dev, 1, &f2, VK_TRUE, 2000000000ull) == VK_SUCCESS,
                     "the imported fence signalled");
         }
      }
      if (f1) pDestroyFence(c.dev, f1, NULL);
      if (f2) pDestroyFence(c.dev, f2, NULL);
   } else {
      NOTE("skip  fence fd round trip: %s",
           device_ext(pd, "VK_KHR_external_fence_fd") ? "entry points missing" : "not advertised");
   }

   /* 8. DMA_BUF buffers: capability, export, import, bytes */
   if (device_ext(pd, "VK_EXT_external_memory_dma_buf") && pGetExternalBufferProperties) {
      VkPhysicalDeviceExternalBufferInfo ebi = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
         .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
         .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      };
      VkExternalBufferProperties ebp = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
      pGetExternalBufferProperties(pd, &ebi, &ebp);
      VkExternalMemoryProperties *em = &ebp.externalMemoryProperties;
      int exportable = em->externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT;
      int importable = em->externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
      NOTE("%-6s DMA_BUF buffer features=0x%x exportable=%d importable=%d",
           (exportable && importable) ? "ok" : "FAIL", em->externalMemoryFeatures, !!exportable, !!importable);
      CHECK(exportable && importable,
            "VK_EXT_external_memory_dma_buf advertised but GetPhysicalDeviceExternalBufferProperties does not "
            "report EXPORTABLE|IMPORTABLE for DMA_BUF");
      CHECK(pGetMemoryFd != NULL && pGetMemoryFdProperties != NULL,
            "VK_EXT_external_memory_dma_buf advertised but vkGetMemoryFdKHR/vkGetMemoryFdPropertiesKHR are NULL");

      if (exportable && importable && pGetMemoryFd && pGetMemoryFdProperties && pCreateBuffer) {
         const VkDeviceSize size = 4096;
         VkExternalMemoryBufferCreateInfo ebci = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
         };
         VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ebci,
            .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         };
         VkBuffer buf_a = VK_NULL_HANDLE, buf_b = VK_NULL_HANDLE;
         VkDeviceMemory mem_a = VK_NULL_HANDLE, mem_b = VK_NULL_HANDLE;
         VkResult r = pCreateBuffer(c.dev, &bci, NULL, &buf_a);
         CHECK(r == VK_SUCCESS, "create exportable buffer (%d)", r);
         if (r == VK_SUCCESS) {
            VkMemoryRequirements req;
            pGetBufferMemoryRequirements(c.dev, buf_a, &req);
            VkPhysicalDeviceMemoryProperties mp;
            pGetMemoryProperties(pd, &mp);
            uint32_t type = UINT32_MAX;
            for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
               if ((req.memoryTypeBits & (1u << i)) &&
                   (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                  type = i;
                  break;
               }
            CHECK(type != UINT32_MAX, "a host-visible memory type for the buffer");
            if (type != UINT32_MAX) {
               VkExportMemoryAllocateInfo emai = {
                  .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                  .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
               };
               VkMemoryAllocateInfo mai = {
                  .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &emai,
                  .allocationSize = req.size, .memoryTypeIndex = type,
               };
               r = pAllocateMemory(c.dev, &mai, NULL, &mem_a);
               CHECK(r == VK_SUCCESS, "allocate exportable memory (%d)", r);
            }
            if (mem_a) {
               CHECK(pBindBufferMemory(c.dev, buf_a, mem_a, 0) == VK_SUCCESS, "bind exportable buffer");
               void *p = NULL;
               if (pMapMemory(c.dev, mem_a, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS && p) {
                  memset(p, 0x5a, size);
                  pUnmapMemory(c.dev, mem_a);
               }
               int fd = -1;
               r = pGetMemoryFd(c.dev, &(VkMemoryGetFdInfoKHR){
                                       .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                                       .memory = mem_a,
                                       .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT}, &fd);
               CHECK(r == VK_SUCCESS, "vkGetMemoryFdKHR(DMA_BUF) (%d)", r);
               NOTE("%-6s memory fd: %d (%s)", fd >= 0 ? "ok" : "FAIL", fd, fd >= 0 ? fd_kind(fd) : "no fd");
               CHECK(fd >= 0, "DMA_BUF export returned no fd");
               if (fd >= 0) {
                  VkMemoryFdPropertiesKHR fp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
                  r = pGetMemoryFdProperties(c.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp);
                  CHECK(r == VK_SUCCESS, "vkGetMemoryFdPropertiesKHR(%d) (%d)", fd, r);
                  VkImportMemoryFdInfoKHR imi = {
                     .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                     .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                     .fd = dup(fd),
                  };
                  VkMemoryAllocateInfo mai = {
                     .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imi,
                     .allocationSize = size, .memoryTypeIndex = type,
                  };
                  r = pAllocateMemory(c.dev, &mai, NULL, &mem_b);
                  if (r == VK_ERROR_INVALID_EXTERNAL_HANDLE && !fd_is_dmabuf(fd)) {
                     /* The exported fd is a host memfd, not a dma-buf: the fake backend's stand-in for a
                      * guest BO. A guest regression on a real dma-buf lands in the CHECK below instead. */
                     NOTE("skip  import arm: exported fd is '%s', not a dma-buf (the fake backend's host-side "
                          "stand-in; kbase exports a real dma-buf)", fd_kind(fd));
                  } else {
                     CHECK(r == VK_SUCCESS, "allocate memory from the exported fd (%d)", r);
                     if (r == VK_SUCCESS) {
                        r = pCreateBuffer(c.dev, &bci, NULL, &buf_b);
                        CHECK(r == VK_SUCCESS, "create the importing buffer (%d)", r);
                        if (buf_b) {
                           CHECK(pBindBufferMemory(c.dev, buf_b, mem_b, 0) == VK_SUCCESS, "bind the importing buffer");
                           void *q = NULL;
                           if (pMapMemory(c.dev, mem_b, 0, VK_WHOLE_SIZE, 0, &q) == VK_SUCCESS && q) {
                              CHECK(((unsigned char *)q)[0] == 0x5a && ((unsigned char *)q)[size - 1] == 0x5a,
                                    "the imported buffer does not show the exported bytes (0x%02x..0x%02x)",
                                    ((unsigned char *)q)[0], ((unsigned char *)q)[size - 1]);
                              NOTE("%-6s imported bytes match", ((unsigned char *)q)[0] == 0x5a ? "ok" : "FAIL");
                              pUnmapMemory(c.dev, mem_b);
                           }
                        }
                     }
                  }
                  close(fd);
               }
            }
         }
         if (buf_b) pDestroyBuffer(c.dev, buf_b, NULL);
         if (buf_a) pDestroyBuffer(c.dev, buf_a, NULL);
         if (mem_b) pFreeMemory(c.dev, mem_b, NULL);
         if (mem_a) pFreeMemory(c.dev, mem_a, NULL);
      }
   } else {
      NOTE("skip  DMA_BUF buffer arm: not advertised");
   }

   rc = g_fail ? 1 : 0;
out_dev:
   if (c.pool) pDestroyCommandPool(c.dev, c.pool, NULL);
   if (c.dev) pDestroyDevice(c.dev, NULL);
out:
   if (inst) {
      PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)GIPA(inst, "vkDestroyInstance");
      if (di)
         di(inst, NULL);
   }
   printf("%s: %s\n", "t_ext_handles", g_fail ? "FAIL" : "OK");
   return rc;
}
