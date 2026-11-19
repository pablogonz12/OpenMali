/* SPDX-License-Identifier: MIT */
/* DXVK's presentation pattern on the ICD, loader-free, fake backend, VK_EXT_headless_surface (no compositor), with a watchdog.
 *   t_vk_dxvk_wsi --direct <icd.so> [--frames N]
 * Per frame (what dxvk::Presenter / the submit path does): wait the fence of the frame that used this slot (frame latency 3),
 * vkAcquireNextImageKHR(UINT64_MAX, acquireSem[slot]), vkQueueSubmit(wait acquireSem, run a clear of the image, signal
 * presentSem[image] + frame fence), vkQueuePresentKHR(wait presentSem[image]).
 * Phases over N (default 1000) frames: FIFO 3 images; resize (recreate with oldSwapchain, new extent, waitIdle); MAILBOX
 * recreate; IMMEDIATE recreate without waitIdle (only the frame fences waited); acquire timeout 0 / 1 ms must return
 * NOT_READY/TIMEOUT promptly with every image held; OUT_OF_DATE/SUBOPTIMAL from acquire/present trigger a recreate.
 * A watchdog thread aborts with exit 3 when no frame completes for 10 s (the deadlock this test guards). Exit 0 = all ok. */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LAT 3
static int g_fail;
static _Atomic unsigned long g_progress;   /* watched from another thread */
static _Atomic(const char *) g_where = "init";
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); g_fail = 1; } } while (0)
#define STEP(s) atomic_store(&g_where, (s))

static void *
watchdog(void *a)
{
   (void)a;
   unsigned long last = atomic_load(&g_progress);
   int idle = 0;
   for (;;) {
      usleep(100000);
      unsigned long cur = atomic_load(&g_progress);
      if (cur != last) { last = cur; idle = 0; continue; }
      if (++idle >= 100) {
         printf("FAIL watchdog: no progress for 10 s after %lu steps, stuck in %s\n", last, atomic_load(&g_where));
         fflush(stdout);
         _exit(3);
      }
   }
}

#define DEV_FNS(X) X(DestroyDevice) X(GetDeviceQueue) X(CreateSemaphore) X(DestroySemaphore) X(CreateFence) X(DestroyFence) \
   X(WaitForFences) X(ResetFences) X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) \
   X(QueuePresentKHR) X(QueueSubmit) X(DeviceWaitIdle) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) \
   X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdClearColorImage) X(ResetCommandPool) X(WaitForPresentKHR)
#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)

static VkDevice dev;
static VkQueue q;
static VkSurfaceKHR surf;
static VkSwapchainKHR sc;
static VkImage imgs[8];
static uint32_t nimg;
static VkExtent2D ext;
static VkSemaphore acq[LAT], prs[8];
static VkFence fen[LAT];
static int fen_used[LAT];
static VkCommandPool pools[LAT];   /* one pool per frame slot: resetting a slot never touches another slot's pending buffer */
static VkCommandBuffer cbs[LAT];
static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetCaps;
static VkPhysicalDevice pd;
static uint64_t g_last_ok_pid;   /* newest present id actually queued on this swapchain */
static uint64_t g_present_id;   /* per swapchain: restarts at 1 on every recreate (ids only have to increase within one swapchain) */

static void
destroy_sync(void)
{
   for (int i = 0; i < LAT; i++) if (acq[i]) { DestroySemaphore(dev, acq[i], NULL); acq[i] = 0; }
   for (int i = 0; i < 8; i++) if (prs[i]) { DestroySemaphore(dev, prs[i], NULL); prs[i] = 0; }
}

static void
make_sync(void)
{
   VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   for (int i = 0; i < LAT; i++) CHECK(CreateSemaphore(dev, &si, NULL, &acq[i]) == VK_SUCCESS, "sem");
   for (uint32_t i = 0; i < nimg; i++) CHECK(CreateSemaphore(dev, &si, NULL, &prs[i]) == VK_SUCCESS, "sem");
}

static void
drain_fences(void)
{
   STEP("drain fences");
   for (int i = 0; i < LAT; i++)
      if (fen_used[i]) { CHECK(WaitForFences(dev, 1, &fen[i], VK_TRUE, UINT64_MAX) == VK_SUCCESS, "wait fence"); ResetFences(dev, 1, &fen[i]); fen_used[i] = 0; }
}

/* DXVK Presenter::recreateSwapChain: new swapchain with oldSwapchain, then the old one is destroyed. */
static void
recreate(VkExtent2D e, VkPresentModeKHR mode, uint32_t min_img, int wait_idle)
{
   STEP("recreate");
   if (wait_idle) { CHECK(DeviceWaitIdle(dev) == VK_SUCCESS, "waitidle"); }
   else drain_fences();
   VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf, .minImageCount = min_img,
      .imageFormat = VK_FORMAT_B8G8R8A8_UNORM, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = e, .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = mode,
      .clipped = VK_TRUE, .oldSwapchain = sc};
   VkSwapchainKHR n = VK_NULL_HANDLE;
   VkResult r = CreateSwapchainKHR(dev, &ci, NULL, &n);
   CHECK(r == VK_SUCCESS, "CreateSwapchain -> %d", r);
   if (r != VK_SUCCESS) { printf("FAIL cannot continue\n"); exit(1); }
   if (!wait_idle && sc && g_last_ok_pid) {   /* presents still wait on prs[]: let the newest one finish (frame() only waits pid-2) */
      STEP("wait outstanding presents");
      VkResult wr = WaitForPresentKHR(dev, sc, g_last_ok_pid, 5000000000ull);
      CHECK(wr == VK_SUCCESS || wr == VK_ERROR_OUT_OF_DATE_KHR, "wait outstanding present id %llu -> %d", (unsigned long long)g_last_ok_pid, wr);
   }
   destroy_sync();
   if (sc) DestroySwapchainKHR(dev, sc, NULL);
   sc = n; ext = e; g_present_id = 0; g_last_ok_pid = 0;
   nimg = 8;
   CHECK(GetSwapchainImagesKHR(dev, sc, &nimg, imgs) == VK_SUCCESS, "images");
   CHECK(nimg >= min_img, "got %u images for min %u", nimg, min_img);
   make_sync();
   for (int i = 0; i < LAT; i++) ResetCommandPool(dev, pools[i], 0);
   atomic_fetch_add(&g_progress, 1);
}

static void
record(VkCommandBuffer cb, VkImage im, float c)
{
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   BeginCommandBuffer(cb, &bi);
   VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im, .subresourceRange = rg};
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,   /* chained behind the acquire wait (also TRANSFER) */ 0, 0, NULL, 0, NULL, 1, &b);
   VkClearColorValue cv = {.float32 = {c, 0.5f, 0.25f, 1}};
   CmdClearColorImage(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rg);
   b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
   b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   EndCommandBuffer(cb);
}

static uint64_t
now_ms(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1000ull + t.tv_nsec / 1000000;
}

/* one frame; 0 = presented, 1 = presented but the swapchain must be recreated (SUBOPTIMAL/present failure),
 * 2 = no frame was submitted (acquire OUT_OF_DATE): the caller recreates and retries the SAME frame */
static int
frame(unsigned long n)
{
   int slot = n % LAT;
   STEP("wait frame fence");
   if (fen_used[slot]) {
      CHECK(WaitForFences(dev, 1, &fen[slot], VK_TRUE, UINT64_MAX) == VK_SUCCESS, "fence wait frame %lu", n);
      ResetFences(dev, 1, &fen[slot]);
      fen_used[slot] = 0;
   }
   STEP("acquire");
   uint32_t idx = 0;
   VkResult r = AcquireNextImageKHR(dev, sc, UINT64_MAX, acq[slot], VK_NULL_HANDLE, &idx);
   if (r == VK_ERROR_OUT_OF_DATE_KHR) return 2;
   CHECK(r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR, "acquire frame %lu -> %d", n, r);
   if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return 2;
   CHECK(idx < nimg, "index %u of %u", idx, nimg);
   STEP("submit");
   ResetCommandPool(dev, pools[slot], 0);   /* this slot's own pool: its previous use was fence-waited above */
   record(cbs[slot], imgs[idx], (n % 16) / 16.0f);
   VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;   /* first use of the image is the transfer layout change + clear */
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acq[slot], .pWaitDstStageMask = &ws,
      .commandBufferCount = 1, .pCommandBuffers = &cbs[slot], .signalSemaphoreCount = 1, .pSignalSemaphores = &prs[idx]};
   CHECK(QueueSubmit(q, 1, &si, fen[slot]) == VK_SUCCESS, "submit");
   fen_used[slot] = 1;
   STEP("present");
   uint64_t pid = ++g_present_id;
   VkPresentIdKHR pidi = {.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1, .pPresentIds = &pid};
   VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = &pidi, .waitSemaphoreCount = 1, .pWaitSemaphores = &prs[idx],
      .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &idx};
   VkResult pr = QueuePresentKHR(q, &pi);
   CHECK(pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR || pr == VK_ERROR_OUT_OF_DATE_KHR, "present frame %lu -> %d", n, pr);
   /* DXVK (VK_KHR_present_wait): the frame limiter waits for the present `LAT-1` frames back; a presentation that never
    * completes is a hang here. Not wait on a failed present (the id was not queued). */
   if (pr == VK_SUCCESS) g_last_ok_pid = pid;
   if (pr == VK_SUCCESS && pid > 2) {
      STEP("wait for present");
      VkResult wr = WaitForPresentKHR(dev, sc, pid - 2, 5000000000ull);
      CHECK(wr == VK_SUCCESS, "vkWaitForPresentKHR(id %llu) -> %d", (unsigned long long)(pid - 2), wr);
   }
   atomic_fetch_add(&g_progress, 1);
   return r == VK_SUBOPTIMAL_KHR || pr != VK_SUCCESS;
}

/* every image held (acquired, not presented): a 0 / 1 ms acquire must give NOT_READY / TIMEOUT, quickly, and not hang */
static void
timeout_semantics(void)
{
   STEP("timeout semantics");
   drain_fences();
   uint32_t idx, held = 0;
   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};   /* every acquire needs a semaphore or a fence */
   VkFence af;
   CHECK(CreateFence(dev, &fci, NULL, &af) == VK_SUCCESS, "acquire fence");
   VkResult r;
   /* the headless/sw swapchains may keep one image back (min image count semantics): acquire until it refuses */
   while (held < nimg + 1 && (r = AcquireNextImageKHR(dev, sc, 0, VK_NULL_HANDLE, af, &idx)) == VK_SUCCESS) {
      held++;
      CHECK(WaitForFences(dev, 1, &af, VK_TRUE, 5000000000ull) == VK_SUCCESS, "acquire fence signal");
      ResetFences(dev, 1, &af);
   }
   CHECK(held >= 1 && held <= nimg, "acquired %u of %u images before running dry", held, nimg);
   CHECK(r == VK_NOT_READY, "acquire(timeout 0) with no free image -> %d (want VK_NOT_READY)", r);
   uint64_t t0 = now_ms();
   r = AcquireNextImageKHR(dev, sc, 1000000, VK_NULL_HANDLE, af, &idx);
   uint64_t dt = now_ms() - t0;
   /* headless hands back a just-presented image on the second try, so SUCCESS is legal here; only a hang or an error is not */
   CHECK(r == VK_SUCCESS || r == VK_TIMEOUT || r == VK_NOT_READY, "acquire(1 ms) -> %d", r);
   CHECK(dt < 1000, "acquire(1 ms) took %llu ms", (unsigned long long)dt);
   if (r == VK_SUCCESS) { WaitForFences(dev, 1, &af, VK_TRUE, 5000000000ull); ResetFences(dev, 1, &af); }
   DestroyFence(dev, af, NULL);
   /* held images are returned by destroying/recreating the chain, like DXVK after a lost surface */
   atomic_fetch_add(&g_progress, 1);
}

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   unsigned long frames = 1000;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = strtoul(argv[++i], NULL, 0);
   }
   if (!icd) { fprintf(stderr, "usage: t_vk_dxvk_wsi --direct icd.so [--frames N]\n"); return 2; }
   pthread_t wt;
   pthread_create(&wt, NULL, watchdog, NULL);

   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   VkResult (*negotiate)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   uint32_t iv = 5;
   if (!gipa || !negotiate || negotiate(&iv)) { printf("icd entry\n"); return 1; }
   const char *iext[] = {"VK_KHR_surface", "VK_EXT_headless_surface"};
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .enabledExtensionCount = 2, .ppEnabledExtensionNames = iext};
   VkInstance inst;
   VkResult r = ((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst);
   if (r != VK_SUCCESS) { printf("FAIL CreateInstance(headless) -> %d\n", r); return 1; }
#define IL(T, n) T n = (T)gipa(inst, "vk" #n); if (!n) { printf("missing vk" #n "\n"); return 1; }
   IL(PFN_vkEnumeratePhysicalDevices, EnumeratePhysicalDevices)
   IL(PFN_vkCreateDevice, CreateDevice)
   IL(PFN_vkGetDeviceProcAddr, GetDeviceProcAddr)
   IL(PFN_vkDestroyInstance, DestroyInstance)
   IL(PFN_vkCreateHeadlessSurfaceEXT, CreateHeadlessSurfaceEXT)
   IL(PFN_vkDestroySurfaceKHR, DestroySurfaceKHR)
   IL(PFN_vkGetPhysicalDeviceSurfaceSupportKHR, GetPhysicalDeviceSurfaceSupportKHR)
   IL(PFN_vkGetPhysicalDeviceSurfacePresentModesKHR, GetPhysicalDeviceSurfacePresentModesKHR)
   GetCaps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gipa(inst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
   uint32_t cnt = 1;
   r = EnumeratePhysicalDevices(inst, &cnt, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || cnt < 1) { printf("no device\n"); return 1; }

   VkHeadlessSurfaceCreateInfoEXT hsi = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
   CHECK(CreateHeadlessSurfaceEXT(inst, &hsi, NULL, &surf) == VK_SUCCESS, "headless surface");
   VkBool32 sup = 0;
   GetPhysicalDeviceSurfaceSupportKHR(pd, 0, surf, &sup);
   CHECK(sup, "queue family 0 cannot present");
   VkSurfaceCapabilitiesKHR caps;
   CHECK(GetCaps(pd, surf, &caps) == VK_SUCCESS, "caps");
   uint32_t nm = 0;
   GetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &nm, NULL);
   VkPresentModeKHR modes[8];
   if (nm > 8) nm = 8;
   GetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &nm, modes);
   int have_fifo = 0, have_mb = 0, have_imm = 0;
   for (uint32_t i = 0; i < nm; i++) { have_fifo |= modes[i] == VK_PRESENT_MODE_FIFO_KHR; have_mb |= modes[i] == VK_PRESENT_MODE_MAILBOX_KHR; have_imm |= modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR; }
   CHECK(have_fifo, "FIFO missing");
   printf("caps: minImages %u maxImages %u, modes fifo=%d mailbox=%d immediate=%d\n", caps.minImageCount, caps.maxImageCount, have_fifo, have_mb, have_imm);

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   const char *dext[] = {"VK_KHR_swapchain", "VK_KHR_present_id", "VK_KHR_present_wait"};
   VkPhysicalDevicePresentWaitFeaturesKHR fw = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, .presentWait = VK_TRUE};
   VkPhysicalDevicePresentIdFeaturesKHR fid = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, .pNext = &fw, .presentId = VK_TRUE};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &fid, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .enabledExtensionCount = 3, .ppEnabledExtensionNames = dext};
   if (CreateDevice(pd, &di, NULL, &dev) != VK_SUCCESS) { printf("CreateDevice\n"); return 1; }
#define LOAD(n) n = (PFN_vk##n)GetDeviceProcAddr(dev, "vk" #n); if (!n) { printf("missing vk" #n "\n"); return 1; }
   DEV_FNS(LOAD)
   GetDeviceQueue(dev, 0, 0, &q);
   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   VkCommandBufferAllocateInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = LAT};
   for (int i = 0; i < LAT; i++) {
      CHECK(CreateCommandPool(dev, &pci, NULL, &pools[i]) == VK_SUCCESS, "pool");
      cbi.commandPool = pools[i]; cbi.commandBufferCount = 1;
      CHECK(AllocateCommandBuffers(dev, &cbi, &cbs[i]) == VK_SUCCESS, "cbs");
   }
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   for (int i = 0; i < LAT; i++) CHECK(CreateFence(dev, &fi, NULL, &fen[i]) == VK_SUCCESS, "fence");

   recreate((VkExtent2D){1280, 720}, VK_PRESENT_MODE_FIFO_KHR, 3, 1);
   unsigned long a = frames * 3 / 10, b = frames * 6 / 10, c = frames * 8 / 10, d = frames * 9 / 10;
   unsigned long recreates = 0, lost = 0, retries = 0, done = 0, fired = (unsigned long)-1;
   int consecutive = 0;
   for (unsigned long n = 0; n < frames;) {
      if (fired != n) {   /* scheduled phase changes fire once per frame number, not again on a retry */
         fired = n;
         if (n == a) { recreate((VkExtent2D){1920, 1080}, VK_PRESENT_MODE_FIFO_KHR, 3, 1); recreates++; }                /* resize */
         else if (n == b && have_mb) { recreate((VkExtent2D){1920, 1080}, VK_PRESENT_MODE_MAILBOX_KHR, 3, 1); recreates++; }
         else if (n == c && have_imm) { recreate((VkExtent2D){1600, 900}, VK_PRESENT_MODE_IMMEDIATE_KHR, 2, 0); recreates++; } /* no waitIdle */
         else if (n == d) { timeout_semantics(); recreate((VkExtent2D){1600, 900}, VK_PRESENT_MODE_FIFO_KHR, 3, 0); recreates++; }
      }
      int fr = frame(n);
      if (fr == 2) {   /* nothing was submitted: recreate and retry this frame; a driver that never recovers must fail, not count */
         lost++; retries++;
         if (++consecutive > 8) { CHECK(0, "frame %lu: %d consecutive OUT_OF_DATE acquires after recreation", n, consecutive); break; }
         recreate(ext, VK_PRESENT_MODE_FIFO_KHR, 3, 1);
         continue;
      }
      consecutive = 0;
      done++; n++;
      if (fr == 1) { lost++; recreate(ext, VK_PRESENT_MODE_FIFO_KHR, 3, 1); }
   }
   CHECK(done == frames, "only %lu of %lu frames were submitted and presented", done, frames);
   drain_fences();
   STEP("teardown");
   CHECK(DeviceWaitIdle(dev) == VK_SUCCESS, "final waitidle");
   destroy_sync();
   DestroySwapchainKHR(dev, sc, NULL);
   for (int i = 0; i < LAT; i++) DestroyFence(dev, fen[i], NULL);
   for (int i = 0; i < LAT; i++) DestroyCommandPool(dev, pools[i], NULL);
   DestroyDevice(dev, NULL);
   DestroySurfaceKHR(inst, surf, NULL);
   DestroyInstance(inst, NULL);
   printf("%s: %lu frames, %lu recreates, %lu forced recreates (%lu retried)\n", g_fail ? "FAIL" : "PASS", done, recreates, lost, retries);
   return g_fail;
}
