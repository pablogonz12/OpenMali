/* SPDX-License-Identifier: MIT */
/* VK_EXT_physical_device_drm: the extension exists only together with a node that stat() proves is the
 * char device the backend drives (the bridge's /dev/dri/renderD<N> alias of /dev/mali0 in the guest,
 * BL_DRM_STANDIN=/dev/mali0), and its numbers are that node's st_rdev (STEAM-GAPS.md, zink choose_pdev).
 *   t_vk_drm --direct <icd.so> --none                   no node: extension absent, no DRM identity
 *   t_vk_drm --direct <icd.so> --node <render path> [--primary <card path>]
 *                                                       extension present; renderMajor/Minor == stat(node);
 *                                                       primary only when --primary is given
 * The caller sets BL_DRM_STANDIN/MALI_DRM_DIR (check.sh builds a temp dir with symlinks). */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

static int g_fail;
#define CHECK(c, ...)                                                                              \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c);                                        \
         printf(__VA_ARGS__);                                                                      \
         printf("\n");                                                                             \
         g_fail = 1;                                                                               \
      }                                                                                            \
   } while (0)

static int
has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
   for (uint32_t i = 0; i < n; i++)
      if (!strcmp(e[i].extensionName, name))
         return 1;
   return 0;
}

static int
node_rdev(const char *path, dev_t *out)
{
   struct stat st;
   if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode))
      return 0;
   *out = st.st_rdev;
   return 1;
}

int
main(int argc, char **argv)
{
   const char *icd = NULL, *node = NULL, *primary = NULL;
   int expect_none = 0;
   for (int i = 1; i < argc; i++)
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--node") && i + 1 < argc) node = argv[++i];
      else if (!strcmp(argv[i], "--primary") && i + 1 < argc) primary = argv[++i];
      else if (!strcmp(argv[i], "--none")) expect_none = 1;
   if (!icd || (!expect_none && !node)) {
      printf("usage: t_vk_drm --direct <icd.so> (--none | --node <path> [--primary <path>])\n");
      return 2;
   }

   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");

   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "t_vk_drm", .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst = NULL;
   CHECK(CreateInstance(&ici, NULL, &inst) == VK_SUCCESS, "CreateInstance");
   if (!inst) return 1;

#define I(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n)
   I(EnumeratePhysicalDevices);
   I(EnumerateDeviceExtensionProperties);
   I(GetPhysicalDeviceProperties2);
#undef I
   uint32_t n = 1;
   VkPhysicalDevice pd;
   CHECK(EnumeratePhysicalDevices(inst, &n, &pd) == VK_SUCCESS && n == 1, "one physical device");

   uint32_t ne = 0;
   EnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
   VkExtensionProperties eps[160];
   if (ne > 160) ne = 160;
   EnumerateDeviceExtensionProperties(pd, NULL, &ne, eps);
   int have = has_ext(eps, ne, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);

   VkPhysicalDeviceDrmPropertiesEXT drm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
   VkPhysicalDeviceProperties2 pr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drm};
   GetPhysicalDeviceProperties2(pd, &pr);

   if (expect_none) {
      printf("no node: VK_EXT_physical_device_drm %s, hasRender=%u hasPrimary=%u\n",
             have ? "advertised" : "absent", drm.hasRender, drm.hasPrimary);
      CHECK(!have, "extension advertised without a matching DRM node");
      CHECK(!drm.hasRender && !drm.hasPrimary, "DRM identity reported without a matching node");
      printf("%s\n", g_fail ? "FAILED" : "PASS");
      return g_fail;
   }

   dev_t want = 0;
   CHECK(node_rdev(node, &want), "stat %s", node);
   printf("node %s = %u:%u (extension %s)\n", node, major(want), minor(want), have ? "present" : "ABSENT");
   CHECK(have, "extension missing with a matching node");
   CHECK(drm.hasRender, "hasRender");
   CHECK(drm.renderMajor == (int64_t)major(want) && drm.renderMinor == (int64_t)minor(want),
         "render %lld:%lld != stat %u:%u", (long long)drm.renderMajor, (long long)drm.renderMinor, major(want), minor(want));

   if (primary) {
      dev_t pwant = 0;
      CHECK(node_rdev(primary, &pwant), "stat %s", primary);
      printf("primary %s = %u:%u\n", primary, major(pwant), minor(pwant));
      CHECK(drm.hasPrimary, "hasPrimary");
      CHECK(drm.primaryMajor == (int64_t)major(pwant) && drm.primaryMinor == (int64_t)minor(pwant),
            "primary %lld:%lld != stat %u:%u", (long long)drm.primaryMajor, (long long)drm.primaryMinor, major(pwant), minor(pwant));
   } else {
      CHECK(!drm.hasPrimary, "hasPrimary without a card node");
   }

   printf("%s\n", g_fail ? "FAILED" : "PASS");
   return g_fail;
}
