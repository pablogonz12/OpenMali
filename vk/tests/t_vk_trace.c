/* SPDX-License-Identifier: MIT */
/* MALI_TRACE=<file>: the opt-in post-mortem trace (for processes whose stderr nobody reads: a browser's GPU process under Steam).
 *   t_vk_trace --direct <icd.so>
 * 1. with MALI_TRACE unset nothing is created;
 * 2. with it set: a load line (pid + process name), vkCreateInstance (app name, api, extensions) and its result, vkCreateDevice and its
 *    result, an unload line on dlclose;
 * 3. a child that dies of SIGSEGV after loading the ICD leaves "FATAL signal 11" in the file;
 * 4. a failing vkCreateDevice (unsupported extension) records the result and the extension it was asked for. */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

static char *slurp(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;
   static char buf[16384];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   buf[n] = 0;
   fclose(f);
   return buf;
}

typedef PFN_vkVoidFunction (*gipa_t)(VkInstance, const char *);

/* load the ICD, create an instance and a device (optionally asking for a bogus extension); returns the vkCreateDevice result */
static VkResult drive(void *lib, int bogus_ext)
{
   gipa_t gipa = (gipa_t)dlsym(lib, "vk_icdGetInstanceProcAddr");
   CHECK(gipa, "no vk_icdGetInstanceProcAddr");
   PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "t_vk_trace_app", .apiVersion = VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
   VkInstance inst = NULL;
   CHECK(ci && ci(&ici, NULL, &inst) == VK_SUCCESS, "vkCreateInstance");
   if (!inst)
      return VK_ERROR_INITIALIZATION_FAILED;
   PFN_vkEnumeratePhysicalDevices ep = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   uint32_t n = 1;
   VkPhysicalDevice pd = NULL;
   VkResult er = ep(inst, &n, &pd);
   CHECK((er == VK_SUCCESS || er == VK_INCOMPLETE) && n >= 1, "enumerate");
   PFN_vkCreateDevice cd = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
   const char *bad[] = { "VK_NV_this_does_not_exist" };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
   if (bogus_ext) {
      dci.enabledExtensionCount = 1;
      dci.ppEnabledExtensionNames = bad;
   }
   VkDevice dev = NULL;
   VkResult r = cd(pd, &dci, NULL, &dev);
   if (r == VK_SUCCESS) {
      PFN_vkDestroyDevice dd = (PFN_vkDestroyDevice)gipa(inst, "vkDestroyDevice");
      dd(dev, NULL);
   }
   PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
   di(inst, NULL);
   return r;
}

int main(int argc, char **argv)
{
   const char *so = NULL;
   for (int i = 1; i < argc; i++)
      if (!strcmp(argv[i], "--direct") && i + 1 < argc)
         so = argv[++i];
   if (!so) {
      fprintf(stderr, "usage: t_vk_trace --direct <icd.so>\n");
      return 2;
   }
   char path[128];
   snprintf(path, sizeof(path), "/tmp/t_vk_trace.%d.log", (int)getpid());
   unlink(path);

   /* 1. unset: no file */
   unsetenv("MALI_TRACE");
   void *lib = dlopen(so, RTLD_NOW | RTLD_LOCAL);
   CHECK(lib, "dlopen %s: %s", so, dlerror());
   if (!lib)
      return 1;
   CHECK(drive(lib, 0) == VK_SUCCESS, "device create (trace off)");
   struct stat st;
   CHECK(stat(path, &st) != 0, "a trace file appeared although MALI_TRACE was unset");
   dlclose(lib);

   /* 2 + 4. set: a normal run, then a failing device create, in a child so the constructor runs again */
   pid_t c = fork();
   if (c == 0) {
      setenv("MALI_TRACE", path, 1);
      void *l = dlopen(so, RTLD_NOW | RTLD_LOCAL);
      if (!l)
         _exit(3);
      VkResult ok = drive(l, 0);
      VkResult bad = drive(l, 1);
      dlclose(l);
      _exit(ok == VK_SUCCESS && bad != VK_SUCCESS ? 0 : 4);
   }
   int ws = 0;
   waitpid(c, &ws, 0);
   CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "child status 0x%x (device create must succeed, the bogus-extension one must fail)", ws);
   char *t = slurp(path);
   CHECK(t, "no trace file");
   if (t) {
      CHECK(strstr(t, ": load "), "no load line:\n%s", t);
      CHECK(strstr(t, "vkCreateInstance app=t_vk_trace_app"), "no vkCreateInstance request line:\n%s", t);
      CHECK(strstr(t, "vkCreateInstance -> ok"), "no vkCreateInstance result:\n%s", t);
      CHECK(strstr(t, "vkCreateDevice exts=0 queues=1 -> 0"), "no successful vkCreateDevice line:\n%s", t);
      CHECK(strstr(t, "vkCreateDevice exts=1 queues=1 -> -7"), "no failed vkCreateDevice line (VK_ERROR_EXTENSION_NOT_PRESENT = -7):\n%s", t);
      CHECK(strstr(t, "requested ext VK_NV_this_does_not_exist"), "failed create does not name the extension:\n%s", t);
      CHECK(strstr(t, ": unload"), "no unload line:\n%s", t);
   }
   unlink(path);

   /* 3. a fatal signal after the ICD loaded */
   c = fork();
   if (c == 0) {
      setenv("MALI_TRACE", path, 1);
      void *l = dlopen(so, RTLD_NOW | RTLD_LOCAL);
      if (!l)
         _exit(3);
      *(volatile int *)0 = 1;   /* a real fault: the handler must name the faulting address and pc */
      _exit(5);
   }
   waitpid(c, &ws, 0);
   CHECK(WIFSIGNALED(ws) && WTERMSIG(ws) == SIGSEGV, "the handler must re-raise: status 0x%x", ws);
   t = slurp(path);
   CHECK(t && strstr(t, "FATAL signal 11"), "no FATAL line:\n%s", t ? t : "(no file)");
   CHECK(t && (strstr(t, "addr=(nil)") || strstr(t, "addr=0x0")), "FATAL line has no fault address:\n%s", t ? t : "");
   CHECK(t && strstr(t, "  pc=0x"), "no pc line:\n%s", t ? t : "");
   unlink(path);

#ifdef __x86_64__
   /* 4. a fault in anonymous executable memory (JIT): no module, the handler names the mapping */
   unlink(path);
   c = fork();
   if (c == 0) {
      setenv("MALI_TRACE", path, 1);
      void *l = dlopen(so, RTLD_NOW | RTLD_LOCAL);
      if (!l)
         _exit(3);
      unsigned char *m = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      memcpy(m, "\x48\x31\xc0\x8b\x00", 5);   /* xor rax,rax; mov eax,[rax] */
      ((void (*)(void))m)();
      _exit(5);
   }
   waitpid(c, &ws, 0);
   t = slurp(path);
   CHECK(t && strstr(t, "(no module)") && strstr(t, "    maps: "), "no mapping line for a pc outside every module:\n%s", t ? t : "(no file)");
#endif
   unlink(path);

   printf("%s\n", g_fail ? "FAIL" : "OK t_vk_trace");
   return g_fail;
}
