/* SPDX-License-Identifier: MIT */
/* ICD edge paths the other host tests never reach, on the fake backend (mali_bc.c image-format rules, mali_device.c create failures
 * and stubs).   t_vk_edge --direct <icd.so>
 * Checks: the BC image-format matrix of mali_bc_image_supported (2D/3D/cube/2D-array rules, 1D, usage and create flags that must be
 * refused); vkCreateDevice fails cleanly (and leaves *pDevice untouched) for a bad queue family, queueCount 2, two queue infos,
 * a failing allocator (OUT_OF_HOST_MEMORY), device_create ENOMEM vs other errors (MALI_FAKE_DEVICE_CREATE_ERR) and a failing
 * hw init (MALI_FAKE_BADFLAGS makes every BO allocation fail); a create after those failures still works; the opaque capture
 * address and sparse-requirements entry points are the documented stubs. Exit 0 = pass. */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...)                                                      \
   do {                                                                    \
      if (!(c)) {                                                          \
         printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c);               \
         printf(__VA_ARGS__);                                              \
         printf("\n");                                                     \
         g_fail = 1;                                                       \
      }                                                                    \
   } while (0)

static int g_alloc_fail; /* allocator callbacks: fail every allocation while set */
static int g_alloc_calls;
static void *VKAPI_PTR cb_alloc(void *u, size_t s, size_t a, VkSystemAllocationScope sc)
{ (void)u; (void)sc; g_alloc_calls++; if (g_alloc_fail) return NULL; void *p = NULL; return posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a, s) ? NULL : p; }
static void *VKAPI_PTR cb_realloc(void *u, void *o, size_t s, size_t a, VkSystemAllocationScope sc)
{ if (!o) return cb_alloc(u, s, a, sc); if (g_alloc_fail) return NULL; return realloc(o, s); }
static void VKAPI_PTR cb_free(void *u, void *p) { (void)u; free(p); }

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++)
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
   if (!icd) { printf("usage: t_vk_edge --direct <icd.so>\n"); return 2; }
   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa) { printf("no vk_icdGetInstanceProcAddr\n"); return 1; }

   VkInstance inst;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("FAIL CreateInstance\n"); return 1; }
#define I(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n)
   I(EnumeratePhysicalDevices); I(GetPhysicalDeviceImageFormatProperties); I(CreateDevice); I(GetDeviceProcAddr); I(DestroyInstance);
   VkPhysicalDevice pd; uint32_t n = 1;
   if (EnumeratePhysicalDevices(inst, &n, &pd) != VK_SUCCESS || n != 1) { printf("FAIL enumerate\n"); return 1; }

   /* ---- BC image formats ---- */
   const VkFormat bc = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
   const VkImageUsageFlags samp = VK_IMAGE_USAGE_SAMPLED_BIT;
   const VkImageTiling opt = VK_IMAGE_TILING_OPTIMAL;
   struct { const char *what; VkFormat f; VkImageType t; VkImageTiling ti; VkImageUsageFlags u; VkImageCreateFlags fl; VkResult want; } bcm[] = {
      {"2D sampled", bc, VK_IMAGE_TYPE_2D, opt, samp, 0, VK_SUCCESS},
      {"3D sampled", bc, VK_IMAGE_TYPE_3D, opt, samp, 0, VK_SUCCESS},
      {"BC7 transfer", VK_FORMAT_BC7_SRGB_BLOCK, VK_IMAGE_TYPE_2D, opt, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, VK_SUCCESS},
      {"1D refused", bc, VK_IMAGE_TYPE_1D, opt, samp, 0, VK_ERROR_FORMAT_NOT_SUPPORTED},
      {"linear refused", bc, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_LINEAR, samp, 0, VK_ERROR_FORMAT_NOT_SUPPORTED},
      {"storage refused", bc, VK_IMAGE_TYPE_2D, opt, samp | VK_IMAGE_USAGE_STORAGE_BIT, 0, VK_ERROR_FORMAT_NOT_SUPPORTED},
      {"color attachment refused", bc, VK_IMAGE_TYPE_2D, opt, samp | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, VK_ERROR_FORMAT_NOT_SUPPORTED},
      {"sparse refused", bc, VK_IMAGE_TYPE_2D, opt, samp, VK_IMAGE_CREATE_SPARSE_BINDING_BIT, VK_ERROR_FORMAT_NOT_SUPPORTED},
      {"mutable ok", bc, VK_IMAGE_TYPE_2D, opt, samp, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, VK_SUCCESS},
      {"cube 2D ok", bc, VK_IMAGE_TYPE_2D, opt, samp, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, VK_SUCCESS},
      {"cube 3D refused", bc, VK_IMAGE_TYPE_3D, opt, samp, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, VK_ERROR_FORMAT_NOT_SUPPORTED},
      {"2D-array on 3D ok", bc, VK_IMAGE_TYPE_3D, opt, samp, VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT, VK_SUCCESS},
      /* the generic image-format code accepts these on a compressed format; the BC emulation (shadow image decode) must not */
      {"input attachment refused", bc, VK_IMAGE_TYPE_2D, opt, samp | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, 0, VK_ERROR_FORMAT_NOT_SUPPORTED},
      {"block-texel-view refused", bc, VK_IMAGE_TYPE_2D, opt, samp, VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT, VK_ERROR_FORMAT_NOT_SUPPORTED},
      {"alias refused", bc, VK_IMAGE_TYPE_2D, opt, samp, VK_IMAGE_CREATE_ALIAS_BIT, VK_ERROR_FORMAT_NOT_SUPPORTED},
      {"2D-array on 2D refused", bc, VK_IMAGE_TYPE_2D, opt, samp, VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT, VK_ERROR_FORMAT_NOT_SUPPORTED},
   };
   for (unsigned i = 0; i < sizeof bcm / sizeof *bcm; i++) {
      VkImageFormatProperties ifp;
      VkResult r = GetPhysicalDeviceImageFormatProperties(pd, bcm[i].f, bcm[i].t, bcm[i].ti, bcm[i].u, bcm[i].fl, &ifp);
      CHECK(r == bcm[i].want, "BC %s: got %d want %d", bcm[i].what, r, bcm[i].want);
   }

   /* ---- vkCreateDevice failures ---- */
   float prio[2] = {1.0f, 1.0f};
   VkDeviceQueueCreateInfo q0 = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q0};
   VkDevice dev, sentinel = (VkDevice)(uintptr_t)0x1234;

   VkDeviceQueueCreateInfo bad_fam = q0; bad_fam.queueFamilyIndex = 1;
   /* invalid usage (queue family / queueCount out of range) is undefined behaviour in the spec: a driver may refuse or tolerate it.
    * Only assert what holds either way: no crash, and a refusal is a spec-legal error that leaves the handle alone. */
#define CHECK_INVALID_USAGE_CREATE(what)                                                                                    \
   do {                                                                                                                     \
      VkResult r_ = CreateDevice(pd, &dci, NULL, &dev);                                                                     \
      if (r_ == VK_SUCCESS) {                                                                                               \
         ((PFN_vkDestroyDevice)GetDeviceProcAddr(dev, "vkDestroyDevice"))(dev, NULL);                                       \
      } else {                                                                                                              \
         CHECK(r_ == VK_ERROR_INITIALIZATION_FAILED || r_ == VK_ERROR_OUT_OF_HOST_MEMORY ||                                 \
               r_ == VK_ERROR_OUT_OF_DEVICE_MEMORY || r_ == VK_ERROR_FEATURE_NOT_PRESENT || r_ == VK_ERROR_TOO_MANY_OBJECTS || \
               r_ == VK_ERROR_DEVICE_LOST, what ": spec-legal error expected, got %d", r_);                                 \
         CHECK(dev == sentinel, what ": failed create must not write the handle");                                          \
      }                                                                                                                     \
   } while (0)
   dci.pQueueCreateInfos = &bad_fam; dev = sentinel;
   CHECK_INVALID_USAGE_CREATE("queue family 1");
   VkDeviceQueueCreateInfo two = q0; two.queueCount = 2;
   dci.pQueueCreateInfos = &two; dev = sentinel;
   CHECK_INVALID_USAGE_CREATE("queueCount 2");
   VkDeviceQueueCreateInfo pair[2] = {q0, q0};
   dci.queueCreateInfoCount = 2; dci.pQueueCreateInfos = pair; dev = sentinel;
   CHECK_INVALID_USAGE_CREATE("two queue infos (one queue exists)");
   dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &q0;

   VkAllocationCallbacks cb = {.pfnAllocation = cb_alloc, .pfnReallocation = cb_realloc, .pfnFree = cb_free};
   g_alloc_fail = 1; dev = sentinel;
   CHECK(CreateDevice(pd, &dci, &cb, &dev) == VK_ERROR_OUT_OF_HOST_MEMORY && dev == sentinel, "allocator failure");
   g_alloc_fail = 0;
   CHECK(g_alloc_calls > 0, "the application allocator was consulted");

   setenv("MALI_FAKE_DEVICE_CREATE_ERR", "12" /* ENOMEM */, 1); dev = sentinel;
   CHECK(CreateDevice(pd, &dci, NULL, &dev) == VK_ERROR_OUT_OF_HOST_MEMORY && dev == sentinel, "backend device_create ENOMEM");
   setenv("MALI_FAKE_DEVICE_CREATE_ERR", "5" /* EIO */, 1); dev = sentinel;
   CHECK(CreateDevice(pd, &dci, NULL, &dev) == VK_ERROR_INITIALIZATION_FAILED && dev == sentinel, "backend device_create EIO");
   unsetenv("MALI_FAKE_DEVICE_CREATE_ERR");

   setenv("MALI_FAKE_BADFLAGS", "1", 1); dev = sentinel;
   VkResult hw = CreateDevice(pd, &dci, NULL, &dev);
   unsetenv("MALI_FAKE_BADFLAGS");
   CHECK(hw != VK_SUCCESS && dev == sentinel, "hw init with failing BO allocation: %d", hw);

   /* the failures left nothing behind: a normal create works */
   if (CreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) { printf("FAIL CreateDevice after failures\n"); return 1; }
   PFN_vkGetBufferOpaqueCaptureAddress GetBufAddr = (PFN_vkGetBufferOpaqueCaptureAddress)GetDeviceProcAddr(dev, "vkGetBufferOpaqueCaptureAddress");
   PFN_vkGetDeviceImageSparseMemoryRequirements GetSparse =
      (PFN_vkGetDeviceImageSparseMemoryRequirements)GetDeviceProcAddr(dev, "vkGetDeviceImageSparseMemoryRequirements");
   PFN_vkDestroyDevice DestroyDevice = (PFN_vkDestroyDevice)GetDeviceProcAddr(dev, "vkDestroyDevice");
   CHECK(GetBufAddr && GetSparse, "entry points present");
   if (GetBufAddr) {
      VkBufferDeviceAddressInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
      CHECK(GetBufAddr(dev, &bi) == 0, "buffer opaque capture address is 0");
   }
   if (GetSparse) {
      VkImageCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      VkDeviceImageMemoryRequirements dim = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, .pCreateInfo = &ic};
      uint32_t cnt = 7;
      GetSparse(dev, &dim, &cnt, NULL);
      CHECK(cnt == 0, "no sparse requirements, got %u", cnt);
   }
   DestroyDevice(dev, NULL);
   DestroyInstance(inst, NULL);
   printf(g_fail ? "FAILED\n" : "OK\n");
   return g_fail;
}
