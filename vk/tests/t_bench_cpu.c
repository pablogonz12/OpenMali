/* SPDX-License-Identifier: MIT */
/* Uncapped CPU draw-submission benchmark: N tiny draws per frame into a 64x64 offscreen
 * target, each with its own dynamic uniform offset. Fill and ALU are negligible, so the
 * limit is command recording, descriptor/queue submission and the driver's per-draw path.
 *
 *   t_bench_cpu [--seconds N] [--draws N] [--size WxH] [--warmup N]
 *               [--device I] [--spv-dir DIR] [--loader LIB]
 *
 * Output is key=value, one per line, for tools/bench/suite.sh:
 *   render_fps=<frames/wall seconds>  cpu_ms_per_frame=...  record_ms=...  submit_ms=...
 * No surface and no swapchain: there is no presentation cap to mistake for the chip.
 * Loader client: dlopens libvulkan.so.1 unless --loader is given, so the ICD is whichever
 * VK_ICD_FILENAMES/VK_DRIVER_FILES selects (the guest ICD the lanes deploy).
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NSLOTS 256

static VkInstance inst;
static VkDevice dev;
static VkPhysicalDevice pdev;
static VkQueue queue;
static uint32_t qfam;
static VkPhysicalDeviceMemoryProperties memprops;
static VkPhysicalDeviceProperties props;

static int width = 64, height = 64;
static long draws = 50000;
static int warmup = 5;
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
   X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory)             \
   X(MapMemory) X(UnmapMemory) X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer)       \
   X(DestroyFramebuffer) X(CreateShaderModule) X(DestroyShaderModule)                             \
   X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool)              \
   X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets)                      \
   X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateGraphicsPipelines) X(DestroyPipeline)  \
   X(CreateCommandPool) X(DestroyCommandPool) X(ResetCommandPool) X(AllocateCommandBuffers)        \
   X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBeginRenderPass)           \
   X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdDraw) X(CmdEndRenderPass) X(CreateFence)       \
   X(DestroyFence) X(WaitForFences) X(ResetFences) X(QueueSubmit) X(DeviceWaitIdle)

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

typedef struct {
   VkImage color;
   VkDeviceMemory color_mem;
   VkImageView view;
   VkFramebuffer fb;
   VkRenderPass rp;
} Target;

static void
make_target(Target *t)
{
   const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
   VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = fmt,
                            .extent = {(uint32_t)width, (uint32_t)height, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VkResult r = CreateImage(dev, &ici, NULL, &t->color);
   if (r != VK_SUCCESS)
      die("vkCreateImage", r);
   VkMemoryRequirements req;
   GetImageMemoryRequirements(dev, t->color, &req);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = req.size,
                              .memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
   r = AllocateMemory(dev, &ai, NULL, &t->color_mem);
   if (r != VK_SUCCESS)
      die("vkAllocateMemory", r);
   BindImageMemory(dev, t->color, t->color_mem, 0);

   VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = t->color,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   r = CreateImageView(dev, &vci, NULL, &t->view);
   if (r != VK_SUCCESS)
      die("vkCreateImageView", r);

   VkAttachmentDescription att = {.format = fmt,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                  .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                  .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                  .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                  .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                               .colorAttachmentCount = 1,
                               .pColorAttachments = &ref};
   VkSubpassDependency dep = {.srcSubpass = VK_SUBPASS_EXTERNAL,
                              .dstSubpass = 0,
                              .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
   VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                  .attachmentCount = 1, .pAttachments = &att,
                                  .subpassCount = 1, .pSubpasses = &sub,
                                  .dependencyCount = 1, .pDependencies = &dep};
   r = CreateRenderPass(dev, &rpci, NULL, &t->rp);
   if (r != VK_SUCCESS)
      die("vkCreateRenderPass", r);

   VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                  .renderPass = t->rp, .attachmentCount = 1, .pAttachments = &t->view,
                                  .width = (uint32_t)width, .height = (uint32_t)height, .layers = 1};
   r = CreateFramebuffer(dev, &fci, NULL, &t->fb);
   if (r != VK_SUCCESS)
      die("vkCreateFramebuffer", r);
}

static void
usage(void)
{
   printf("usage: t_bench_cpu [--seconds N] [--draws N] [--size WxH] [--warmup N]\n"
          "                    [--device I] [--spv-dir DIR] [--loader LIB]\n");
   exit(2);
}

static void
parse(int argc, char **argv)
{
   for (int i = 1; i < argc; i++) {
      const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (usage(), (char *)0))
      if (!strcmp(a, "--seconds")) seconds = atof(NEXT());
      else if (!strcmp(a, "--draws")) draws = atol(NEXT());
      else if (!strcmp(a, "--size")) {
         if (sscanf(NEXT(), "%dx%d", &width, &height) != 2)
            usage();
      } else if (!strcmp(a, "--warmup")) warmup = atoi(NEXT());
      else if (!strcmp(a, "--device")) device_index = atoi(NEXT());
      else if (!strcmp(a, "--spv-dir")) spv_dir = NEXT();
      else if (!strcmp(a, "--loader")) loader_name = NEXT();
      else usage();
#undef NEXT
   }
   if (width < 16 || height < 16 || width > 4096 || height > 4096) usage();
   if (draws < 100 || draws > 200000) usage();
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
   CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   if (!CreateInstance) {
      printf("error=no-instance-fn vkCreateInstance\n");
      return 1;
   }

   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pApplicationName = "t_bench_cpu",
                            .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
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

   uint32_t align = props.limits.minUniformBufferOffsetAlignment ? (uint32_t)props.limits.minUniformBufferOffsetAlignment : 1;
   uint32_t stride = (16u + align - 1u) / align * align;
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = (VkDeviceSize)stride * NSLOTS,
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer ubo = VK_NULL_HANDLE;
   r = CreateBuffer(dev, &bci, NULL, &ubo);
   if (r != VK_SUCCESS)
      die("vkCreateBuffer", r);
   VkMemoryRequirements breq;
   GetBufferMemoryRequirements(dev, ubo, &breq);
   VkMemoryAllocateInfo bai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = breq.size,
                               .memoryTypeIndex = mem_type(breq.memoryTypeBits,
                                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
   VkDeviceMemory ubo_mem = VK_NULL_HANDLE;
   r = AllocateMemory(dev, &bai, NULL, &ubo_mem);
   if (r != VK_SUCCESS)
      die("vkAllocateMemory", r);
   BindBufferMemory(dev, ubo, ubo_mem, 0);
   void *map = NULL;
   r = MapMemory(dev, ubo_mem, 0, VK_WHOLE_SIZE, 0, &map);
   if (r != VK_SUCCESS)
      die("vkMapMemory", r);
   for (uint32_t i = 0; i < NSLOTS; i++) {
      float v[4] = {(float)(i & 7), (float)((i >> 3) & 7), 0.0f, 0.0f};
      memcpy((char *)map + (size_t)i * stride, v, sizeof v);
   }
   UnmapMemory(dev, ubo_mem);

   VkDescriptorSetLayoutBinding bind = {.binding = 0,
                                        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                        .descriptorCount = 1,
                                        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT};
   VkDescriptorSetLayoutCreateInfo dsli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 1, .pBindings = &bind};
   VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
   r = CreateDescriptorSetLayout(dev, &dsli, NULL, &dsl);
   if (r != VK_SUCCESS)
      die("vkCreateDescriptorSetLayout", r);
   VkDescriptorPoolSize psz = {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .descriptorCount = 1};
   VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &psz};
   VkDescriptorPool dpool = VK_NULL_HANDLE;
   r = CreateDescriptorPool(dev, &dpci, NULL, &dpool);
   if (r != VK_SUCCESS)
      die("vkCreateDescriptorPool", r);
   VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   VkDescriptorSet dset = VK_NULL_HANDLE;
   r = AllocateDescriptorSets(dev, &dsai, &dset);
   if (r != VK_SUCCESS)
      die("vkAllocateDescriptorSets", r);
   VkDescriptorBufferInfo dbi = {.buffer = ubo, .offset = 0, .range = 16};
   VkWriteDescriptorSet wds = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = dset, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                               .pBufferInfo = &dbi};
   UpdateDescriptorSets(dev, 1, &wds, 0, NULL);

   VkShaderModule vs = load_spv("bench_cpu.vert.spv");
   VkShaderModule fs = load_spv("bench_cpu.frag.spv");
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
                                                .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                              .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba = {.blendEnable = VK_FALSE, .colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                             .attachmentCount = 1, .pAttachments = &cba};
   VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1, .pSetLayouts = &dsl};
   VkPipelineLayout layout = VK_NULL_HANDLE;
   r = CreatePipelineLayout(dev, &plci, NULL, &layout);
   if (r != VK_SUCCESS)
      die("vkCreatePipelineLayout", r);
   VkGraphicsPipelineCreateInfo gci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .stageCount = 2, .pStages = stages,
                                       .pVertexInputState = &vin, .pInputAssemblyState = &ia,
                                       .pViewportState = &vstate, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pColorBlendState = &cb,
                                       .layout = layout, .renderPass = t.rp, .subpass = 0};
   VkPipeline pipe = VK_NULL_HANDLE;
   r = CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe);
   if (r != VK_SUCCESS)
      die("vkCreateGraphicsPipelines", r);
   DestroyShaderModule(dev, vs, NULL);
   DestroyShaderModule(dev, fs, NULL);

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
                                       .commandBufferCount = 2};
   VkCommandBuffer cbuf[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
   r = AllocateCommandBuffers(dev, &cbai, cbuf);
   if (r != VK_SUCCESS)
      die("vkAllocateCommandBuffers", r);
   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
   for (int i = 0; i < 2; i++) {
      r = CreateFence(dev, &fci, NULL, &fence[i]);
      if (r != VK_SUCCESS)
         die("vkCreateFence", r);
   }

   VkClearValue clear = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
   VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                .renderPass = t.rp, .framebuffer = t.fb,
                                .renderArea = {{0, 0}, {(uint32_t)width, (uint32_t)height}},
                                .clearValueCount = 1, .pClearValues = &clear};
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1};

   double rec_us = 0, sub_us = 0;
   unsigned long frames = 0;

   for (int warm_i = 0; warm_i < warmup; warm_i++) {
      int s = warm_i & 1;
      if (warm_i >= 2) {
         WaitForFences(dev, 1, &fence[s], VK_TRUE, UINT64_MAX);
         ResetFences(dev, 1, &fence[s]);
      }
      VkCommandBuffer cb = cbuf[s];
      BeginCommandBuffer(cb, &bi);
      CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      for (long i = 0; i < draws; i++) {
         uint32_t off = (uint32_t)(i % NSLOTS) * stride;
         CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &dset, 1, &off);
         CmdDraw(cb, 3, 1, 0, 0);
      }
      CmdEndRenderPass(cb);
      EndCommandBuffer(cb);
      si.pCommandBuffers = &cb;
      QueueSubmit(queue, 1, &si, fence[s]);
   }
   for (int i = 0; i < 2; i++) {
      WaitForFences(dev, 1, &fence[i], VK_TRUE, UINT64_MAX);
      ResetFences(dev, 1, &fence[i]);
   }

   double wall0 = now_us();
   for (;;) {
      int s = (int)(frames & 1);
      if (frames >= 2) {
         WaitForFences(dev, 1, &fence[s], VK_TRUE, UINT64_MAX);
         ResetFences(dev, 1, &fence[s]);
      }
      VkCommandBuffer cb = cbuf[s];
      double a = now_us();
      r = BeginCommandBuffer(cb, &bi);
      if (r != VK_SUCCESS)
         die("vkBeginCommandBuffer", r);
      CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      for (long i = 0; i < draws; i++) {
         uint32_t off = (uint32_t)(i % NSLOTS) * stride;
         CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &dset, 1, &off);
         CmdDraw(cb, 3, 1, 0, 0);
      }
      CmdEndRenderPass(cb);
      r = EndCommandBuffer(cb);
      if (r != VK_SUCCESS)
         die("vkEndCommandBuffer", r);
      double b = now_us();
      rec_us += b - a;
      si.pCommandBuffers = &cb;
      r = QueueSubmit(queue, 1, &si, fence[s]);
      if (r != VK_SUCCESS)
         die("vkQueueSubmit", r);
      sub_us += now_us() - b;
      frames++;
      if ((now_us() - wall0) / 1e6 >= seconds)
         break;
   }
   for (int i = 0; i < 2; i++)
      WaitForFences(dev, 1, &fence[i], VK_TRUE, UINT64_MAX);
   double wall = (now_us() - wall0) / 1e6;

   const char *icd = getenv("VK_DRIVER_FILES");
   if (!icd)
      icd = getenv("VK_ICD_FILENAMES");

   printf("device=%s\n", props.deviceName);
   printf("driver_version=0x%08x\n", props.driverVersion);
   printf("icd=%s\n", icd ? icd : "default");
   printf("resolution=%dx%d\n", width, height);
   printf("samples=1\n");
   printf("draws_per_frame=%ld\n", draws);
   printf("vsync=none\n");
   printf("present_path=none\n");
   printf("warmup_frames=%d\n", warmup);
   printf("frames=%lu\n", frames);
   printf("seconds=%.3f\n", wall);
   printf("render_fps=%.3f\n", frames ? (double)frames / wall : 0.0);
   printf("record_ms=%.4f\n", frames ? rec_us / (double)frames / 1e3 : 0.0);
   printf("submit_ms=%.4f\n", frames ? sub_us / (double)frames / 1e3 : 0.0);
   double cpu_ms = frames ? (rec_us + sub_us) / (double)frames / 1e3 : 0.0;
   double wall_ms = frames ? wall * 1e3 / (double)frames : 0.0;
   printf("frame_ms=%.4f\n", wall_ms);
   printf("cpu_ms_per_frame=%.4f\n", cpu_ms);
   printf("bound=%s\n", wall_ms > cpu_ms * 1.10 ? "gpu" : "cpu");

   DeviceWaitIdle(dev);
   for (int i = 0; i < 2; i++)
      DestroyFence(dev, fence[i], NULL);
   DestroyCommandPool(dev, pool, NULL);
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, layout, NULL);
   DestroyDescriptorPool(dev, dpool, NULL);
   DestroyDescriptorSetLayout(dev, dsl, NULL);
   DestroyBuffer(dev, ubo, NULL);
   FreeMemory(dev, ubo_mem, NULL);
   DestroyFramebuffer(dev, t.fb, NULL);
   DestroyRenderPass(dev, t.rp, NULL);
   DestroyImageView(dev, t.view, NULL);
   DestroyImage(dev, t.color, NULL);
   FreeMemory(dev, t.color_mem, NULL);
   DestroyDevice(dev, NULL);
   {
      PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
      if (di)
         di(inst, NULL);
   }
   dlclose(lib);
   free(devs);
   return 0;
}
