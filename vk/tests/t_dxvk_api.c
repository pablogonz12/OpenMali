/* SPDX-License-Identifier: MIT */
/* DXVK entry-point replay (lane dxvkapi). tests/dxvk_api_table.h lists every command DXVK master loads through its
 * LibraryFn/InstanceFn/DeviceFn (src/vulkan/vulkan_loader.h) with the vk.xml owner (core version or extension).
 * DXVK fetches those pointers unconditionally and calls the core ones and those of every extension it enabled without
 * a NULL check, so:
 *   - every core (1.0-1.3) command must resolve (vkGetInstanceProcAddr / vkGetDeviceProcAddr), and
 *   - every command of an extension we advertise (and DXVK uses) must resolve.
 * Commands of extensions we do not advertise are reported as "optional, absent" (DXVK degrades; it tests the extension
 * name first); they are not failures. Wine-only wine_vk* are provided by winevulkan, not the ICD: ignored.
 *   t_dxvk_api --direct <icd.so> [--verbose]
 * Exit 0 = pass. */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dxvk_api_table.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); g_fail = 1; } } while (0)

static int has(const VkExtensionProperties *p, uint32_t n, const char *name)
{
   for (uint32_t i = 0; i < n; i++)
      if (!strcmp(p[i].extensionName, name))
         return 1;
   return 0;
}

static int is_core(const char *owner) { return !strncmp(owner, "VK_BASE_VERSION_1_", 18) || !strncmp(owner, "VK_COMPUTE_VERSION_1_", 21) || !strncmp(owner, "VK_GRAPHICS_VERSION_1_", 22) || !strncmp(owner, "VK_VERSION_1_", 13); }

/* vk.xml `depends` expression: ',' = or, '+' = and (parentheses ignored); VK_VERSION_1_0..1_3 count as available. A command whose
 * extension is advertised but whose dependency is not is not part of that extension's advertised surface. */
static int dep_ok(const char *dep, const VkExtensionProperties *a, uint32_t na, const VkExtensionProperties *b, uint32_t nb)
{
   if (!dep || !*dep) return 1;
   char buf[256];
   snprintf(buf, sizeof buf, "%s", dep);
   for (char *or_ = buf, *save_or; (or_ = strtok_r(or_, ",", &save_or)); or_ = NULL) {
      char term[256]; snprintf(term, sizeof term, "%s", or_);
      int all = 1;
      for (char *t = term, *save_and; (t = strtok_r(t, "+", &save_and)); t = NULL) {
         while (*t == '(') t++;
         char *e = t + strlen(t); while (e > t && e[-1] == ')') *--e = 0;
         int ok = !strncmp(t, "VK_VERSION_1_", 13) ? t[13] >= '0' && t[13] <= '3' : has(a, na, t) || has(b, nb, t);
         if (!ok) { all = 0; break; }
      }
      if (all) return 1;
   }
   return 0;
}

int main(int argc, char **argv)
{
   const char *icd = NULL;
   int verbose = 0;
   for (int i = 1; i < argc; i++)
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--verbose")) verbose = 1;
   if (!icd) { printf("usage: t_dxvk_api --direct <icd.so> [--verbose]\n"); return 2; }
   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa) { printf("no vk_icdGetInstanceProcAddr\n"); return 1; }

   PFN_vkEnumerateInstanceExtensionProperties eiep = (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
   PFN_vkCreateInstance cin = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   CHECK(eiep && cin, "global commands (DXVK LibraryFn)");
   if (!eiep || !cin) return 1;
   uint32_t nie = 0;
   eiep(NULL, &nie, NULL);
   VkExtensionProperties *ie = calloc(nie + 1, sizeof *ie);
   eiep(NULL, &nie, ie);
   const char **iuse = calloc(nie + 1, sizeof *iuse);
   for (uint32_t i = 0; i < nie; i++) iuse[i] = ie[i].extensionName;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pEngineName = "DXVK", .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .enabledExtensionCount = nie, .ppEnabledExtensionNames = iuse};
   VkInstance inst = NULL;
   VkResult r = cin(&ici, NULL, &inst);
   CHECK(r == VK_SUCCESS, "vkCreateInstance: %d", r);
   if (!inst) { free(ie); free(iuse); return 1; }

#define I(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n)
   I(EnumeratePhysicalDevices); I(EnumerateDeviceExtensionProperties); I(CreateDevice); I(GetDeviceProcAddr); I(DestroyInstance);
   CHECK(EnumeratePhysicalDevices && EnumerateDeviceExtensionProperties && CreateDevice && GetDeviceProcAddr, "instance core");
   uint32_t np = 1;
   VkPhysicalDevice pd = NULL;
   EnumeratePhysicalDevices(inst, &np, &pd);
   CHECK(np >= 1 && pd, "no physical device");
   if (!pd) { free(ie); free(iuse); return 1; }
   uint32_t ne = 0;
   EnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
   VkExtensionProperties *eps = calloc(ne + 1, sizeof *eps);
   EnumerateDeviceExtensionProperties(pd, NULL, &ne, eps);
   const char **use = calloc(ne + 1, sizeof *use);
   for (uint32_t i = 0; i < ne; i++) use[i] = eps[i].extensionName;

   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = (float[]){1.0f}};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                             .enabledExtensionCount = ne, .ppEnabledExtensionNames = use};
   VkDevice dev = NULL;
   r = CreateDevice(pd, &dci, NULL, &dev);
   CHECK(r == VK_SUCCESS, "vkCreateDevice with all %u advertised extensions: %d", ne, r);
   if (!dev) { free(ie); free(iuse); free(eps); free(use); return 1; }

   unsigned req = 0, req_missing = 0, opt_absent = 0, opt_present = 0, wine = 0;
   for (unsigned i = 0; i < sizeof dxvk_api / sizeof dxvk_api[0]; i++) {
      const struct dxa_fn *f = &dxvk_api[i];
      if (!strncmp(f->name, "wine_", 5)) { wine++; continue; }
      if (f->level == 0) {   /* LibraryFn: resolved with a NULL instance */
         PFN_vkVoidFunction p = gipa(NULL, f->name);
         CHECK(p, "global command %s is NULL through GIPA(NULL)", f->name);
         if (p && !strcmp(f->name, "vkEnumerateInstanceLayerProperties")) {
            uint32_t nl = 99;
            VkResult lr = ((PFN_vkEnumerateInstanceLayerProperties)p)(&nl, NULL);
            CHECK(lr == VK_SUCCESS && nl == 0, "ICD has no layers: result %d count %u", lr, nl);
         }
         req++;
         continue;
      }
      /* an extension DXVK uses, advertised by the instance or the device */
      int adv = (is_core(f->owner) || has(ie, nie, f->owner) || has(eps, ne, f->owner)) && dep_ok(f->depends, ie, nie, eps, ne);
      PFN_vkVoidFunction p = f->level == 2 ? GetDeviceProcAddr(dev, f->name) : gipa(inst, f->name);
      if (adv) {
         req++;
         if (!p) { req_missing++; printf("MISSING required %s %s (%s)\n", f->level == 2 ? "device" : "instance", f->name, f->owner); }
      } else {
         if (p) opt_present++; else opt_absent++;
         if (verbose) printf("optional %-7s %s (%s)\n", p ? "present" : "absent", f->name, f->owner);
      }
   }
   printf("dxvk_api: %u commands required (core or advertised extension), %u NULL; optional absent %u, present %u; wine_* skipped %u\n", req, req_missing, opt_absent, opt_present, wine);
   CHECK(req_missing == 0, "%u DXVK commands of core/advertised extensions resolve to NULL (DXVK calls them unchecked)", req_missing);
   CHECK(req > 200, "only %u required commands replayed: table not applied", req);
   ((PFN_vkDestroyDevice)GetDeviceProcAddr(dev, "vkDestroyDevice"))(dev, NULL);
   DestroyInstance(inst, NULL);
   free(ie); free(iuse); free(eps); free(use);
   printf("{\"test\":\"t_dxvk_api\",\"pass\":%s,\"required\":%u,\"null\":%u}\n", g_fail ? "false" : "true", req, req_missing);
   return g_fail;
}
