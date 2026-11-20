/* SPDX-License-Identifier: MIT */
#include "mali_private.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <ucontext.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef __GLIBC__
#include <execinfo.h>
#endif
#ifdef __ANDROID__
#include <android/log.h>
#endif

#ifndef MALI_GIT_SHA
#define MALI_GIT_SHA "unknown"
#endif

#include "mali_entrypoints.h"
#include "vk_alloc.h"
#include "vk_util.h"

static const struct vk_instance_extension_table mali_instance_extensions = {
   .KHR_get_physical_device_properties2 = true,
   .KHR_external_memory_capabilities = true,
   .KHR_external_semaphore_capabilities = true,
   .KHR_external_fence_capabilities = true,
   .EXT_debug_report = true,
   /* M14: surfaces come from wsi_common (Wayland, XCB, Xlib: whatever Mesa was configured with) */
   .KHR_surface = true,
   .KHR_get_surface_capabilities2 = true,
   .EXT_headless_surface = true,       /* wsi_common_headless: tests (t_vk_dxvk_wsi) present without a compositor */
   .EXT_surface_maintenance1 = true,   /* companion of swapchain_maintenance1 (wsi_common) */
   .EXT_swapchain_colorspace = true,   /* ANGLE/gamescope ask for it; wsi_common lists SRGB_NONLINEAR (+ PASS_THROUGH on Wayland), t_steam_caps swcs */
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
   .KHR_wayland_surface = true,
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
   .KHR_xcb_surface = true,    /* X11 WSI: wsi_common_x11 (DRI3/Present with the shm fallback), see STEAM-GAPS.md */
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
   .KHR_xlib_surface = true,
#endif
   .EXT_headless_surface = true,       /* wsi_common_headless.c is compiled on every non-Windows build (ANGLE's VerifyExtensionsPresent needs it) */
   .EXT_swapchain_colorspace = true,   /* wsi_common only offers colour spaces the surface really supports (sRGB-nonlinear everywhere) */
   .EXT_debug_utils = true,
};

/* MALI_TRACE=<file>: opt-in post-mortem trace for processes whose stderr nobody reads (a browser's GPU process, a game under a launcher).
 * One line per event, appended with O_APPEND (safe across processes): load/unload (pid, process name), vkCreateInstance / vkCreateDevice
 * (what was asked, what came back) and fatal signals (with a backtrace on glibc). Unset = nothing is opened, no handler installed. */
static int mali_trace_fd = -1;

void
mali_trace(const char *fmt, ...)
{
   char buf[1024];
   int n;
   va_list ap;

   if (mali_trace_fd < 0)
      return;
   n = snprintf(buf, sizeof(buf), "%d: ", (int)getpid());
   va_start(ap, fmt);
   n += vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
   va_end(ap);
   if (n > (int)sizeof(buf) - 2)
      n = sizeof(buf) - 2;
   buf[n++] = '\n';
   if (write(mali_trace_fd, buf, n) < 0) {}
}

/* one address: "<module>+0x<offset>" (dladdr), or the maps line that holds it (JIT / anonymous code) */
static void
mali_trace_addr(const char *what, uintptr_t a)
{
   Dl_info di;

   if (dladdr((void *)a, &di) && di.dli_fname)
      mali_trace("  %s=0x%lx %s+0x%lx%s%s", what, (unsigned long)a, di.dli_fname, (unsigned long)(a - (uintptr_t)di.dli_fbase),
                 di.dli_sname ? " " : "", di.dli_sname ? di.dli_sname : "");
   else {   /* not in any loaded object (JIT, a stripped or unmapped one): name the mapping that holds it */
      char line[256], buf[4096];
      int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC), len = 0;
      bool done = false;
      mali_trace("  %s=0x%lx (no module)", what, (unsigned long)a);
      if (fd < 0)
         return;
      for (ssize_t n; !done && (n = read(fd, buf, sizeof buf)) > 0;) {
         for (ssize_t i = 0; i < n && !done; i++) {
            if (buf[i] != '\n') {
               if (len < (int)sizeof line - 1)
                  line[len++] = buf[i];
               continue;
            }
            line[len] = 0;
            len = 0;
            unsigned long lo = 0, hi = 0;
            if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && a >= lo && a < hi) {
               mali_trace("    maps: %s", line);
               done = true;
            }
         }
      }
      close(fd);
   }
}

/* The faulting frame itself: glibc's backtrace() from inside the handler stops at the signal trampoline, so it never names the code that
 * crashed. PC/LR/SP from the ucontext, then a frame-pointer walk (aarch64, bounded, strictly rising, 16-aligned). */
static void
mali_trace_fatal(int sig, siginfo_t *si, void *ucv)
{
   ucontext_t *uc = ucv;

   mali_trace("FATAL signal %d code=%d addr=%p", sig, si ? si->si_code : 0, si ? si->si_addr : NULL);
#if defined(__aarch64__)
   if (uc) {
      uintptr_t fp = uc->uc_mcontext.regs[29], sp = uc->uc_mcontext.sp;
      mali_trace_addr("pc", uc->uc_mcontext.pc);
      mali_trace_addr("lr", uc->uc_mcontext.regs[30]);
      mali_trace("  sp=0x%lx fp=0x%lx", (unsigned long)sp, (unsigned long)fp);
      for (int i = 0; i < 24 && fp > sp && !(fp & 15) && fp - sp < (1u << 24); i++) {
         uintptr_t *f = (uintptr_t *)fp;
         if (!f[1])
            break;
         mali_trace_addr("frame", f[1]);
         sp = fp;
         fp = f[0];
      }
   }
#elif defined(__x86_64__)
   if (uc) {
      mali_trace_addr("pc", uc->uc_mcontext.gregs[REG_RIP]);
      mali_trace("  sp=0x%lx", (unsigned long)uc->uc_mcontext.gregs[REG_RSP]);
   }
#endif
#ifdef __GLIBC__
   void *bt[40];
   int cnt = backtrace(bt, 40);
   backtrace_symbols_fd(bt, cnt, mali_trace_fd);
#endif
   signal(sig, SIG_DFL);
   raise(sig);
}

__attribute__((constructor)) static void
mali_trace_init(void)
{
   const char *path = getenv("MALI_TRACE");
   char name[96] = "?";
   ssize_t n;
   int fd;

   if (!path || !*path)
      return;
   mali_trace_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
   if (mali_trace_fd < 0)
      return;
   fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
   if (fd >= 0) {
      n = read(fd, name, sizeof(name) - 1);
      name[n > 0 ? n : 0] = 0;
      for (int i = 0; i + 1 < n; i++)
         if (!name[i])
            name[i] = ' ';
      close(fd);
   }
   mali_trace("load %s", name);
   static const int sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };
   for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
      struct sigaction sa = { .sa_sigaction = mali_trace_fatal, .sa_flags = SA_SIGINFO | SA_RESETHAND | SA_NODEFER };
      sigaction(sigs[i], &sa, NULL);
   }
}

__attribute__((destructor)) static void
mali_trace_fini(void)
{
   mali_trace("unload");
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_EnumerateInstanceVersion(uint32_t *pApiVersion)
{
   uint32_t version_override = vk_get_version_override();

   *pApiVersion = version_override ? version_override : MALI_INSTANCE_API_VERSION;
   return VK_SUCCESS;
}

/* The ICD implements no layers. vkGetInstanceProcAddr(NULL, ...) must still return this command (spec table; DXVK's
 * LibraryFn loads it, so a direct/Wine caller that skips the Khronos loader would jump to NULL). */
VKAPI_ATTR VkResult VKAPI_CALL
mali_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
   (void)pProperties;
   *pPropertyCount = 0;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_EnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pPropertyCount,
                                          VkExtensionProperties *pProperties)
{
   if (pLayerName)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   return vk_enumerate_instance_extension_properties(&mali_instance_extensions, pPropertyCount,
                                                     pProperties);
}

/* Opt-in proof of life: MALI_STATUS_FILE=<path> gets one appended line, MALI_BANNER=1 also logs it to logcat.
 * Never fails instance creation. */
static void
mali_banner(void)
{
   const char *path = getenv("MALI_STATUS_FILE");
   const char *lc = getenv("MALI_BANNER");
   char exe[64] = "?", line[160];
   int fd, n;

   if ((!path || !*path) && !(lc && *lc == '1'))
      return;
   fd = open("/proc/self/comm", O_RDONLY | O_CLOEXEC);
   if (fd >= 0) {
      ssize_t r = read(fd, exe, sizeof(exe) - 1);
      exe[r > 0 ? r : 0] = 0;
      exe[strcspn(exe, "\n")] = 0;
      close(fd);
   }
   n = snprintf(line, sizeof(line) - 1, "mali-open %s pid %d exe %s", MALI_GIT_SHA, (int)getpid(), exe);
   if (n <= 0)
      return;
   if (path && *path) {
      /* O_NONBLOCK: a FIFO without a reader must not block vkCreateInstance; only regular files are written */
      fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NONBLOCK, 0644);
      if (fd >= 0) {
         struct stat st;
         if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
            line[n++] = '\n';
            if (write(fd, line, n) < 0) {}
            line[--n] = 0;
         }
         close(fd);
      }
   }
#ifdef __ANDROID__
   if (lc && *lc == '1')
      __android_log_write(ANDROID_LOG_INFO, "mali-open", line);
#endif
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
   struct mali_instance *instance;
   VkResult result;

   if (mali_trace_fd >= 0) {
      const VkApplicationInfo *ai = pCreateInfo->pApplicationInfo;
      mali_trace("vkCreateInstance app=%s engine=%s api=0x%x exts=%u layers=%u", ai && ai->pApplicationName ? ai->pApplicationName : "-",
                 ai && ai->pEngineName ? ai->pEngineName : "-", ai ? ai->apiVersion : 0, pCreateInfo->enabledExtensionCount,
                 pCreateInfo->enabledLayerCount);
      for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++)
         mali_trace("  instance ext %s", pCreateInfo->ppEnabledExtensionNames[i]);
   }
   pAllocator = pAllocator ? pAllocator : vk_default_allocator();
   instance = vk_zalloc(pAllocator, sizeof(*instance), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_instance_dispatch_table dispatch_table;
   vk_instance_dispatch_table_from_entrypoints(&dispatch_table, &mali_instance_entrypoints, true);
   vk_instance_dispatch_table_from_entrypoints(&dispatch_table, &wsi_instance_entrypoints, false);

   result = vk_instance_init(&instance->vk, &mali_instance_extensions, &dispatch_table,
                             pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      mali_trace("vkCreateInstance -> %d", result);
      vk_free(pAllocator, instance);
      return vk_error(NULL, result);
   }

   instance->vk.physical_devices.enumerate = mali_enumerate_physical_devices;
   instance->vk.physical_devices.destroy = mali_destroy_physical_device;

   *pInstance = mali_instance_to_handle(instance);
   mali_banner();
   mali_trace("vkCreateInstance -> ok (api %u.%u)", VK_API_VERSION_MAJOR(instance->vk.app_info.api_version),
              VK_API_VERSION_MINOR(instance->vk.app_info.api_version));
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
mali_DestroyInstance(VkInstance _instance, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(mali_instance, instance, _instance);

   if (!instance)
      return;

   vk_instance_finish(&instance->vk);
   vk_free(&instance->vk.alloc, instance);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
mali_GetInstanceProcAddr(VkInstance _instance, const char *pName)
{
   VK_FROM_HANDLE(mali_instance, instance, _instance);

   return vk_instance_get_proc_addr(instance ? &instance->vk : NULL, &mali_instance_entrypoints,
                                    pName);
}

/* Loader entry point. vk_icdNegotiateLoaderICDInterfaceVersion and
 * vk_icdGetPhysicalDeviceProcAddr come from the common runtime (vk_instance.c). */
PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   return mali_GetInstanceProcAddr(instance, pName);
}
