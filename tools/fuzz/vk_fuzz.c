/* SPDX-License-Identifier: MIT */
/* Host fuzz harness for the Mali ICD against the fake backend (no GPU, no phone).
 *
 *   vk_fuzz --icd <libvulkan_mali.so> --spv-dir <dir> [--mode spv|structs|all] [--seed S] [--iters N]
 *           [--calls C] [--timeout-s T] [--no-fork] [--trap-fatal]
 *
 * Every iteration runs in a forked child (seed = base seed + i), so a crash or hang is isolated and reproducible:
 *   vk_fuzz ... --seed <printed seed> --iters 1 --no-fork   (run in-process, e.g. under gdb)
 * Modes:
 *   spv     mutate the .spv files in --spv-dir (bit flips, word replacement, truncation, splice, insert/delete), feed them to
 *           vkCreateShaderModule + vkCreateComputePipelines (compute) and a graphics pipeline when the module is a fragment shader.
 *   structs perturb the SCALAR fields of Vulkan create-info structs (buffer, image, image view, sampler, render pass, descriptor
 *           set layout, pool, pipeline layout, graphics pipeline state, query pool) with boundary/random values. Pointers stay
 *           valid: the goal is driver robustness on bad values, not on bad addresses.
 * Calls with invalid usage may legitimately crash a driver (the spec gives no guarantee); triage each finding against the spec
 * before filing it. Needs MALI_BACKEND=fake (the harness refuses to run otherwise) and ideally the ASan ICD
 * (MALI_SAN=1 vk/check.sh builds it; see tools/fuzz/run.sh).
 */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

static uint64_t rs;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static uint32_t rn(uint32_t n) { return n ? rnd() % n : 0; }

/* A scalar value biased towards the interesting edges. */
static uint32_t
val32(uint32_t cur)
{
   static const uint32_t edge[] = {0, 1, 2, 3, 4, 7, 8, 15, 16, 17, 31, 32, 33, 64, 255, 256, 1023, 4096, 0x7fff, 0x8000, 0xffff,
                                   0x10000, 0x7fffffff, 0x80000000u, 0xfffffffeu, 0xffffffffu};
   switch (rn(6)) {
   case 0: return edge[rn(sizeof edge / sizeof *edge)];
   case 1: return cur + 1;
   case 2: return cur ? cur - 1 : 0xffffffffu;
   case 3: return rnd();
   case 4: return 1u << rn(32);
   default: return rn(20);
   }
}

/* Perturb ~1-3 random 32-bit words of a struct, skipping the first 16 bytes (sType + pNext) and any word listed in keep[]. */
static void
perturb(void *s, size_t size, const size_t *keep, int nkeep)
{
   uint8_t *b = s;
   for (int k = 1 + rn(3); k--;) {
      size_t off = 16 + 4 * rn((uint32_t)((size - 16) / 4));
      int skip = 0;
      for (int i = 0; i < nkeep; i++)
         if (off + 4 > keep[i] && off < keep[i] + 8) skip = 1;   /* pointer or counted field: leave alone */
      if (skip) continue;
      uint32_t v; memcpy(&v, b + off, 4); v = val32(v); memcpy(b + off, &v, 4);
   }
}
#define PERTURB(st, ...) do { size_t k_[] = {__VA_ARGS__}; perturb(&(st), sizeof(st), k_, sizeof k_ / sizeof *k_); } while (0)
#define PERTURB0(st) perturb(&(st), sizeof(st), NULL, 0)

static VkDevice dev;
#define DEV_FNS(X) \
   X(DestroyDevice) X(CreateBuffer) X(DestroyBuffer) X(CreateImage) X(DestroyImage) X(CreateImageView) X(DestroyImageView) \
   X(CreateSampler) X(DestroySampler) X(CreateRenderPass) X(DestroyRenderPass) X(CreateDescriptorSetLayout) \
   X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) X(CreatePipelineLayout) \
   X(DestroyPipelineLayout) X(CreateShaderModule) X(DestroyShaderModule) X(CreateComputePipelines) X(CreateGraphicsPipelines) \
   X(DestroyPipeline) X(CreateQueryPool) X(DestroyQueryPool) X(CreateFramebuffer) X(DestroyFramebuffer) \
   X(GetBufferMemoryRequirements) X(GetImageMemoryRequirements) X(CreateCommandPool) X(DestroyCommandPool) \
   X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdDispatch) X(CmdBindPipeline) X(DeviceWaitIdle) \
   X(CreateEvent) X(DestroyEvent) X(CreateFence) X(DestroyFence) X(CreateSemaphore) X(DestroySemaphore)
#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)
static PFN_vkGetPhysicalDeviceFormatProperties GFP;
static PFN_vkGetPhysicalDeviceImageFormatProperties GIFP;
static VkPhysicalDevice pd;

/* ---------- SPIR-V corpus + mutator ---------- */
struct spv { uint32_t *w; size_t n; };
static struct spv corpus[256];
static int ncorpus;

static void
load_corpus(const char *dir)
{
   DIR *d = opendir(dir);
   if (!d) { fprintf(stderr, "vk_fuzz: cannot open --spv-dir %s\n", dir); exit(2); }
   struct dirent *e;
   while ((e = readdir(d)) && ncorpus < 256) {
      size_t l = strlen(e->d_name);
      if (l < 5 || strcmp(e->d_name + l - 4, ".spv")) continue;
      char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
      FILE *f = fopen(p, "rb"); if (!f) continue;
      fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
      if (n >= 20 && n % 4 == 0) {
         uint32_t *w = malloc(n);
         if (fread(w, 1, n, f) == (size_t)n) { corpus[ncorpus].w = w; corpus[ncorpus].n = n / 4; ncorpus++; } else free(w);
      }
      fclose(f);
   }
   closedir(d);
   if (!ncorpus) { fprintf(stderr, "vk_fuzz: no .spv in %s\n", dir); exit(2); }
}

static uint32_t *
mutate(struct spv *o, size_t *n_out)
{
   *o = corpus[rn(ncorpus)];
   size_t n = o->n, cap = n + 64;
   uint32_t *w = malloc(cap * 4);
   memcpy(w, o->w, n * 4);
   for (int k = 1 + rn(4); k--;) {
      switch (rn(8)) {
      case 0: w[5 + rn(n - 5)] ^= 1u << rn(32); break;                        /* bit flip past the header */
      case 1: w[5 + rn(n - 5)] = val32(w[5]); break;                           /* word replace */
      case 2: if (n > 6) n -= 1 + rn(n > 16 ? 16 : n - 5); break;              /* truncate */
      case 3: w[rn(5)] = val32(0); break;                                      /* corrupt header (magic/version/bound/schema) */
      case 4: { size_t a = 5 + rn(n - 5), b = 5 + rn(n - 5); uint32_t t = w[a]; w[a] = w[b]; w[b] = t; break; }  /* swap */
      case 5: if (n + 1 < cap) { size_t a = 5 + rn(n - 5); memmove(w + a + 1, w + a, (n - a) * 4); w[a] = rnd(); n++; } break;
      case 6: if (n > 7) { size_t a = 5 + rn(n - 6); memmove(w + a, w + a + 1, (n - a - 1) * 4); n--; } break;
      default: { size_t a = 5 + rn(n - 5), l = 1 + rn(4); for (size_t i = 0; i < l && a + i < n; i++) w[a + i] = corpus[rn(ncorpus)].w[5 + rn(corpus[0].n - 5)]; }
      }
   }
   *n_out = n;
   return w;
}

static void
fuzz_spv(void)
{
   struct spv src; size_t n;
   uint32_t *w = mutate(&src, &n);
   VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n * 4, .pCode = w};
   VkShaderModule m = VK_NULL_HANDLE;
   if (CreateShaderModule(dev, &ci, NULL, &m) == VK_SUCCESS) {
      VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_ALL, .size = 16};
      if (rn(2)) { pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr; }
      VkPipelineLayout lay = VK_NULL_HANDLE;
      if (CreatePipelineLayout(dev, &pl, NULL, &lay) == VK_SUCCESS) {
         VkComputePipelineCreateInfo cp = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = lay,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}};
         VkPipeline p = VK_NULL_HANDLE;
         if (CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, NULL, &p) == VK_SUCCESS) DestroyPipeline(dev, p, NULL);
         DestroyPipelineLayout(dev, lay, NULL);
      }
      DestroyShaderModule(dev, m, NULL);
   }
   free(w);
}

/* ---------- create-info struct fuzzing ---------- */
static void
fuzz_structs(void)
{
   switch (rn(10)) {
   case 0: {
      VkBufferCreateInfo b = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
      PERTURB(b, offsetof(VkBufferCreateInfo, pQueueFamilyIndices));
      { static const uint32_t qfi0[1] = {0}; if (b.sharingMode == VK_SHARING_MODE_CONCURRENT) { b.queueFamilyIndexCount = 1; b.pQueueFamilyIndices = qfi0; } else b.pQueueFamilyIndices = NULL; }
      if (!b.size) b.size = 1;   /* valid-usage floor (runtime asserts size > 0) */
      VkBuffer h; if (CreateBuffer(dev, &b, NULL, &h) == VK_SUCCESS) { VkMemoryRequirements mr; GetBufferMemoryRequirements(dev, h, &mr); DestroyBuffer(dev, h, NULL); }
      break; }
   case 1: {
      VkImageCreateInfo i = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
      PERTURB(i, offsetof(VkImageCreateInfo, pQueueFamilyIndices));
      { static const uint32_t qfi0[1] = {0}; if (i.sharingMode == VK_SHARING_MODE_CONCURRENT) { i.queueFamilyIndexCount = 1; i.pQueueFamilyIndices = qfi0; } else i.pQueueFamilyIndices = NULL; }
      /* Valid-usage floor (the Mesa runtime asserts on these in a debug build; they are application bugs, not driver findings). */
      if (i.imageType > VK_IMAGE_TYPE_3D) i.imageType = rn(3);
      if (!i.samples || (i.samples & (i.samples - 1)) || i.samples > VK_SAMPLE_COUNT_64_BIT) i.samples = VK_SAMPLE_COUNT_1_BIT;
      /* Flags: sparse needs device features (not enabled), cube needs square 2D with >= 6 layers, 2D-array-compatible is 3D only. */
      i.flags &= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | (i.imageType == VK_IMAGE_TYPE_3D ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0);
      if (i.imageType != VK_IMAGE_TYPE_3D) i.extent.depth = 1;
      if (i.imageType == VK_IMAGE_TYPE_1D) i.extent.height = 1;
      if (i.imageType == VK_IMAGE_TYPE_3D) i.arrayLayers = 1;
      if (!i.mipLevels) i.mipLevels = 1;
      if (!i.arrayLayers) i.arrayLayers = 1;
      if (!i.extent.width) i.extent.width = 1;
      if (!i.extent.height) i.extent.height = 1;
      if (!i.extent.depth) i.extent.depth = 1;
      if (i.format > 1000 && rn(2)) i.format = rn(185);
      /* Spec: vkCreateImage with parameters the format/usage/tiling does not support is invalid usage; mirror what an app does and ask first. */
      VkImageFormatProperties ifp;
      if (GIFP && GIFP(pd, i.format, i.imageType, i.tiling, i.usage, i.flags, &ifp) != VK_SUCCESS && rn(8)) break;
      VkImage h;
      if (CreateImage(dev, &i, NULL, &h) == VK_SUCCESS) {
         VkMemoryRequirements mr; GetImageMemoryRequirements(dev, h, &mr);
         VkImageViewCreateInfo v = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = h, .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = i.format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
         /* Multi-planar formats have plane aspects; skip them (not covered by this harness). */
         int ds = i.format >= VK_FORMAT_D16_UNORM && i.format <= VK_FORMAT_D32_SFLOAT_S8_UINT;
         VkImageAspectFlags img_aspects = !ds ? VK_IMAGE_ASPECT_COLOR_BIT :
            i.format == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT :
            (i.format == VK_FORMAT_D16_UNORM || i.format == VK_FORMAT_X8_D24_UNORM_PACK32 || i.format == VK_FORMAT_D32_SFLOAT) ? VK_IMAGE_ASPECT_DEPTH_BIT :
            (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
         if (i.format >= 1000156000 && i.format < 1000157000) { DestroyImage(dev, h, NULL); break; }
         PERTURB(v, offsetof(VkImageViewCreateInfo, image));
         /* Valid-usage floor: the view type must exist and fit the image type; the aspect mask must be a non-empty subset of the image's. */
         static const VkImageViewType ok[3][3] = {{VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_1D},
                                                  {VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_2D},
                                                  {VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D}};
         if (v.viewType > VK_IMAGE_VIEW_TYPE_CUBE_ARRAY || (v.viewType != ok[i.imageType][0] && v.viewType != ok[i.imageType][1] && v.viewType != ok[i.imageType][2]))
            v.viewType = ok[i.imageType][rn(3)];
         v.subresourceRange.aspectMask &= img_aspects;
         if (!v.subresourceRange.aspectMask) v.subresourceRange.aspectMask = img_aspects;   /* the image handle is a 64-bit pointer: a perturbed half is a bad address, not a bad value */
         /* Valid-usage floor: the subresource range must lie inside the image. */
         VkImageSubresourceRange *sr = &v.subresourceRange;
         if (sr->baseMipLevel >= i.mipLevels) sr->baseMipLevel = 0;
         if (sr->levelCount != VK_REMAINING_MIP_LEVELS && (!sr->levelCount || sr->baseMipLevel + sr->levelCount > i.mipLevels)) sr->levelCount = 1;
         if (sr->baseArrayLayer >= i.arrayLayers) sr->baseArrayLayer = 0;
         if (sr->layerCount != VK_REMAINING_ARRAY_LAYERS && (!sr->layerCount || sr->baseArrayLayer + sr->layerCount > i.arrayLayers)) sr->layerCount = 1;
         if (!(i.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) v.format = i.format;   /* spec: view format must match unless MUTABLE_FORMAT (the Mesa runtime asserts) */
         VkImageView iv; if (CreateImageView(dev, &v, NULL, &iv) == VK_SUCCESS) DestroyImageView(dev, iv, NULL);
         DestroyImage(dev, h, NULL);
      }
      break; }
   case 2: {
      VkSamplerCreateInfo s = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
         .maxLod = 1.f, .maxAnisotropy = 1.f};
      PERTURB0(s);
      if (s.borderColor > VK_BORDER_COLOR_INT_OPAQUE_WHITE) s.borderColor = rn(6);   /* custom colours need an extension */
      VkSampler h; if (CreateSampler(dev, &s, NULL, &h) == VK_SUCCESS) DestroySampler(dev, h, NULL);
      break; }
   case 3: {
      VkAttachmentDescription a[2]; memset(a, 0, sizeof a);
      for (int i = 0; i < 2; i++) { a[i].format = VK_FORMAT_R8G8B8A8_UNORM; a[i].samples = VK_SAMPLE_COUNT_1_BIT; a[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
         a[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE; a[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; }
      for (int k = 1 + rn(3); k--;) { uint32_t *p = (uint32_t *)&a[rn(2)]; p[rn(sizeof(VkAttachmentDescription) / 4)] = val32(0); }
      for (int i = 0; i < 2; i++) if (!a[i].format || a[i].format > VK_FORMAT_ASTC_12x12_SRGB_BLOCK || (a[i].format >= VK_FORMAT_D16_UNORM && a[i].format <= VK_FORMAT_D32_SFLOAT_S8_UINT)) a[i].format = VK_FORMAT_R8G8B8A8_UNORM;   /* refs below are colour */
      uint32_t nat = 1 + rn(2);
      /* Valid-usage floor: every reference is an attachment index below attachmentCount or VK_ATTACHMENT_UNUSED. */
      VkAttachmentReference ref[2] = {{rn(2) ? rn(nat) : VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                      {rn(2) ? rn(nat) : VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
      VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = rn(3), .pColorAttachments = ref};
      VkSubpassDependency dep = {.srcSubpass = rn(3) ? 0 : VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = val32(0), .dstStageMask = val32(0)};
      VkRenderPassCreateInfo r = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = nat, .pAttachments = a,
         .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = rn(2), .pDependencies = &dep};
      VkRenderPass h; if (CreateRenderPass(dev, &r, NULL, &h) == VK_SUCCESS) DestroyRenderPass(dev, h, NULL);
      break; }
   case 4: {
      VkDescriptorSetLayoutBinding b[3]; memset(b, 0, sizeof b);
      for (int i = 0; i < 3; i++) { b[i].binding = i; b[i].descriptorType = rn(12); b[i].descriptorCount = rn(4); b[i].stageFlags = VK_SHADER_STAGE_ALL; }
      for (int k = rn(3); k--;) ((uint32_t *)&b[rn(3)])[rn(3)] = val32(0);
      VkDescriptorSetLayoutCreateInfo l = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = rn(4), .pBindings = b};
      VkDescriptorSetLayout h; if (CreateDescriptorSetLayout(dev, &l, NULL, &h) == VK_SUCCESS) DestroyDescriptorSetLayout(dev, h, NULL);
      break; }
   case 5: {
      VkDescriptorPoolSize ps[2] = {{rn(12), val32(1) & 0xffff}, {rn(12), val32(1) & 0xffff}};
      VkDescriptorPoolCreateInfo d = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = val32(1) & 0xffff, .poolSizeCount = 1 + rn(2), .pPoolSizes = ps};
      d.flags = rn(2) ? 0 : val32(0) & 3;
      VkDescriptorPool h; if (CreateDescriptorPool(dev, &d, NULL, &h) == VK_SUCCESS) DestroyDescriptorPool(dev, h, NULL);
      break; }
   case 6: {
      VkPushConstantRange pc[2] = {{VK_SHADER_STAGE_ALL, val32(0), val32(4)}, {VK_SHADER_STAGE_VERTEX_BIT, val32(0), val32(4)}};
      VkPipelineLayoutCreateInfo p = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = rn(3), .pPushConstantRanges = pc};
      VkPipelineLayout h; if (CreatePipelineLayout(dev, &p, NULL, &h) == VK_SUCCESS) DestroyPipelineLayout(dev, h, NULL);
      break; }
   case 7: {
      VkQueryPoolCreateInfo q = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 8};
      PERTURB0(q);
      VkQueryPool h; if (CreateQueryPool(dev, &q, NULL, &h) == VK_SUCCESS) DestroyQueryPool(dev, h, NULL);
      break; }
   case 8: {
      VkCommandPoolCreateInfo c = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      PERTURB0(c);
      VkCommandPool h;
      if (CreateCommandPool(dev, &c, NULL, &h) == VK_SUCCESS) {
         VkCommandBufferAllocateInfo a = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = h, .level = rn(3) ? 0 : 1, .commandBufferCount = 1};
         VkCommandBuffer cb;
         if (AllocateCommandBuffers(dev, &a, &cb) == VK_SUCCESS) {
            VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = rn(8)};
            if (BeginCommandBuffer(cb, &bi) == VK_SUCCESS) { if (rn(2)) CmdDispatch(cb, val32(1) & 0xffff, val32(1) & 0xffff, val32(1) & 0xffff); EndCommandBuffer(cb); }
         }
         DestroyCommandPool(dev, h, NULL);
      }
      break; }
   default: {
      VkEventCreateInfo e = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO}; PERTURB0(e);
      VkEvent h; if (CreateEvent(dev, &e, NULL, &h) == VK_SUCCESS) DestroyEvent(dev, h, NULL);
      VkFenceCreateInfo f = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; PERTURB0(f);
      VkFence fh; if (CreateFence(dev, &f, NULL, &fh) == VK_SUCCESS) DestroyFence(dev, fh, NULL);
      break; }
   }
}

/* ---------- driver setup + one iteration ---------- */
static const char *g_icd;
static int
setup(void)
{
   void *lib = dlopen(g_icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 0; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   VkResult (*neg)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   uint32_t iv = 5;
   if (!gipa || !neg || neg(&iv)) return 0;
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst;
   if (((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst)) return 0;
   uint32_t cnt = 1;
   ((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &cnt, &pd);
   if (!cnt) return 0;
   GFP = (PFN_vkGetPhysicalDeviceFormatProperties)gipa(inst, "vkGetPhysicalDeviceFormatProperties");
   GIFP = (PFN_vkGetPhysicalDeviceImageFormatProperties)gipa(inst, "vkGetPhysicalDeviceImageFormatProperties");
   float prio = 1.f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
   if (((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(pd, &di, NULL, &dev)) return 0;
   PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
#define LOAD(n) n = (PFN_vk##n)gdpa(dev, "vk" #n); if (!n) { fprintf(stderr, "missing vk" #n "\n"); return 0; }
   DEV_FNS(LOAD)
   return 1;
}

static int g_mode; /* 1 spv, 2 structs, 3 all */
/* A debug (non-NDEBUG) Mesa build executes debug_break() (int3 -> SIGTRAP) in vtn_fail() after logging "SPIR-V parsing FAILED" and
 * before it longjmps back out; a release build just returns NULL. That is Mesa reporting invalid SPIR-V correctly, not a driver bug, so
 * by default the harness lets the int3 continue. --trap-fatal restores the crash. */
static void on_trap(int sig) { (void)sig; }
static void
iteration(uint64_t seed, int calls)
{
   rs = seed * 0x9E3779B97F4A7C15ull + 0x1234567; if (!rs) rs = 1;
   for (int i = 0; i < 8; i++) rnd();
   for (int c = 0; c < calls; c++) {
      int m = g_mode == 3 ? (int)(1 + rn(2)) : g_mode;
      if (m == 1) fuzz_spv(); else fuzz_structs();
   }
}

int
main(int argc, char **argv)
{
   const char *spvdir = NULL, *mode = "all";
   uint64_t seed = 1; long iters = 1000; int tmo = 20, nofork = 0, calls = 20, trapfatal = 0;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--icd") && i + 1 < argc) g_icd = argv[++i];
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) spvdir = argv[++i];
      else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
      else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
      else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atol(argv[++i]);
      else if (!strcmp(argv[i], "--calls") && i + 1 < argc) calls = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--timeout-s") && i + 1 < argc) tmo = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--no-fork")) nofork = 1;
      else if (!strcmp(argv[i], "--trap-fatal")) trapfatal = 1;
      else { fprintf(stderr, "usage: vk_fuzz --icd so --spv-dir dir [--mode spv|structs|all] [--seed S] [--iters N] [--calls C] [--timeout-s T] [--no-fork] [--trap-fatal]\n"); return 2; }
   }
   const char *be = getenv("MALI_BACKEND");
   if (!g_icd || !spvdir || !be || strcmp(be, "fake")) { fprintf(stderr, "need --icd, --spv-dir and MALI_BACKEND=fake\n"); return 2; }
   g_mode = !strcmp(mode, "spv") ? 1 : !strcmp(mode, "structs") ? 2 : 3;
   load_corpus(spvdir);
   if (!trapfatal) signal(SIGTRAP, on_trap);
   long bad = 0;
   for (long i = 0; i < iters; i++) {
      uint64_t s = seed + i;
      if (nofork) { if (!setup()) return 2; iteration(s, calls); continue; }
      pid_t p = fork();
      if (p == 0) { alarm(tmo); if (!setup()) _exit(2); iteration(s, calls); _exit(0); }
      int st; waitpid(p, &st, 0);
      if (WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st) != 0)) {
         bad++;
         if (WIFSIGNALED(st)) printf("FINDING seed=%llu mode=%s signal=%d(%s)%s\n", (unsigned long long)s, mode, WTERMSIG(st), strsignal(WTERMSIG(st)),
                                     WTERMSIG(st) == SIGALRM ? " HANG" : "");
         else printf("FINDING seed=%llu mode=%s exit=%d\n", (unsigned long long)s, mode, WEXITSTATUS(st));
         fflush(stdout);
      }
   }
   printf("done iters=%ld findings=%ld seed0=%llu\n", iters, bad, (unsigned long long)seed);
   return bad ? 1 : 0;
}
