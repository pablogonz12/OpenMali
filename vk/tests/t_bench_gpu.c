/* SPDX-License-Identifier: MIT */
/* Uncapped GPU fill benchmark: a fullscreen triangle with a 12-octave noise fragment shader,
 * rendered into an offscreen (headless) colour target. There is no surface and no swapchain,
 * so no presentation path can cap it: the number is the chip, not the panel.
 *
 *   t_bench_gpu [--seconds N] [--width W] [--height H] [--samples 1|2|4|8] [--layers L]
 *               [--warmup N] [--device I] [--spv-dir DIR] [--loader LIB]
 *
 * Output is key=value, one per line, for tools/bench/suite.sh:
 *   render_fps=<frames/wall seconds>  resolution=WxH  samples=N  frames=N ...
 * Same shader and geometry every frame: deterministic and re-runnable.
 * Loader client: dlopens libvulkan.so.1 unless --loader is given, so the ICD is whichever
 * one VK_ICD_FILENAMES/VK_DRIVER_FILES selects (the guest ICD the lanes deploy).
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static VkInstance inst;
static VkDevice dev;
static VkPhysicalDevice pdev;
static VkQueue queue;
static uint32_t qfam;
static VkPhysicalDeviceMemoryProperties memprops;
static VkPhysicalDeviceProperties props;

static int width = 1920, height = 1080, layers = 1, samples = 1, warmup = 10;
static double seconds = 5.0;
static int device_index;
static const char *spv_dir = "shader";
static const char *loader_name;

#define INST_FNS(X)                                                                                \
   X(CreateInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties)                   \
   X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties)                 \
   X(CreateDevice) X(GetDeviceProcAddr)
#define DEV_FNS(X)                                                                                 \
   X(DestroyDevice) X(GetDeviceQueue) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) \
   X(AllocateMemory) X(FreeMemory) X(BindImageMemory) X(CreateImageView) X(DestroyImageView)       \
   X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer)             \
   X(CreateShaderModule) X(DestroyShaderModule) X(CreatePipelineLayout) X(DestroyPipelineLayout)   \
   X(CreateGraphicsPipelines) X(DestroyPipeline) X(CreateCommandPool) X(DestroyCommandPool)        \
   X(ResetCommandPool) X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer)       \
   X(EndCommandBuffer) X(CmdBeginRenderPass) X(CmdBindPipeline) X(CmdDraw) X(CmdEndRenderPass)     \
   X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(QueueSubmit)                   \
   X(QueueWaitIdle) X(DeviceWaitIdle)

#define DECL(n) static PFN_vk##n n;
INST_FNS(DECL)
DEV_FNS(DECL)

static PFN_vkGetInstanceProcAddr gipa;

static void
die(const char *what, VkResult r)
{
   printf("error=%s (%d)\n", what, (int)r);
   exit(1);
}

static double
now_us(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

static VkShaderModule
load_spv(const char *name)
{
   char path[1024];
   snprintf(path, sizeof path, "%s/%s", spv_dir, name);
   FILE *f = fopen(path, "rb");
   if (!f) {
      printf("error=cannot-open %s\n", path);
      exit(1);
   }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (n <= 0 || (n & 3)) {
      printf("error=bad-spirv-size %s\n", path);
      exit(1);
   }
   uint32_t *code = malloc((size_t)n);
   if (!code || fread(code, 1, (size_t)n, f) != (size_t)n) {
      printf("error=read-spirv %s\n", path);
      exit(1);
   }
   fclose(f);
   VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                  .codeSize = (size_t)n,
                                  .pCode = code};
   VkShaderModule m = VK_NULL_HANDLE;
   VkResult r = CreateShaderModule(dev, &ci, NULL, &m);
   free(code);
   if (r != VK_SUCCESS)
      die("vkCreateShaderModule", r);
   return m;
}

static uint32_t
mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
         return i;
   printf("error=no-memory-type\n");
   exit(1);
}

/* One colour image plus (for samples > 1) a single-sample resolve image, both offscreen. */
typedef struct {
   VkImage color, resolve;
   VkDeviceMemory color_mem, resolve_mem;
   VkImageView color_view, resolve_view;
   VkFramebuffer fb;
   VkRenderPass rp;
} Target;

static VkImage
make_image(VkFormat fmt, VkSampleCountFlagBits s, VkImageUsageFlags usage, VkDeviceMemory *mem)
{
   VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                           .imageType = VK_IMAGE_TYPE_2D,
                           .format = fmt,
                           .extent = {(uint32_t)width, (uint32_t)height, 1},
                           .mipLevels = 1,
                           .arrayLayers = 1,
                           .samples = s,
                           .tiling = VK_IMAGE_TILING_OPTIMAL,
                           .usage = usage,
                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VkImage img = VK_NULL_HANDLE;
   VkResult r = CreateImage(dev, &ci, NULL, &img);
   if (r != VK_SUCCESS)
      die("vkCreateImage", r);
   VkMemoryRequirements req;
   GetImageMemoryRequirements(dev, img, &req);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = req.size,
                              .memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
   r = AllocateMemory(dev, &ai, NULL, mem);
   if (r != VK_SUCCESS)
      die("vkAllocateMemory", r);
   r = BindImageMemory(dev, img, *mem, 0);
   if (r != VK_SUCCESS)
      die("vkBindImageMemory", r);
   return img;
}

static VkImageView
make_view(VkImage img, VkFormat fmt)
{
   VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                               .image = img,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D,
                               .format = fmt,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkImageView v = VK_NULL_HANDLE;
   VkResult r = CreateImageView(dev, &ci, NULL, &v);
   if (r != VK_SUCCESS)
      die("vkCreateImageView", r);
   return v;
}

static void
make_target(Target *t)
{
   const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
   const VkSampleCountFlagBits s = (VkSampleCountFlagBits)samples;
   t->color = make_image(fmt, s, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &t->color_mem);
   t->color_view = make_view(t->color, fmt);
   VkImageView views[2];
   uint32_t nviews = 1;
   views[0] = t->color_view;
   if (samples > 1) {
      t->resolve = make_image(fmt, VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &t->resolve_mem);
      t->resolve_view = make_view(t->resolve, fmt);
      views[nviews++] = t->resolve_view;
   }

   VkAttachmentDescription att[2] = {
      {.format = fmt, .samples = s, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
       .storeOp = samples > 1 ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
       .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
       .finalLayout = samples > 1 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
      {.format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
       .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
   };
   VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkAttachmentReference resolve_ref = {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                               .colorAttachmentCount = 1,
                               .pColorAttachments = &ref};
   if (samples > 1)
      sub.pResolveAttachments = &resolve_ref;
   VkSubpassDependency dep = {.srcSubpass = VK_SUBPASS_EXTERNAL,
                              .dstSubpass = 0,
                              .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                              .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
   VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                  .attachmentCount = samples > 1 ? 2u : 1u,
                                  .pAttachments = att,
                                  .subpassCount = 1,
                                  .pSubpasses = &sub,
                                  .dependencyCount = 1,
                                  .pDependencies = &dep};
   VkResult r = CreateRenderPass(dev, &rpci, NULL, &t->rp);
   if (r != VK_SUCCESS)
      die("vkCreateRenderPass", r);

   VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                  .renderPass = t->rp,
                                  .attachmentCount = nviews,
                                  .pAttachments = views,
                                  .width = (uint32_t)width,
                                  .height = (uint32_t)height,
                                  .layers = 1};
   r = CreateFramebuffer(dev, &fci, NULL, &t->fb);
   if (r != VK_SUCCESS)
      die("vkCreateFramebuffer", r);
}

static VkPipeline
make_pipeline(VkRenderPass rp)
{
   VkShaderModule vs = load_spv("bench_gpu.vert.spv");
   VkShaderModule fs = load_spv("bench_gpu.frag.spv");
   VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"},
   };
   VkPipelineVertexInputStateCreateInfo vin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport vp = {0, 0, (float)width, (float)height, 0, 1};
   VkRect2D sc = {{0, 0}, {(uint32_t)width, (uint32_t)height}};
   VkPipelineViewportStateCreateInfo vstate = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                               .viewportCount = 1, .pViewports = &vp,
                                               .scissorCount = 1, .pScissors = &sc};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                .polygonMode = VK_POLYGON_MODE_FILL,
                                                .cullMode = VK_CULL_MODE_NONE,
                                                .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                              .rasterizationSamples = (VkSampleCountFlagBits)samples,
                                              .sampleShadingEnable = VK_FALSE};
   VkPipelineColorBlendAttachmentState cba = {.blendEnable = VK_FALSE,
                                             .colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                             .attachmentCount = 1, .pAttachments = &cba};
   VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkResult r = CreatePipelineLayout(dev, &lci, NULL, &layout);
   if (r != VK_SUCCESS)
      die("vkCreatePipelineLayout", r);
   VkGraphicsPipelineCreateInfo gci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .stageCount = 2, .pStages = stages,
                                       .pVertexInputState = &vin,
                                       .pInputAssemblyState = &ia,
                                       .pViewportState = &vstate,
                                       .pRasterizationState = &rs,
                                       .pMultisampleState = &ms,
                                       .pColorBlendState = &cb,
                                       .layout = layout,
                                       .renderPass = rp,
                                       .subpass = 0};
   VkPipeline pipe = VK_NULL_HANDLE;
   r = CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe);
   if (r != VK_SUCCESS)
      die("vkCreateGraphicsPipelines", r);
   DestroyShaderModule(dev, vs, NULL);
   DestroyShaderModule(dev, fs, NULL);
   DestroyPipelineLayout(dev, layout, NULL);
   return pipe;
}

static void
usage(void)
{
   printf("usage: t_bench_gpu [--seconds N] [--width W] [--height H] [--samples 1|2|4|8]\n"
          "                    [--layers L] [--warmup N] [--device I] [--spv-dir DIR] [--loader LIB]\n");
   exit(2);
}

static void
parse(int argc, char **argv)
{
   for (int i = 1; i < argc; i++) {
      const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (usage(), (char *)0))
      if (!strcmp(a, "--seconds")) seconds = atof(NEXT());
      else if (!strcmp(a, "--width")) width = atoi(NEXT());
      else if (!strcmp(a, "--height")) height = atoi(NEXT());
      else if (!strcmp(a, "--samples")) samples = atoi(NEXT());
      else if (!strcmp(a, "--layers")) layers = atoi(NEXT());
      else if (!strcmp(a, "--warmup")) warmup = atoi(NEXT());
      else if (!strcmp(a, "--device")) device_index = atoi(NEXT());
      else if (!strcmp(a, "--spv-dir")) spv_dir = NEXT();
      else if (!strcmp(a, "--loader")) loader_name = NEXT();
      else usage();
#undef NEXT
   }
   if (width < 16 || height < 16 || width > 16384 || height > 16384) usage();
   if (samples != 1 && samples != 2 && samples != 4 && samples != 8) usage();
   if (layers < 1 || layers > 1024) usage();
   if (seconds <= 0 || seconds > 600) usage();
}

int
main(int argc, char **argv)
{
   parse(argc, argv);

   const char *libname = loader_name ? loader_name : "libvulkan.so.1";
   void *lib = dlopen(libname, RTLD_NOW | RTLD_LOCAL);
   if (!lib && !loader_name) {
      libname = "libvulkan.so";
      lib = dlopen(libname, RTLD_NOW | RTLD_LOCAL);
   }
   if (!lib) {
      printf("error=no-loader %s: %s\n", libname, dlerror());
      return 1;
   }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
   if (!gipa) {
      printf("error=no-vkGetInstanceProcAddr\n");
      return 1;
   }

#define LOAD_I(n)                                                                                  \
   do {                                                                                            \
      n = (PFN_vk##n)gipa(NULL, "vk" #n);                                                          \
      if (!n) { printf("error=no-instance-fn vk%s\n", #n); return 1; }                             \
   } while (0)
   LOAD_I(CreateInstance);
#undef LOAD_I

   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pApplicationName = "t_bench_gpu",
                            .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app};
   VkResult r = CreateInstance(&ici, NULL, &inst);
   if (r != VK_SUCCESS)
      die("vkCreateInstance", r);

#define LOAD(n)                                                                                    \
   do {                                                                                            \
      n = (PFN_vk##n)gipa(inst, "vk" #n);                                                           \
      if (!n) { printf("error=no-instance-fn vk%s\n", #n); return 1; }                              \
   } while (0)
   LOAD(EnumeratePhysicalDevices);
   LOAD(GetPhysicalDeviceProperties);
   LOAD(GetPhysicalDeviceMemoryProperties);
   LOAD(GetPhysicalDeviceQueueFamilyProperties);
   LOAD(CreateDevice);
   LOAD(GetDeviceProcAddr);
#undef LOAD

   uint32_t ndev = 0;
   r = EnumeratePhysicalDevices(inst, &ndev, NULL);
   if (r != VK_SUCCESS || ndev == 0) {
      printf("error=no-physical-device (%d)\n", (int)r);
      return 1;
   }
   VkPhysicalDevice *devs = calloc(ndev, sizeof *devs);
   EnumeratePhysicalDevices(inst, &ndev, devs);
   if ((uint32_t)device_index >= ndev) {
      printf("error=device-index %d >= %u\n", device_index, ndev);
      return 1;
   }
   pdev = devs[device_index];
   GetPhysicalDeviceProperties(pdev, &props);
   GetPhysicalDeviceMemoryProperties(pdev, &memprops);
   uint32_t ns = (uint32_t)props.limits.framebufferColorSampleCounts;
   if (!(ns & (uint32_t)samples)) {
      printf("error=samples-%d-unsupported (mask 0x%x)\n", samples, ns);
      return 1;
   }
   uint32_t nq = 0;
   GetPhysicalDeviceQueueFamilyProperties(pdev, &nq, NULL);
   VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
   GetPhysicalDeviceQueueFamilyProperties(pdev, &nq, qp);
   int found = 0;
   for (uint32_t i = 0; i < nq; i++)
      if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && !found) {
         qfam = i;
         found = 1;
      }
   free(qp);
   if (!found) {
      printf("error=no-graphics-queue\n");
      return 1;
   }

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
   r = CreateDevice(pdev, &dci, NULL, &dev);
   if (r != VK_SUCCESS)
      die("vkCreateDevice", r);

#define LOAD_D(n)                                                                                  \
   do {                                                                                            \
      n = (PFN_vk##n)GetDeviceProcAddr(dev, "vk" #n);                                              \
      if (!n) { printf("error=no-device-fn vk%s\n", #n); return 1; }                               \
   } while (0);
   DEV_FNS(LOAD_D)
#undef LOAD_D
   GetDeviceQueue(dev, qfam, 0, &queue);

   Target t = {0};
   make_target(&t);
   VkPipeline pipe = make_pipeline(t.rp);

   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                  .queueFamilyIndex = qfam};
   VkCommandPool pool = VK_NULL_HANDLE;
   r = CreateCommandPool(dev, &pci, NULL, &pool);
   if (r != VK_SUCCESS)
      die("vkCreateCommandPool", r);
   VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
   VkCommandBuffer cb = VK_NULL_HANDLE;
   r = AllocateCommandBuffers(dev, &cbai, &cb);
   if (r != VK_SUCCESS)
      die("vkAllocateCommandBuffers", r);

   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                  .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
   r = BeginCommandBuffer(cb, &bi);
   if (r != VK_SUCCESS)
      die("vkBeginCommandBuffer", r);
   VkClearValue clear = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
   VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                .renderPass = t.rp,
                                .framebuffer = t.fb,
                                .renderArea = {{0, 0}, {(uint32_t)width, (uint32_t)height}},
                                .clearValueCount = 1,
                                .pClearValues = &clear};
   CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   for (int i = 0; i < layers; i++)
      CmdDraw(cb, 3, 1, 0, 0);
   CmdEndRenderPass(cb);
   r = EndCommandBuffer(cb);
   if (r != VK_SUCCESS)
      die("vkEndCommandBuffer", r);

   VkFence fences[2];
   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   for (int i = 0; i < 2; i++) {
      r = CreateFence(dev, &fci, NULL, &fences[i]);
      if (r != VK_SUCCESS)
         die("vkCreateFence", r);
   }

   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                      .commandBufferCount = 1, .pCommandBuffers = &cb};

   unsigned char submitted[2] = {0, 0};     /* slots that really received a submission in this phase */
   for (int i = 0; i < warmup; i++) {
      int s = i & 1;
      if (i >= 2) {
         r = WaitForFences(dev, 1, &fences[s], VK_TRUE, UINT64_MAX);
         if (r != VK_SUCCESS)
            die("vkWaitForFences (warmup slot reuse)", r);
         ResetFences(dev, 1, &fences[s]);
      }
      r = QueueSubmit(queue, 1, &si, fences[s]);   /* a failed submit has no fence to wait on */
      if (r != VK_SUCCESS)
         die("vkQueueSubmit (warmup)", r);
      submitted[s] = 1;
   }
   for (int i = 0; i < 2; i++) {
      if (!submitted[i])
         continue;
      r = WaitForFences(dev, 1, &fences[i], VK_TRUE, UINT64_MAX);
      if (r != VK_SUCCESS)
         die("vkWaitForFences (warmup drain)", r);
      ResetFences(dev, 1, &fences[i]);
   }

   double submit_us = 0;
   unsigned long frames = 0;
   double t0 = now_us();
   int slot = 0;
   for (;;) {
      if (frames >= 2) {
         r = WaitForFences(dev, 1, &fences[slot], VK_TRUE, UINT64_MAX);
         if (r != VK_SUCCESS)
            die("vkWaitForFences (slot reuse)", r);
         ResetFences(dev, 1, &fences[slot]);
      }
      double a = now_us();
      r = QueueSubmit(queue, 1, &si, fences[slot]);
      if (r != VK_SUCCESS)
         die("vkQueueSubmit", r);
      submit_us += now_us() - a;
      frames++;
      slot ^= 1;
      if ((now_us() - t0) / 1e6 >= seconds)
         break;
   }
   for (int i = 0; i < 2; i++) {
      if (!submitted[i])
         continue;
      r = WaitForFences(dev, 1, &fences[i], VK_TRUE, UINT64_MAX);
      if (r != VK_SUCCESS)                    /* device lost after the last submit: the FPS would be a fiction */
         die("vkWaitForFences (final drain)", r);
   }
   double wall = (now_us() - t0) / 1e6;

   const char *icd = getenv("VK_DRIVER_FILES");
   if (!icd)
      icd = getenv("VK_ICD_FILENAMES");

   printf("device=%s\n", props.deviceName);
   printf("driver_version=0x%08x\n", props.driverVersion);
   printf("device_api=%u.%u.%u\n", VK_API_VERSION_MAJOR(props.apiVersion),
          VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
   printf("icd=%s\n", icd ? icd : "default");
   printf("resolution=%dx%d\n", width, height);
   printf("samples=%d\n", samples);
   printf("layers=%d\n", layers);
   printf("vsync=none\n");
   printf("present_path=none\n");
   printf("warmup_frames=%d\n", warmup);
   printf("frames=%lu\n", frames);
   printf("seconds=%.3f\n", wall);
   printf("render_fps=%.3f\n", frames ? (double)frames / wall : 0.0);
   printf("frame_ms=%.3f\n", frames ? wall * 1e3 / (double)frames : 0.0);
   printf("cpu_ms_per_frame=%.4f\n", frames ? submit_us / (double)frames / 1e3 : 0.0);

   DeviceWaitIdle(dev);
   for (int i = 0; i < 2; i++)
      DestroyFence(dev, fences[i], NULL);
   DestroyCommandPool(dev, pool, NULL);
   DestroyPipeline(dev, pipe, NULL);
   DestroyFramebuffer(dev, t.fb, NULL);
   DestroyRenderPass(dev, t.rp, NULL);
   DestroyImageView(dev, t.color_view, NULL);
   DestroyImage(dev, t.color, NULL);
   FreeMemory(dev, t.color_mem, NULL);
   if (samples > 1) {
      DestroyImageView(dev, t.resolve_view, NULL);
      DestroyImage(dev, t.resolve, NULL);
      FreeMemory(dev, t.resolve_mem, NULL);
   }
   DestroyDevice(dev, NULL);
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
   {
      PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
      if (di)
         di(inst, NULL);
   }
   dlclose(lib);
   free(devs);
   return 0;
}
