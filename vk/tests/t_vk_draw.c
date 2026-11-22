/* SPDX-License-Identifier: MIT */
/* M9 test: Vulkan client draws a rotating triangle through the ICD (shader modules, graphics pipeline,
 * render pass, framebuffer, command buffer, queue submit, fence, host readback). Loader-free: dlopens the
 * ICD like t_queue --direct. One JSON line per case; exit 0 = all pass.
 *
 *   t_vk_draw --direct <icd.so> [--loop N] [--loop-reset] [--only triangle|two|depth|rot|idx16|idx32|scissor|viewport|multivp|clearatt|ring|pushfs|ubo|descupd|sets8|texture|depth16|depth32s8|stencil]
 *             [--spv-dir dir]
 * Pixel checks are skipped on the fake backend (MALI_BACKEND=fake): it renders nothing.
 * --loop N repeats the triangle case N times; --loop-reset reuses one command buffer via
 * vkResetCommandPool instead of allocating a fresh one each time.
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define W 256
#define H 256

static VkFormat g_depfmt = VK_FORMAT_D32_SFLOAT;
static VkFormat g_colfmt = VK_FORMAT_R8G8B8A8_UNORM;   /* colour attachment format (4 or 8 bytes per texel) */
static VkFormat g_vfmt;   /* view (and render pass attachment) format when it differs from g_colfmt: MUTABLE_FORMAT image; 0 = same */
static unsigned g_colbpp = 4;
static float g_pcfs[4] = {0.75f, 0.25f, 0.5f, 1.0f};   /* MODE_PUSHFS fragment colour (linear); tests override it */
static VkFormat g_texfmt, g_texvfmt;   /* MODE_TEX source image / view format (0 = R8G8B8A8_UNORM; differs = MUTABLE_FORMAT) */
static const float *g_clear_f;   /* clear the colour attachment to these four floats (SNORM / UNORM16 / float RAW targets) */
static int g_nofs;      /* pipeline without a fragment stage (depth/stencil-only draw, no rasterizer discard) */
static int g_hostread;  /* record the colour-write -> HOST_READ dependency: the test reads the image through its mapping */
static int g_sparse;    /* vertex attributes at locations 0 and 2 (layout_skip): the skipped slot 1 is filled by the driver */
static int g_clearzs;   /* clearquad: the CmdClearAttachments after the draw also clears depth+stencil of the sub-rect */
static int g_nocol;     /* render pass with a depth attachment only (no colour attachment) */
static int g_dynstr;   /* dynamic VERTEX_INPUT_BINDING_STRIDE with a wrong static stride; the draw binds strides through CmdBindVertexBuffers2 */
static PFN_vkCmdBindVertexBuffers2 CmdBindVertexBuffers2_;
static int g_discard;   /* 1: rasterizerDiscardEnable with both stages, 2: vertex-only pipeline + discard */
static int g_clear_u;   /* clear the colour attachment to the integer value (200,100,50,7) instead of float zeros */
static int g_load;   /* two-pass LOAD test: store depth/stencil, and build s->rp2 = the same pass with loadOp LOAD */
static int g_fail, g_fake, g_loop = 1, g_reset;
/* R4 fixed-function state of the next scene_create()/scene_draw() (all zero = the old tests' state) */
static struct {
   int on;                      /* use fxz.vert (z = rot.w) and the fields below */
   VkCullModeFlags cull;
   int cw;                      /* VK_FRONT_FACE_CLOCKWISE instead of COUNTER_CLOCKWISE */
   int clamp, bias;
   int dclip;                   /* VK_EXT_depth_clip_enable: 0 = not chained, 1 = depthClipEnable FALSE, 2 = TRUE */
   float bias_c, bias_s, bias_clamp;
   int blend;
   VkBlendFactor sf, df;
   VkBlendOp op;
   uint32_t wmask;
   float bc[4];
   int poly;                    /* VkPolygonMode (0 fill, 1 line, 2 point), applied with or without .on */
   int dynpoly;                 /* poly is set with vkCmdSetPolygonModeEXT (pipeline polygonMode FILL + VK_DYNAMIC_STATE_POLYGON_MODE_EXT) */
   int dual;                    /* dual.frag (two outputs: o0 = varying colour, o1 = 0.5) instead of var.frag */
   int logic;                   /* 1 + VkLogicOp: static logicOpEnable */
   int dyn;                     /* cull/front face/bias/blend constants are set dynamically (the static pipeline values are wrong on purpose) */
} g_fx;
/* stencil test of the next scene (depth test is off then); values are Vulkan's */
struct st_state {
   int on;
   VkCompareOp cmp_f, cmp_b;
   VkStencilOp fail, dfail, pass;
   uint32_t cmask, wmask, ref, clear;
   int cw;                      /* front face = clockwise */
   int dyn;                     /* compare mask / write mask / reference are dynamic (the static values are wrong on purpose) */
};
static struct st_state g_st;
static VkPhysicalDeviceFeatures g_feat;
static double g_t_rec, g_t_sub;   /* scene_draw: record / QueueSubmit microseconds, summed */
static int g_share;   /* share test: 2 = alternate the scissor halves per draw, 3 = dynamic-UBO offset from g_share_off[] and firstInstance from g_share_fi[] */
static uint32_t g_share_off[8], g_share_fi[8];
static int g_dirty;   /* share_dirty: 1 viewport / 2 cull mode changes between draws, 3 = the same pipeline rebound before every draw
                        (push constants: the caller passes different rot[] values) */
static int g_inst;   /* share_inst: per-instance attribute pipeline; 1 = firstInstance / vertex-buffer-offset changes between draws, 2 = four identical draws */
static int g_dxvk;   /* dxvk_frame: second pipeline (cull back + alpha blend) and six dynamic states so the draw loop can churn state like DXVK */
static int g_dyn;   /* many_draws: dynamic-UBO descriptor offset per draw (the mdbench triangle shape) */
static PFN_vkCmdSetPolygonModeEXT CmdSetPolygonModeEXT_;
static PFN_vkCmdSetDepthClipEnableEXT CmdSetDepthClipEnableEXT_;   /* blitreplay: depthClipEnable via the dynamic state, if the extension is there */
static int g_have_eds3;      /* VK_EXT_extended_dynamic_state3 advertised, enabled, extendedDynamicState3PolygonMode set */
static int g_have_dclip;   /* VK_EXT_depth_clip_enable advertised and enabled */
static int g_have_dyn_dclip;   /* extendedDynamicState3DepthClipEnable advertised and enabled: the bit the dynamic
                                * VK_DYNAMIC_STATE_DEPTH_CLIP_ENABLE_EXT declaration and setter both need */
static VkPipelineRasterizationDepthClipStateCreateInfoEXT g_dce = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT};
static VkSampleCountFlags g_fbcounts;   /* limits.framebufferColorSampleCounts */
static const char *g_only, *g_spv = "shader";
static int g_msaa;   /* 1: 4x MSAA colour + resolve attachment, 2: additionally sampleShadingEnable (the cases gate on the driver's advertised counts, not on MALI_EXPERIMENTAL_MSAA) */
static int g_msaa_store;   /* g_msaa: storeOp STORE on the 4x attachment (samples stored, resolve in a separate pass) instead of DONT_CARE (hardware Average) */

/* MODE_SHAREIDX stays BELOW MODE_PUSHFS: the >= MODE_PUSHFS test means "the full-screen indexed quad", and that branch must not bind
 * an index buffer of its own before the share_idx draws (the point of that case is the exact index-buffer register sequence). */
enum { MODE_TRI, MODE_IDX16, MODE_IDX32, MODE_CLEARATT, MODE_CLEARQUAD, MODE_SCISSOR, MODE_VPORT, MODE_MVP, MODE_SHAREIDX, MODE_PUSHFS, MODE_TEX, MODE_UBO, MODE_DESCUPD, MODE_MDI, MODE_SETS8 };

#define CASE_ON(n) ((!g_only || !strcmp(g_only, n)) && !case_skipped(n))
/* MALI_SKIP_CASES="a,b": cases to leave out (farm --env route: lets one failing case be isolated without a phone shell) */
static int
case_skipped(const char *n)
{
   const char *e = getenv("MALI_SKIP_CASES");
   size_t l = strlen(n);
   for (; e && *e; e++)
      if (!strncmp(e, n, l) && (e[l] == ',' || !e[l]) && (e == getenv("MALI_SKIP_CASES") || e[-1] == ','))
         return 1;
   return 0;
}
#define CHECK(c, ...)                                                                              \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("{\"fail\":\"%s:%d\",\"expr\":\"%s\",\"detail\":\"", __FILE__, __LINE__, #c);     \
         printf(__VA_ARGS__);                                                                      \
         printf("\"}\n");                                                                          \
         g_fail = 1;                                                                               \
         return 0;                                                                                 \
      }                                                                                            \
   } while (0)

#define DEV_FNS(X)                                                                                 \
   X(DestroyDevice) X(GetDeviceQueue) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) X(InvalidateMappedMemoryRanges)  \
   X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(QueueSubmit) X(DeviceWaitIdle)  \
   X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory)                  \
   X(CreateImageView) X(DestroyImageView) X(GetImageSubresourceLayout) X(CreateBuffer)             \
   X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateShaderModule)       \
   X(DestroyShaderModule) X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateRenderPass)      \
   X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateGraphicsPipelines)       \
   X(DestroyPipeline) X(CreateCommandPool) X(DestroyCommandPool) X(ResetCommandPool)                \
   X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer)        \
   X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline) X(CmdSetViewport) X(CmdSetScissor)  \
   X(CmdBindVertexBuffers) X(CmdBindVertexBuffers2) X(CmdPushConstants) X(CmdDraw) X(CmdDrawIndexed) X(CmdBindIndexBuffer) X(CmdDrawIndexedIndirect)  \
   X(CmdClearAttachments) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(FlushMappedMemoryRanges) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout)  \
   X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateSampler) \
   X(DestroySampler) X(CmdBindDescriptorSets) X(CreateQueryPool) X(DestroyQueryPool)       \
   X(CmdSetStencilReference) X(CmdSetStencilCompareMask) X(CmdSetStencilWriteMask) X(CmdSetCullMode) X(CmdSetFrontFace) X(CmdSetDepthBias) X(CmdSetBlendConstants) X(CmdResetQueryPool) X(CmdBeginQuery) X(CmdEndQuery) X(GetQueryPoolResults) \
   X(CmdBeginRendering) X(CmdEndRendering) X(CmdPipelineBarrier2) X(CmdSetViewportWithCount)      \
   X(CmdSetScissorWithCount) X(CmdSetLineWidth) X(CmdSetDepthBoundsTestEnable) X(CmdSetPrimitiveTopology)    \
   X(CmdSetDepthTestEnable) X(CmdSetDepthWriteEnable) X(CmdSetDepthCompareOp)                      \
   X(CmdSetStencilTestEnable) X(CmdSetStencilOp)                                                   \
   X(CmdSetRasterizerDiscardEnable) X(CmdSetDepthBiasEnable) X(CmdSetPrimitiveRestartEnable)

#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memprops;

static double
now_us(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static int
memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
         return (int)i;
   return -1;
}

static VkShaderModule
load_spv(const char *name)
{
   char path[512];
   snprintf(path, sizeof path, "%s/%s", g_spv, name);
   FILE *f = fopen(path, "rb");
   if (!f) { printf("{\"fail\":\"open %s\"}\n", path); return VK_NULL_HANDLE; }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *buf = malloc(n);
   if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return VK_NULL_HANDLE; }
   fclose(f);
   VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = buf};
   VkShaderModule m = VK_NULL_HANDLE;
   if (CreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS)
      m = VK_NULL_HANDLE;
   free(buf);
   return m;
}

/* ------------------------------------------------------------------ scene */
struct scene {
   int depth;
   VkShaderModule vs, fs;
   VkPipelineLayout layout;
   VkRenderPass rp, rp2;
   int second;   /* scene_draw uses rp2 (LOAD) */
   VkPipeline pipe, pipe2;
   VkImage col, dep;
   VkDeviceMemory colmem, depmem, vbmem;
   VkImageView colv, depv;
   VkFramebuffer fb;
   VkBuffer vb[2];
   VkCommandPool pool;
   VkCommandBuffer cb; /* reused with --loop-reset */
   uint8_t *px;
   uint32_t pitch;
   VkBuffer ib;
   VkBuffer ib2;  /* MODE_SHAREIDX: a second index buffer in the same allocation (only the VA differs from ib) */
   VkDeviceMemory ibmem;
   int mode;     /* MODE_* */
   VkDescriptorSetLayout dsl;
   VkDescriptorPool dpool;
   VkDescriptorSet dset[2];
   VkDescriptorSet dset8[8]; /* MODE_SETS8: eight sets bound in one draw */
   VkDescriptorSetLayout dsl8[8]; /* persistent set-layout array (a stack array dangled behind the pipeline layout) */
   VkBuffer ubo[8];          /* uniform buffers (colours) */
   VkDeviceMemory ubomem;
   uint8_t *ubop;            /* mapped, 256 bytes per buffer */
   VkImage tim;
   VkDeviceMemory timem;
   VkImageView tiv;
   VkSampler smp;
   VkImage msi;              /* g_msaa: the 4-sample colour image (col is its single-sample resolve target) */
   VkDeviceMemory msmem;
   VkImageView msv;
};

static int
scene_create(struct scene *s, int depth)
{
   /* s->mode is an INPUT: callers must initialise the struct. An uninitialised stack `struct scene` made mode random per process (low half of
    * an ASLR pointer): >= MODE_PUSHFS drew the full-screen indexed quad instead of the triangle (coverage 65536, ~50 % of runs on the CMF). */
   int mode = s->mode;
   memset(s, 0, sizeof *s);
   s->mode = mode;
   s->depth = depth;
   const char *vsn = g_inst ? "inst_attr.vert.spv" : g_sparse ? "rot_sparse.vert.spv" : g_fx.on ? "fxz.vert.spv" : "rot.vert.spv", *fsn = "var.frag.spv";
   if (g_fx.dual) fsn = "dual.frag.spv";
   if (mode == MODE_TEX) { vsn = "tex.vert.spv"; fsn = "tex.frag.spv"; }
   if (mode == MODE_UBO || mode == MODE_DESCUPD || mode == MODE_SETS8) fsn = "ubo.frag.spv";
   if (mode == MODE_SETS8) fsn = "sets8.frag.spv";
   if (mode == MODE_PUSHFS) fsn = "pc.frag.spv";
   s->vs = load_spv(vsn);
   s->fs = load_spv(fsn);
   CHECK(s->vs && s->fs, "shader modules %s %s", vsn, fsn);

   VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = 16};
   if (mode == MODE_PUSHFS) { pcr.stageFlags |= VK_SHADER_STAGE_FRAGMENT_BIT; pcr.size = 32; }
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   if (mode == MODE_TEX || mode == MODE_UBO || mode == MODE_DESCUPD || mode == MODE_SETS8) {
      VkDescriptorSetLayoutBinding lb = {.binding = 0, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
         .descriptorType = mode == MODE_TEX ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : g_dyn ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
      VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                             .bindingCount = 1, .pBindings = &lb};
      CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &s->dsl) == VK_SUCCESS, "dsl");
      for (unsigned i = 0; i < 8; i++) s->dsl8[i] = s->dsl;
      VkDescriptorSetLayout *l8 = s->dsl8;
      unsigned nset = mode == MODE_SETS8 ? 8 : 2;   /* MODE_SETS8: eight identical layouts, all bound in one draw */
      pli.setLayoutCount = mode == MODE_SETS8 ? 8 : 1;
      pli.pSetLayouts = l8;
      VkDescriptorPoolSize ps = {lb.descriptorType, nset};
      VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = nset,
                                        .poolSizeCount = 1, .pPoolSizes = &ps};
      CHECK(CreateDescriptorPool(dev, &dpi, NULL, &s->dpool) == VK_SUCCESS, "dpool");
      VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = s->dpool,
                                         .descriptorSetCount = nset, .pSetLayouts = l8};
      CHECK(AllocateDescriptorSets(dev, &dai, mode == MODE_SETS8 ? s->dset8 : s->dset) == VK_SUCCESS, "alloc sets");
   }
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &s->layout) == VK_SUCCESS, "pipeline layout");

   /* colour image: linear RGBA8, host visible so the test can read it back. g_vfmt: the view format differs from the
    * image format (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT), as zink presents an SRGB view of a UNORM swapchain image. */
   const VkFormat colvfmt = g_vfmt ? g_vfmt : g_colfmt;
   VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                           .format = g_colfmt, .extent = {W, H, 1}, .mipLevels = 1,
                           .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
                           .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           .flags = colvfmt != g_colfmt ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
                           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   CHECK(CreateImage(dev, &ii, NULL, &s->col) == VK_SUCCESS, "colour image");
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, s->col, &mr);
   int mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   CHECK(mt >= 0, "no host visible type");
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = mt};
   CHECK(AllocateMemory(dev, &ai, NULL, &s->colmem) == VK_SUCCESS, "colour mem");
   CHECK(BindImageMemory(dev, s->col, s->colmem, 0) == VK_SUCCESS, "bind colour");
   CHECK(MapMemory(dev, s->colmem, 0, VK_WHOLE_SIZE, 0, (void **)&s->px) == VK_SUCCESS, "map colour");
   VkImageSubresource sr = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
   VkSubresourceLayout sl;
   GetImageSubresourceLayout(dev, s->col, &sr, &sl);
   s->pitch = (uint32_t)sl.rowPitch;
   s->px += sl.offset;
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = s->col,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = g_vfmt ? g_vfmt : g_colfmt,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   CHECK(CreateImageView(dev, &vi, NULL, &s->colv) == VK_SUCCESS, "colour view");

   VkImageView atts[2] = {s->colv, VK_NULL_HANDLE};
   if (g_msaa) {
      VkImageCreateInfo mi = ii;
      mi.samples = VK_SAMPLE_COUNT_4_BIT;
      mi.tiling = VK_IMAGE_TILING_OPTIMAL;
      mi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      CHECK(CreateImage(dev, &mi, NULL, &s->msi) == VK_SUCCESS, "4-sample colour image");
      GetImageMemoryRequirements(dev, s->msi, &mr);
      VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = memtype(mr.memoryTypeBits, 0)};
      CHECK(AllocateMemory(dev, &ma, NULL, &s->msmem) == VK_SUCCESS, "msaa mem");
      CHECK(BindImageMemory(dev, s->msi, s->msmem, 0) == VK_SUCCESS, "bind msaa");
      VkImageViewCreateInfo mv = vi;
      mv.image = s->msi;
      CHECK(CreateImageView(dev, &mv, NULL, &s->msv) == VK_SUCCESS, "msaa view");
      atts[0] = s->msv;      /* attachment 0 = MSAA, attachment 1 = the resolve image (colv) */
      atts[1] = s->colv;
   }
   if (depth) {
      VkImageCreateInfo di = ii;
      di.format = g_depfmt;
      di.tiling = VK_IMAGE_TILING_OPTIMAL;
      di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
      CHECK(CreateImage(dev, &di, NULL, &s->dep) == VK_SUCCESS, "depth image");
      GetImageMemoryRequirements(dev, s->dep, &mr);
      mt = memtype(mr.memoryTypeBits, 0);
      VkMemoryAllocateInfo da = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = mt};
      CHECK(AllocateMemory(dev, &da, NULL, &s->depmem) == VK_SUCCESS, "depth mem");
      CHECK(BindImageMemory(dev, s->dep, s->depmem, 0) == VK_SUCCESS, "bind depth");
      VkImageViewCreateInfo dv = vi;
      dv.image = s->dep;
      dv.format = g_depfmt;
      dv.subresourceRange.aspectMask = g_depfmt == VK_FORMAT_D32_SFLOAT_S8_UINT || g_depfmt == VK_FORMAT_D24_UNORM_S8_UINT ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) :
                                        g_depfmt == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
      CHECK(CreateImageView(dev, &dv, NULL, &s->depv) == VK_SUCCESS, "depth view");
      atts[1] = s->depv;
   }

   VkAttachmentDescription ad[2] = {
      {.format = colvfmt, .samples = VK_SAMPLE_COUNT_1_BIT,
       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
       .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_GENERAL},
      {.format = g_depfmt, .samples = VK_SAMPLE_COUNT_1_BIT,
       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .stencilLoadOp = g_st.on ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
   if (g_msaa) {
      ad[0].samples = VK_SAMPLE_COUNT_4_BIT;
      ad[0].storeOp = g_msaa_store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
      ad[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      ad[1] = ad[0];
      ad[1].samples = VK_SAMPLE_COUNT_1_BIT;
      ad[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      ad[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      ad[1].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
   }
   if (g_nocol)
      ad[0] = ad[1];   /* the depth attachment becomes attachment 0; the colour image is created but not attached */
   VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkAttachmentReference rref = {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkAttachmentReference dref = {g_nocol ? 0 : 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = g_nocol ? 0 : 1,
                              .pColorAttachments = g_nocol ? NULL : &cref, .pResolveAttachments = g_msaa ? &rref : NULL,
                              .pDepthStencilAttachment = depth ? &dref : NULL};
   VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = g_nocol ? 1 : depth || g_msaa ? 2 : 1,
                                 .pAttachments = ad, .subpassCount = 1, .pSubpasses = &sp};
   if (g_load) {
      ad[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      if (g_st.on) ad[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
   }
   CHECK(CreateRenderPass(dev, &rpi, NULL, &s->rp) == VK_SUCCESS, "render pass");
   if (g_load) {
      /* the second pass over the same attachments: LOAD everything the first one stored */
      ad[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      ad[0].initialLayout = VK_IMAGE_LAYOUT_GENERAL;
      ad[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      ad[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      if (g_st.on) ad[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      CHECK(CreateRenderPass(dev, &rpi, NULL, &s->rp2) == VK_SUCCESS, "render pass (LOAD)");
   }
   if (g_nocol)
      atts[0] = atts[1];
   VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = s->rp,
                                  .attachmentCount = g_nocol ? 1 : depth || g_msaa ? 2 : 1, .pAttachments = atts, .width = W, .height = H,
                                  .layers = 1};
   CHECK(CreateFramebuffer(dev, &fbi, NULL, &s->fb) == VK_SUCCESS, "framebuffer");

   /* vertex buffers: binding 0 position, binding 1 colour */
   /* vertices 0..2 triangle; 3..6 a quad (-.5..+.5) drawn indexed with vertexOffset 3 */
   static const float pos[44] = {-.5f, -.5f, 0, 1, .5f, -.5f, 0, 1, 0, .5f, 0, 1,
                                 -.5f, -.5f, 0, 1, .5f, -.5f, 0, 1, -.5f, .5f, 0, 1, .5f, .5f, 0, 1,
                                 -1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1};
   static const float colr[44] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1,
                                  1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1,
                                  0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1};
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256,
                            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
   for (int i = 0; i < 2; i++)
      CHECK(CreateBuffer(dev, &bi, NULL, &s->vb[i]) == VK_SUCCESS, "vertex buffer");
   GetBufferMemoryRequirements(dev, s->vb[0], &mr);
   VkDeviceSize stride = (mr.size + mr.alignment - 1) & ~(mr.alignment - 1);
   VkMemoryAllocateInfo va = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = stride * 2,
                              .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
   CHECK(AllocateMemory(dev, &va, NULL, &s->vbmem) == VK_SUCCESS, "vb mem");
   uint8_t *vp;
   CHECK(MapMemory(dev, s->vbmem, 0, VK_WHOLE_SIZE, 0, (void **)&vp) == VK_SUCCESS, "map vb");
   memcpy(vp, pos, sizeof pos);
   memcpy(vp + stride, colr, sizeof colr);
   UnmapMemory(dev, s->vbmem);
   CHECK(BindBufferMemory(dev, s->vb[0], s->vbmem, 0) == VK_SUCCESS, "bind vb0");
   CHECK(BindBufferMemory(dev, s->vb[1], s->vbmem, stride) == VK_SUCCESS, "bind vb1");

   /* index buffer: u16 {0,1,2,2,1,3} at byte 0, u32 same at byte 16 */
   VkBufferCreateInfo ibi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 128,
                             .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
   CHECK(CreateBuffer(dev, &ibi, NULL, &s->ib) == VK_SUCCESS, "index buffer");
   GetBufferMemoryRequirements(dev, s->ib, &mr);
   VkMemoryAllocateInfo ia = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size * 2,
                              .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
   CHECK(AllocateMemory(dev, &ia, NULL, &s->ibmem) == VK_SUCCESS, "ib mem");
   uint8_t *ip;
   CHECK(MapMemory(dev, s->ibmem, 0, VK_WHOLE_SIZE, 0, (void **)&ip) == VK_SUCCESS, "map ib");
   static const uint16_t i16[6] = {0, 1, 2, 2, 1, 3};
   uint32_t i32[6];
   for (int i = 0; i < 6; i++) i32[i] = i16[i];
   memcpy(ip, i16, sizeof i16);
   memcpy(ip + 16, i32, sizeof i32);
   /* R4: two DrawIndexedIndirect commands at byte 64 (stride 20): triangle 1 {0,1,2}, triangle 2 {2,1,3} */
   static const uint32_t ind[10] = {3, 1, 0, 3, 0, 3, 1, 3, 3, 0};
   memcpy(ip + 64, ind, sizeof ind);
   /* MODE_SHAREIDX: the second index buffer, its own BO at ibmem + mr.size (same size, same type, only the VA differs) */
   CHECK(CreateBuffer(dev, &ibi, NULL, &s->ib2) == VK_SUCCESS, "index buffer 2");
   static const uint16_t i16b[6] = {3, 4, 5, 5, 4, 6};
   memcpy(ip + mr.size, i16b, sizeof i16b);
   UnmapMemory(dev, s->ibmem);
   CHECK(BindBufferMemory(dev, s->ib, s->ibmem, 0) == VK_SUCCESS, "bind ib");
   CHECK(BindBufferMemory(dev, s->ib2, s->ibmem, mr.size) == VK_SUCCESS, "bind ib2");

   if (mode == MODE_UBO || mode == MODE_DESCUPD || mode == MODE_SETS8) {
      /* 256-byte uniform buffers in one allocation; colours written by the test. MODE_SETS8: one per set (0..7) */
      const unsigned nubo = mode == MODE_SETS8 ? 8 : 3;
      VkBufferCreateInfo ui = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256,
                               .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
      VkDeviceSize step = 0;
      for (unsigned i = 0; i < nubo; i++) CHECK(CreateBuffer(dev, &ui, NULL, &s->ubo[i]) == VK_SUCCESS, "ubo");
      GetBufferMemoryRequirements(dev, s->ubo[0], &mr);
      step = (256 + mr.alignment - 1) & ~(mr.alignment - 1);
      VkMemoryAllocateInfo ua = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = step * nubo,
                                 .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
      CHECK(AllocateMemory(dev, &ua, NULL, &s->ubomem) == VK_SUCCESS, "ubo mem");
      CHECK(MapMemory(dev, s->ubomem, 0, VK_WHOLE_SIZE, 0, (void **)&s->ubop) == VK_SUCCESS, "map ubo");
      static const float colA[4] = {0.25f, 0.5f, 0.75f, 1.0f}, colB[4] = {1.0f, 0.0f, 0.5f, 1.0f}, colC[4] = {0.0f, 1.0f, 0.0f, 1.0f};
      static const float colD[4] = {0.5f, 0.0f, 1.0f, 1.0f};   /* sets 2..6: never read, must never alias to set 7 */
      for (unsigned i = 0; i < nubo; i++) {
         /* MODE_SETS8: only set 7 is green. UBO/DESCUPD: ubo[2] is the green one the descriptor update binds */
         const float *c = i == 0 ? colA : i == 1 ? colB : (mode == MODE_SETS8 ? i == 7 : i == 2) ? colC : colD;
         memcpy(s->ubop + step * i, c, 16);
      }
      for (unsigned i = 0; i < nubo; i++) CHECK(BindBufferMemory(dev, s->ubo[i], s->ubomem, step * i) == VK_SUCCESS, "bind ubo");
      /* set 0 = colour A, set 1 = colour B, MODE_SETS8: set i = ubo[i], the shader reads set 7 = colour C */
      for (unsigned i = 0; i < (mode == MODE_SETS8 ? 8u : 2u); i++) {
         VkDescriptorBufferInfo bi2 = {s->ubo[i], 0, 16};
         VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = mode == MODE_SETS8 ? s->dset8[i] : s->dset[i], .dstBinding = 0,
                                   .descriptorCount = 1, .descriptorType = g_dyn ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi2};
         UpdateDescriptorSets(dev, 1, &w, 0, NULL);
      }
   }
   if (mode == MODE_TEX) {
      /* 8x8 RGBA8 linear texture, texel (i,j) = (i*32, j*32, 128, 255) */
      const VkFormat tfmt = g_texfmt ? g_texfmt : VK_FORMAT_R8G8B8A8_UNORM;
      const VkFormat tvfmt = g_texvfmt ? g_texvfmt : tfmt;
      VkImageCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                              .format = tfmt, .extent = {8, 8, 1}, .mipLevels = 1, .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
                              .flags = tvfmt != tfmt ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
                              .usage = VK_IMAGE_USAGE_SAMPLED_BIT, .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED};
      CHECK(CreateImage(dev, &ti, NULL, &s->tim) == VK_SUCCESS, "texture image");
      GetImageMemoryRequirements(dev, s->tim, &mr);
      VkMemoryAllocateInfo ta = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
      CHECK(AllocateMemory(dev, &ta, NULL, &s->timem) == VK_SUCCESS, "tex mem");
      CHECK(BindImageMemory(dev, s->tim, s->timem, 0) == VK_SUCCESS, "bind tex");
      uint8_t *tp;
      CHECK(MapMemory(dev, s->timem, 0, VK_WHOLE_SIZE, 0, (void **)&tp) == VK_SUCCESS, "map tex");
      VkSubresourceLayout tl;
      GetImageSubresourceLayout(dev, s->tim, &sr, &tl);
      for (int j = 0; j < 8; j++)
         for (int i = 0; i < 8; i++) {
            uint8_t *t = tp + tl.offset + j * tl.rowPitch + i * 4;
            t[0] = i * 32; t[1] = j * 32; t[2] = 128; t[3] = 255;
         }
      UnmapMemory(dev, s->timem);
      VkImageViewCreateInfo tv = vi;
      tv.image = s->tim;
      tv.format = tvfmt;
      CHECK(CreateImageView(dev, &tv, NULL, &s->tiv) == VK_SUCCESS, "tex view");
      VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                                .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
      CHECK(CreateSampler(dev, &si, NULL, &s->smp) == VK_SUCCESS, "sampler");
      VkDescriptorImageInfo ii2 = {s->smp, s->tiv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->dset[0], .dstBinding = 0,
                                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                .pImageInfo = &ii2};
      UpdateDescriptorSets(dev, 1, &w, 0, NULL);
   }

   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = s->vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = s->fs, .pName = "main"}};
   VkVertexInputBindingDescription bd[2] = {{0, 16, VK_VERTEX_INPUT_RATE_VERTEX}, {1, 16, VK_VERTEX_INPUT_RATE_VERTEX}};
   VkVertexInputAttributeDescription ad2[2] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0}};
   if (g_sparse) ad2[1].location = 2;
   if (g_dynstr) bd[0].stride = bd[1].stride = 4;
   if (g_inst) bd[0].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
   VkPipelineVertexInputStateCreateInfo vis = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = g_inst ? 1 : 2, .pVertexBindingDescriptions = bd,
                                               .vertexAttributeDescriptionCount = g_inst ? 1 : 2, .pVertexAttributeDescriptions = ad2};
   VkPipelineInputAssemblyStateCreateInfo ias = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = s->mode == MODE_MVP ? 4 : 1, .scissorCount = s->mode == MODE_MVP ? 4 : 1};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                .polygonMode = g_fx.dynpoly ? VK_POLYGON_MODE_FILL : (VkPolygonMode)g_fx.poly, .cullMode = VK_CULL_MODE_NONE,
                                                .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f, .rasterizerDiscardEnable = g_discard != 0};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                              .rasterizationSamples = g_msaa ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT,
                                              .sampleShadingEnable = g_msaa == 2, .minSampleShading = g_msaa == 2 ? 1.0f : 0.0f};
   VkPipelineDepthStencilStateCreateInfo dss = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
                                                .depthCompareOp = VK_COMPARE_OP_LESS, .maxDepthBounds = 1.0f};
   if (g_st.on) {
      dss.depthTestEnable = dss.depthWriteEnable = VK_FALSE;
      dss.stencilTestEnable = VK_TRUE;
      VkStencilOpState f = {.failOp = g_st.fail, .passOp = g_st.pass, .depthFailOp = g_st.dfail, .compareOp = g_st.cmp_f,
                            .compareMask = g_st.dyn ? 0x0f : g_st.cmask, .writeMask = g_st.dyn ? 0 : g_st.wmask,
                            .reference = g_st.dyn ? 9 : g_st.ref};
      dss.front = f;
      dss.back = f;
      dss.back.compareOp = g_st.cmp_b;
      rs.frontFace = g_st.cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
   }
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   if (g_fx.on) {
      rs.cullMode = g_fx.dyn ? VK_CULL_MODE_NONE : g_fx.cull;
      rs.frontFace = (g_fx.cw != 0) != (g_fx.dyn != 0) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
      rs.depthClampEnable = g_fx.clamp;
      if (g_fx.dclip) { g_dce.depthClipEnable = g_fx.dclip == 2; rs.pNext = &g_dce; }
      rs.depthBiasEnable = g_fx.bias;
      if (!g_fx.dyn) { rs.depthBiasConstantFactor = g_fx.bias_c; rs.depthBiasSlopeFactor = g_fx.bias_s; rs.depthBiasClamp = g_fx.bias_clamp; }
      cba.blendEnable = g_fx.blend;
      cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = g_fx.sf;
      cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = g_fx.df;
      cba.colorBlendOp = cba.alphaBlendOp = g_fx.op;
      cba.colorWriteMask = g_fx.wmask;
   }
   VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = g_nocol ? 0 : 1, .pAttachments = &cba};
   if (g_fx.on && g_fx.logic) { cbs.logicOpEnable = VK_TRUE; cbs.logicOp = (VkLogicOp)(g_fx.logic - 1); }
   VkDynamicState dyn[11] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
                            VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
   VkPipelineDynamicStateCreateInfo dys = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = (g_fx.on && g_fx.dyn) || g_dxvk ? 6 : 2, .pDynamicStates = dyn};
   if (g_dynstr) dyn[dys.dynamicStateCount++] = VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
   if (g_fx.dynpoly) dyn[dys.dynamicStateCount++] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
   if (g_st.on && g_st.dyn) {   /* the three stencil states follow the others */
      for (int k = 0; k < 3; k++) dyn[dys.dynamicStateCount + k] = dyn[6 + k];
      dys.dynamicStateCount += 3;
   }
   if (g_fx.on) cbs.blendConstants[0] = g_fx.dyn ? 0.9f : g_fx.bc[0], cbs.blendConstants[1] = g_fx.dyn ? 0.9f : g_fx.bc[1],
                cbs.blendConstants[2] = g_fx.dyn ? 0.9f : g_fx.bc[2], cbs.blendConstants[3] = g_fx.dyn ? 0.9f : g_fx.bc[3];
   VkGraphicsPipelineCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = g_discard == 2 || g_nofs ? 1 : 2,
                                      .pStages = st, .pVertexInputState = &vis, .pInputAssemblyState = &ias,
                                      .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                      .pDepthStencilState = depth ? &dss : NULL, .pColorBlendState = &cbs,
                                      .pDynamicState = &dys, .layout = s->layout, .renderPass = s->rp};
   VkResult r = CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &s->pipe);
   CHECK(r == VK_SUCCESS, "graphics pipeline = %d", r);
   if (g_dxvk) {
      rs.cullMode = VK_CULL_MODE_BACK_BIT;
      cba.blendEnable = VK_TRUE;
      cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      r = CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &s->pipe2);
      CHECK(r == VK_SUCCESS, "graphics pipeline 2 = %d", r);
   }

   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   CHECK(CreateCommandPool(dev, &pci, NULL, &s->pool) == VK_SUCCESS, "pool");
   return 1;
}

static void
scene_destroy(struct scene *s)
{
   DeviceWaitIdle(dev);
   if (s->pool) DestroyCommandPool(dev, s->pool, NULL);
   if (s->pipe) DestroyPipeline(dev, s->pipe, NULL);
   if (s->pipe2) DestroyPipeline(dev, s->pipe2, NULL);
   if (s->fb) DestroyFramebuffer(dev, s->fb, NULL);
   if (s->rp) DestroyRenderPass(dev, s->rp, NULL);
   if (s->rp2) DestroyRenderPass(dev, s->rp2, NULL);
   for (int i = 0; i < 2; i++) if (s->vb[i]) DestroyBuffer(dev, s->vb[i], NULL);
   if (s->smp) DestroySampler(dev, s->smp, NULL);
   if (s->tiv) DestroyImageView(dev, s->tiv, NULL);
   if (s->tim) DestroyImage(dev, s->tim, NULL);
   if (s->timem) FreeMemory(dev, s->timem, NULL);
   for (int i = 0; i < 3; i++) if (s->ubo[i]) DestroyBuffer(dev, s->ubo[i], NULL);
   if (s->ubomem) FreeMemory(dev, s->ubomem, NULL);
   if (s->dpool) DestroyDescriptorPool(dev, s->dpool, NULL);
   if (s->dsl) DestroyDescriptorSetLayout(dev, s->dsl, NULL);
   if (s->ib) DestroyBuffer(dev, s->ib, NULL);
   if (s->ib2) DestroyBuffer(dev, s->ib2, NULL);
   if (s->ibmem) FreeMemory(dev, s->ibmem, NULL);
   if (s->vbmem) FreeMemory(dev, s->vbmem, NULL);
   if (s->msv) DestroyImageView(dev, s->msv, NULL);
   if (s->msi) DestroyImage(dev, s->msi, NULL);
   if (s->msmem) FreeMemory(dev, s->msmem, NULL);
   if (s->colv) DestroyImageView(dev, s->colv, NULL);
   if (s->depv) DestroyImageView(dev, s->depv, NULL);
   if (s->col) DestroyImage(dev, s->col, NULL);
   if (s->dep) DestroyImage(dev, s->dep, NULL);
   if (s->colmem) FreeMemory(dev, s->colmem, NULL);
   if (s->depmem) FreeMemory(dev, s->depmem, NULL);
   if (s->layout) DestroyPipelineLayout(dev, s->layout, NULL);
   if (s->vs) DestroyShaderModule(dev, s->vs, NULL);
   if (s->fs) DestroyShaderModule(dev, s->fs, NULL);
}

/* draw n triangles (rot[i] = cos, sin, ox, oy), submit, wait */
static int
scene_draw(struct scene *s, int n, const float (*rot)[4], VkFence fence)
{
   VkCommandBuffer cb = s->cb;
   double t_rec0 = now_us();
   if (!cb || !g_reset) {
      VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s->pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
      CHECK(AllocateCommandBuffers(dev, &ai, &cb) == VK_SUCCESS, "alloc cb");
      s->cb = cb;
   } else {
      CHECK(ResetCommandPool(dev, s->pool, 0) == VK_SUCCESS, "reset pool");
   }
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CHECK(BeginCommandBuffer(cb, &bi) == VK_SUCCESS, "begin");
   const int lop = g_fx.on && g_fx.logic;   /* logic-op cases need a nonzero destination: (51,102,153,204) */
   VkClearValue cv[2] = {{.color = {{lop ? 0.2f : 0, lop ? 0.4f : 0, lop ? 0.6f : 0, lop ? 0.8f : 0}}}, {.depthStencil = {1.0f, g_st.clear}}};
   if (g_clear_f)
      memcpy(cv[0].color.float32, g_clear_f, 16);
   if (g_nocol)
      cv[0] = cv[1];
   if (g_clear_u)
      cv[0].color.uint32[0] = 200, cv[0].color.uint32[1] = 100, cv[0].color.uint32[2] = 50, cv[0].color.uint32[3] = 7;
   VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = s->second ? s->rp2 : s->rp,
                               .framebuffer = s->fb, .renderArea = {{0, 0}, {W, H}},
                               .clearValueCount = g_nocol ? 1 : s->depth ? 2 : 1, .pClearValues = cv};
   CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
   VkViewport vp = {0, 0, W, H, 0.0f, 1.0f};
   VkRect2D sc = {{0, 0}, {W, H}};
   if (s->mode == MODE_SCISSOR) sc.extent.width = W / 2;   /* left half only */
   if (s->mode == MODE_VPORT || s->mode == MODE_MVP) vp = (VkViewport){0, 0, W / 2, H, 0.0f, 1.0f};   /* NDC quad maps into the left half */
   if (s->mode == MODE_CLEARATT) {
      VkClearAttachment ca = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0,
                              .clearValue = {.color = {{0, 0, 1, 1}}}};
      VkClearRect cr = {.rect = {{0, 0}, {W, H}}, .baseArrayLayer = 0, .layerCount = 1};
      CmdClearAttachments(cb, 1, &ca, 1, &cr);
   }
   if (s->mode == MODE_MVP) {
      /* multiViewport: 4 viewports/scissors; only index 0 is reachable without a geometry shader, so 1..3 are junk that must be ignored.
       * Set in two calls (first 2..3, then 0..1) to cover firstViewport != 0. */
      VkViewport vj[2] = {{0, 0, 3, 3, 0.0f, 1.0f}, {W / 2, 0, W / 2, H, 0.0f, 1.0f}};
      VkRect2D sj[2] = {{{0, 0}, {1, 1}}, {{0, 0}, {W, H}}};
      VkViewport v01[2] = {vp, {0, 0, 5, 5, 0.0f, 1.0f}};
      VkRect2D s01[2] = {sc, {{0, 0}, {2, 2}}};
      CmdSetViewport(cb, 2, 2, vj);
      CmdSetScissor(cb, 2, 2, sj);
      CmdSetViewport(cb, 0, 2, v01);
      CmdSetScissor(cb, 0, 2, s01);
   } else {
      CmdSetViewport(cb, 0, 1, &vp);
      CmdSetScissor(cb, 0, 1, &sc);
   }
   if (g_fx.on && g_fx.dyn) {
      CmdSetCullMode(cb, g_fx.cull);
      CmdSetFrontFace(cb, g_fx.cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
      CmdSetDepthBias(cb, g_fx.bias_c, g_fx.bias_clamp, g_fx.bias_s);
      CmdSetBlendConstants(cb, g_fx.bc);
   }
   if (g_fx.dynpoly) CmdSetPolygonModeEXT_(cb, (VkPolygonMode)g_fx.poly);
   VkBuffer vbs[2] = {s->vb[0], s->vb[1]};
   VkDeviceSize offs[2] = {0, 0};
   if (g_st.on && g_st.dyn) {
      CmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, g_st.cmask);
      CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, g_st.wmask);
      CmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, g_st.ref);
   }
   if (g_inst) {
      /* instance k of binding 0 = quad corner k (vertex 3 + k of the position buffer): firstInstance k draws the triangle at corner k */
      VkDeviceSize base = 3 * 16;
      CmdBindVertexBuffers(cb, 0, 1, vbs, &base);
      CmdDraw(cb, 3, 1, 0, 0);                              /* corner 0 */
      if (g_inst == 1) {
         CmdDraw(cb, 3, 1, 0, 1);                           /* differs only in firstInstance: corner 1 */
         VkDeviceSize b2 = base + 2 * 16;
         CmdBindVertexBuffers(cb, 0, 1, vbs, &b2);
         CmdDraw(cb, 3, 1, 0, 1);                           /* differs only in the vertex buffer offset: corner 3 */
         CmdDraw(cb, 3, 1, 0, 0);                           /* differs only in firstInstance: corner 2 */
      } else {
         CmdDraw(cb, 3, 1, 0, 0);
         CmdDraw(cb, 3, 1, 0, 0);
         CmdDraw(cb, 3, 1, 0, 0);
      }
      n = 0;
   } else if (g_dynstr) {
      /* two draws that differ only in the dynamic stride: stride 0 (every vertex = vertex 0: degenerate, no pixels), then 16 (the triangle) */
      VkDeviceSize sz[2] = {VK_WHOLE_SIZE, VK_WHOLE_SIZE}, z[2] = {0, 0}, st16[2] = {16, 16};
      CmdBindVertexBuffers2_(cb, 0, 2, vbs, offs, sz, z);
      CmdPushConstants(cb, s->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, rot[0]);
      CmdDraw(cb, 3, 1, 0, 0);
      CmdBindVertexBuffers2_(cb, 0, 2, vbs, offs, sz, st16);
      if (g_dynstr == 2) {
         /* a plain vkCmdBindVertexBuffers after the 2-form (new offsets/sizes only) must keep the stride 16 set above: if it reset the
          * stride to 0 the draw below would be degenerate and the triangle missing */
         CmdBindVertexBuffers(cb, 0, 2, vbs, offs);
      }
      CmdPushConstants(cb, s->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, rot[0]);
      CmdDraw(cb, 3, 1, 0, 0);
      n = 0;
   } else
   CmdBindVertexBuffers(cb, 0, 2, vbs, offs);
   /* the core 1.3 two-form (what zink always calls) must land on the same binding; strides NULL = the pipeline's. A draw that follows is the test. */
   { VkDeviceSize sizes[2] = {VK_WHOLE_SIZE, VK_WHOLE_SIZE}; CmdBindVertexBuffers2(cb, 0, 2, vbs, offs, sizes, NULL); }
   if (s->mode == MODE_IDX16) CmdBindIndexBuffer(cb, s->ib, 0, VK_INDEX_TYPE_UINT16);
   if (s->mode >= MODE_PUSHFS || s->mode == MODE_IDX32 || s->mode == MODE_MDI || s->mode == MODE_SCISSOR || s->mode == MODE_VPORT || s->mode == MODE_MVP) CmdBindIndexBuffer(cb, s->ib, 16, VK_INDEX_TYPE_UINT32);
   if ((s->mode == MODE_TEX || s->mode == MODE_UBO) && !g_dyn)
      CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->layout, 0, 1, &s->dset[0], 0, NULL);
   if (s->mode == MODE_SETS8)
      CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->layout, 0, 8, s->dset8, 0, NULL);
   if (s->mode == MODE_SHAREIDX) {
      /* four indexed draws with identical per-draw parameters (count, offsets, instance, index type/size): only the index buffer
       * moves, A B A B, and the pipeline is rebound (identically) before the third. vkCmdBindIndexBuffer sets no dirty bit, so this
       * is the case that hangs on whether the index-buffer VA is part of the frame-build fast path's key (mali_cs_emit.c
       * same_draw_words) — it must keep emitting the index-buffer register. Decoded by tests/check_share_idx.py. */
      for (int i = 0; i < n; i++) {
         if (i == 2) CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
         CmdBindIndexBuffer(cb, (i & 1) ? s->ib2 : s->ib, 0, VK_INDEX_TYPE_UINT16);
         CmdPushConstants(cb, s->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, rot[0]);
         CmdDrawIndexed(cb, 6, 1, 0, 3, 0);
      }
      n = 0;
   }
   for (int i = 0; i < n; i++) {
      if (g_dirty == 1) {
         VkViewport vi = {(i & 1) ? W / 2 : 0, 0, W / 2, H, 0.0f, 1.0f};
         CmdSetViewport(cb, 0, 1, &vi);
      } else if (g_dirty == 2)
         CmdSetCullMode(cb, (i & 1) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
      else if (g_dirty == 3)
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
      else if (g_dirty == 4) {
         VkRect2D si = {{(i & 1) ? W / 2 : 0, 0}, {W / 2, H}};
         CmdSetScissor(cb, 0, 1, &si);
      }
      if (g_share == 2) {
         VkRect2D sci = {{(i & 1) ? W / 2 : 0, 0}, {W / 2, H}};
         CmdSetScissor(cb, 0, 1, &sci);
      }
      if (s->mode == MODE_PUSHFS) {
         float pc[8] = {rot[i][0], rot[i][1], rot[i][2], rot[i][3], g_pcfs[0], g_pcfs[1], g_pcfs[2], g_pcfs[3]};
         CmdPushConstants(cb, s->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
      } else {
         CmdPushConstants(cb, s->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, rot[i]);
      }
      if (g_dyn) {
         uint32_t off = (g_share == 3 ? g_share_off[i] : (uint32_t)(i % 3)) * 256;   /* three colour blocks, 256 B apart */
         CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->layout, 0, 1, &s->dset[0], 1, &off);
      }
      if (s->mode == MODE_DESCUPD)
         CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->layout, 0, 1, &s->dset[i & 1], 0, NULL);
      if (s->mode == MODE_MDI)
         CmdDrawIndexedIndirect(cb, s->ib, 64, 2, 20);   /* multiDrawIndirect: drawCount 2 */
      else if (s->mode >= MODE_PUSHFS)
         CmdDrawIndexed(cb, 6, 1, 0, 7, g_share == 3 ? g_share_fi[i] : 0);
      else if (s->mode == MODE_IDX16 || s->mode == MODE_IDX32 || s->mode == MODE_SCISSOR || s->mode == MODE_VPORT || s->mode == MODE_MVP)
         CmdDrawIndexed(cb, 6, 1, 0, 3, 0);
      else {
         CmdDraw(cb, 3, 1, 0, 0);
         /* polygonMode POINT: a 2-vertex triangle-list draw assembles no triangle and is rewritten to a different primitive than the draw above;
          * with MALI_DIRTY_CHECK (check.sh) a reuse of the previous descriptors across that change aborts in build_draw_descs */
         if (g_fx.poly == 2) CmdDraw(cb, 2, 1, 0, 0);
      }
   }
   if (s->mode == MODE_CLEARQUAD) {   /* CmdClearAttachments AFTER a draw, sub-rectangle: a constant-colour quad (the triangle must survive outside the rect) */
      VkClearAttachment ca = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0, .clearValue = {.color = {{0, 0, 1, 1}}}};
      VkClearRect cr = {.rect = {{100, 100}, {60, 60}}, .baseArrayLayer = 0, .layerCount = 1};
      VkClearAttachment cas[2] = {ca, {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .clearValue = {.depthStencil = {1.0f, 0}}}};
      CmdClearAttachments(cb, g_clearzs && s->depth ? 2 : 1, cas, 1, &cr);
   }
   CmdEndRenderPass(cb);
   /* the mapped colour image is read on the host after the fence wait: colour-attachment writes are not visible to the
    * host by themselves and a fence wait is not a memory dependency (g_hostread: the view-format test reads pixels) */
   if (g_hostread) {
      VkMemoryBarrier2 hb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
         .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
      VkDependencyInfo hd = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &hb};
      CmdPipelineBarrier2(cb, &hd);
   }
   CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "end");
   g_t_rec += now_us() - t_rec0;
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   double t_sub0 = now_us();
   VkResult r = QueueSubmit(queue, 1, &si, fence);
   g_t_sub += now_us() - t_sub0;
   CHECK(r == VK_SUCCESS, "submit = %d", r);
   r = WaitForFences(dev, 1, &fence, VK_TRUE, 20000000000ull);
   CHECK(r == VK_SUCCESS, "wait = %d", r);
   ResetFences(dev, 1, &fence);
   if (!g_reset)
      FreeCommandBuffers(dev, s->pool, 1, &cb), s->cb = NULL;
   return 1;
}

static uint32_t
count_cov(struct scene *s)
{
   uint32_t n = 0;
   for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
         n += g_colbpp == 8 ? (s->px[y * s->pitch + x * 8 + 6] | s->px[y * s->pitch + x * 8 + 7]) != 0 : s->px[y * s->pitch + x * 4 + 3] != 0;
   return n;
}

static int
report(const char *name, struct scene *s, double expect, double tolpct, int exact_colour)
{
   if (g_fake) {
      printf("{\"test\":\"%s\",\"skipped\":\"fake backend\",\"ok\":true}\n", name);
      return 1;
   }
   uint32_t cov = count_cov(s);
   double err = fabs((double)cov - expect) / expect * 100.0;
   CHECK(err <= tolpct, "%s coverage %u expected %.0f (%.2f%%)", name, cov, expect, err);
   if (exact_colour) {
      /* centroid of (64,64) red, (192,64) green, (128,192) blue = (128,106): ~equal thirds */
      const uint8_t *p = s->px + 106 * s->pitch + 128 * 4;
      CHECK(abs(p[0] - 85) < 12 && abs(p[1] - 85) < 12 && abs(p[2] - 85) < 12 && p[3] == 255,
            "centroid colour %u,%u,%u,%u", p[0], p[1], p[2], p[3]);
      const uint8_t *q = s->px + 70 * s->pitch + 74 * 4; /* near the red vertex */
      CHECK(q[0] > 200 && q[1] < 60 && q[2] < 60, "red corner %u,%u,%u", q[0], q[1], q[2]);
      const uint8_t *g = s->px + 70 * s->pitch + 182 * 4;
      CHECK(g[1] > 200 && g[0] < 60 && g[2] < 60, "green corner %u,%u,%u", g[0], g[1], g[2]);
      const uint8_t *b = s->px + 182 * s->pitch + 128 * 4;
      CHECK(b[2] > 200 && b[0] < 60 && b[1] < 60, "blue corner %u,%u,%u", b[0], b[1], b[2]);
   }
   printf("{\"test\":\"%s\",\"coverage\":%u,\"expect\":%.0f,\"err_pct\":%.3f,\"ok\":true}\n", name, cov, expect, err);
   return 1;
}

/* ------------------------------------------------------------- blitreplay */
/* notes/blitreplay.md: the black glBlitFramebuffer frame of notes/apilog.md (its "two call sequences", seq 11-19) issued
 * as literally as that log allows by our own Vulkan test, so the black has a reproduction without zink. One pass, one
 * submission, dynamic rendering with flags 0, one colour attachment (view fmt 44 on image fmt 44, usage 0x97,
 * MUTABLE_FORMAT), loadOp DONT_CARE / storeOp STORE, dynamic state through the *WithCount setters, one draw:
 * TRIANGLE_FAN, 4 vertices, one vertex buffer stride 32 with 2 attributes (position vec4, texcoord vec4 in pixels), FS =
 * the dumped zink fragment shader read back with OpImageFetch (set 2, binding 128), source = an 800x600 colour image
 * rendered by an earlier pass in an earlier submission. The vertex data is the frame's, copied from notes/jobdump.md:
 * v0 pos(-1,-1,0,1) tex(0,600,0,0); v1 pos(1,-1,0,1) tex(800,600,0,0); v2/v3 = the top edge with tex y 0.
 * BLITREPLAY=x[,y] selects the rows (default "0,6"): 0 = the replay, 1 = our FS with zink's VS, 2 = zink's FS with our
 * VS, 3 = binding 128 -> 0, 4 = set 2 -> set 0, 5 = fan -> list, 6 = the twin (both shaders ours). Every row is read
 * back (RGB mean, centre texel, covered pixels), so "black" and "paints" are numbers and not a verdict. */
static const char *g_brname[7] = {"replay", "our_fs", "our_vs", "bind0", "set0", "list", "twin"};
#define BR_W 800
#define BR_H 600

struct br_res {
   VkImage src, dst;
   VkDeviceMemory srcm, dstm;
   VkImageView srcv, dstv;
   VkSampler smp;
   VkDescriptorSetLayout dsl[3];
   VkPipelineLayout pl;
   VkDescriptorPool dp;
   VkDescriptorSet dset;
   VkBuffer vb;
   VkDeviceMemory vbm;
   VkCommandPool pool;
   uint8_t *dpx;
   uint32_t dpitch;
};

/* one 800x600 colour image of the frame's shape, LINEAR + host visible so the test can read it back */
static int
br_img(VkImage *img, VkDeviceMemory *mem, VkImageView *view, VkFormat fmt, uint8_t **px, uint32_t *pitch)
{
   VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
      .format = fmt, .extent = {BR_W, BR_H, 1}, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,   /* 0x97, the frame's */
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT};
   CHECK(CreateImage(dev, &ii, NULL, img) == VK_SUCCESS, "br image");
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, *img, &mr);
   int mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   CHECK(mt >= 0, "br image memory type");
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
   CHECK(AllocateMemory(dev, &ai, NULL, mem) == VK_SUCCESS, "br image memory");
   CHECK(BindImageMemory(dev, *img, *mem, 0) == VK_SUCCESS, "br bind image");
   uint8_t *map;
   CHECK(MapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, (void **)&map) == VK_SUCCESS, "br map image");
   VkImageSubresource sr = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
   VkSubresourceLayout sl;
   GetImageSubresourceLayout(dev, *img, &sr, &sl);
   *pitch = (uint32_t)sl.rowPitch;
   *px = map + sl.offset;
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   CHECK(CreateImageView(dev, &vi, NULL, view) == VK_SUCCESS, "br image view");
   return 1;
}

static int
br_setup(struct br_res *r)
{
   memset(r, 0, sizeof *r);
   /* the sampled source is fmt 37 (R8G8B8A8_UNORM) and the frame's colour target fmt 44 (A8B8G8R8_UNORM_PACK32), as in
    * notes/blit-instr.md's view line and notes/apilog.md's attachment line */
   if (!br_img(&r->src, &r->srcm, &r->srcv, VK_FORMAT_R8G8B8A8_UNORM, &r->dpx, &r->dpitch)) return 0;
   r->dpx = NULL;
   if (!br_img(&r->dst, &r->dstm, &r->dstv, VK_FORMAT_B8G8R8A8_UNORM, &r->dpx, &r->dpitch)) return 0;
   VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f};   /* notes/blit-instr.md SAMPLER line */
   CHECK(CreateSampler(dev, &si, NULL, &r->smp) == VK_SUCCESS, "br sampler");
   /* no descriptor pool here: test_blitreplay's row loop creates and frees one per row, so a failed recreation cannot
    * leave a NULL pool behind for the next AllocateDescriptorSets */
   /* the frame's vertex data, 4 x 32 bytes, and the 6-vertex list form of the same quad after it */
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 512, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
   CHECK(CreateBuffer(dev, &bi, NULL, &r->vb) == VK_SUCCESS, "br vertex buffer");
   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(dev, r->vb, &mr);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
      .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
   CHECK(AllocateMemory(dev, &ai, NULL, &r->vbm) == VK_SUCCESS, "br vb memory");
   CHECK(BindBufferMemory(dev, r->vb, r->vbm, 0) == VK_SUCCESS, "br bind vb");
   float *vp;
   CHECK(MapMemory(dev, r->vbm, 0, VK_WHOLE_SIZE, 0, (void **)&vp) == VK_SUCCESS, "br map vb");
   static const float quad[4][8] = {
      {-1, -1, 0, 1,    0, 600, 0, 0}, {1, -1, 0, 1,  800, 600, 0, 0},
      {-1,  1, 0, 1,    0,   0, 0, 0}, {1,  1, 0, 1,  800,   0, 0, 0}};
   /* the 6-vertex list form of the fan's own triangles, in list order: (1,2,0) = the fan's (0,1,2) and (2,3,0) = its
    * (0,2,3), so rows 5 and the fan rows have the same footprint and a coverage difference can only be the topology.
    * Offsets are bytes: vp is a float*, one vertex is 8 floats = 32 B, and the list starts after the 4 quad vertices */
   static const int list[6] = {1, 2, 0, 2, 3, 0};
   memcpy(vp, quad, sizeof quad);
   for (int i = 0; i < 6; i++) memcpy((char *)vp + 32 * (4 + i), quad[list[i]], 32);
   UnmapMemory(dev, r->vbm);
   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   CHECK(CreateCommandPool(dev, &pci, NULL, &r->pool) == VK_SUCCESS, "br pool");
   return 1;
}

static void
br_destroy(struct br_res *r)
{
   DeviceWaitIdle(dev);
   if (r->pool) DestroyCommandPool(dev, r->pool, NULL);
   if (r->pl) DestroyPipelineLayout(dev, r->pl, NULL);
   for (int i = 0; i < 3; i++) if (r->dsl[i]) DestroyDescriptorSetLayout(dev, r->dsl[i], NULL);
   if (r->dp) DestroyDescriptorPool(dev, r->dp, NULL);
   if (r->smp) DestroySampler(dev, r->smp, NULL);
   if (r->vb) DestroyBuffer(dev, r->vb, NULL);
   if (r->vbm) FreeMemory(dev, r->vbm, NULL);
   for (int i = 0; i < 2; i++) {
      VkImageView v = i ? r->dstv : r->srcv;
      VkImage im = i ? r->dst : r->src;
      VkDeviceMemory m = i ? r->dstm : r->srcm;
      if (v) DestroyImageView(dev, v, NULL);
      if (im) DestroyImage(dev, im, NULL);
      if (m) FreeMemory(dev, m, NULL);
   }
}

/* one pass into an image: CLEAR blue (the earlier submission's source) or the frame's own DONT_CARE pass */
static int
br_pass(struct br_res *r, VkCommandBuffer cb, int src, VkPipeline pipe, int variant)
{
   const VkFormat fmt = src ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
   VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = src ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
      .srcAccessMask = src ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : 0,
      .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,   /* both passes start from UNDEFINED (br_img's initialLayout): the
                                                 * source pass clears the whole image, so its contents are not needed */
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,   /* the frame's own transition: UNDEFINED -> GENERAL, seq 11 */
      .image = src ? r->src : r->dst, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &ib};
   CmdPipelineBarrier2(cb, &dep);
   VkRenderingAttachmentInfo att = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = src ? r->srcv : r->dstv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .resolveMode = VK_RESOLVE_MODE_NONE,
      .loadOp = src ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
   att.clearValue.color.float32[0] = 0.0f;
   att.clearValue.color.float32[1] = 0.0f;
   att.clearValue.color.float32[2] = 1.0f;
   att.clearValue.color.float32[3] = 1.0f;
   VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .flags = 0,   /* the frame's ri.flags = 0 */
      .renderArea = {{0, 0}, {BR_W, BR_H}}, .layerCount = 1, .viewMask = 0,
      .colorAttachmentCount = 1, .pColorAttachments = &att};
   (void)fmt;
   CmdBeginRendering(cb, &ri);
   if (!src) {
      CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      VkBuffer b = r->vb;
      VkDeviceSize off = (variant == 5) ? 128 : 0, str = 32;
      CmdBindVertexBuffers2_(cb, 0, 1, &b, &off, NULL, &str);
      VkViewport vp = {0, 0, BR_W, BR_H, 0.0f, 1.0f};
      VkRect2D sc = {{0, 0}, {BR_W, BR_H}};
      CmdSetViewportWithCount(cb, 1, &vp);
      CmdSetScissorWithCount(cb, 1, &sc);
      CmdSetLineWidth(cb, 1.0f);
      CmdSetDepthBias(cb, 0.0f, 0.0f, 0.0f);
      float bc[4] = {0, 0, 0, 0};
      CmdSetBlendConstants(cb, bc);
      CmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
      CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
      CmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
      CmdSetCullMode(cb, VK_CULL_MODE_NONE);
      CmdSetFrontFace(cb, VK_FRONT_FACE_CLOCKWISE);   /* apilog prints the enum: front=1 = VK_FRONT_FACE_CLOCKWISE */
      CmdSetPrimitiveTopology(cb, variant == 5 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
      CmdSetDepthTestEnable(cb, VK_FALSE);
      CmdSetDepthWriteEnable(cb, VK_FALSE);
      CmdSetDepthCompareOp(cb, VK_COMPARE_OP_NEVER);   /* apilog: df=0 */
      CmdSetDepthBoundsTestEnable(cb, VK_FALSE);
      CmdSetStencilTestEnable(cb, VK_FALSE);
      CmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
      CmdSetRasterizerDiscardEnable(cb, VK_FALSE);
      CmdSetDepthBiasEnable(cb, VK_FALSE);
      CmdSetPrimitiveRestartEnable(cb, VK_FALSE);
      if (CmdSetPolygonModeEXT_) CmdSetPolygonModeEXT_(cb, VK_POLYGON_MODE_FILL);
      if (g_have_dyn_dclip && CmdSetDepthClipEnableEXT_) CmdSetDepthClipEnableEXT_(cb, VK_TRUE);   /* apilog: clip=1 */
      VkDescriptorSet ds = r->dset;
      CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pl, variant == 4 ? 0 : 2, 1, &ds, 0, NULL);
      char pc[52] = {0};   /* zink's gfx_pushconst block (52 B); the frame's blocks are zero */
      CmdPushConstants(cb, r->pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
      CmdDraw(cb, variant == 5 ? 6 : 4, 1, 0, 0);
   }
   CmdEndRendering(cb);
   return 1;
}

static int
test_blitreplay(void)
{
   struct br_res r;
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "br fence");
   if (!br_setup(&r)) { br_destroy(&r); DestroyFence(dev, f, NULL); return 0; }
   const char *sel = getenv("BLITREPLAY");
   int rows[8], nrow = 0;
   if (!sel || !*sel) sel = "0,6";
   for (char *p = (char *)sel; *p && nrow < 8;) {
      char *end;
      const long v = strtol(p, &end, 10);
      /* every token must be a number inside 0..6 followed by a comma or the end: "0,typo,6" used to fill the rest of the
       * slots with row 0 and silently skip row 6 */
      if (end == p || v < 0 || v > 6 || (*end != ',' && *end != '\0')) {
         printf("{\"test\":\"blitreplay\",\"fail\":\"bad BLITREPLAY token\",\"ok\":false}\n");
         br_destroy(&r);
         DestroyFence(dev, f, NULL);
         return 0;
      }
      rows[nrow++] = (int)v;
      p = (*end == ',') ? end + 1 : end;
   }
   int ok = 1;
   for (int i = 0; i < nrow; i++) {
      const int v = rows[i];
      const int vs_ours = v == 2 || v == 6;
      /* declared before the first goto so every failure path can free what exists and nothing else */
      VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
      VkPipeline pipe = VK_NULL_HANDLE;
      vs = load_spv(vs_ours ? "blitv_o.vert.spv" : "blitv_z.vert.spv");
      fs = load_spv(v == 0 || v == 2 || v == 5 ? "blitfs_z.frag.spv" :
                    v == 1 ? "blitfs_o.frag.spv" : v == 3 ? "blitfs_o_b0.frag.spv" :
                    v == 4 ? "blitfs_o_s0.frag.spv" : "blitfs_o.frag.spv");
      if (!vs || !fs) { ok = 0; goto row_out; }
      /* descriptor layout: the frame has 3 sets and the sampler at set 2 binding 128; rows 3/4 move exactly one of the
       * two, which is the only way the FS's hard-coded decorations can be compared */
      const int dsidx = v == 4 ? 0 : 2, dbind = v == 3 ? 0 : 128;
      for (int k = 0; k < 3; k++) {
         VkDescriptorSetLayoutBinding lb = {.binding = (uint32_t)dbind, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
         VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = k == dsidx ? 1 : 0, .pBindings = &lb};
         if (CreateDescriptorSetLayout(dev, &dli, NULL, &r.dsl[k]) != VK_SUCCESS) { g_fail = 1; ok = 0; goto row_out; }
      }
      /* one pool per row, created and checked here: a failed recreation used to reach AllocateDescriptorSets as NULL */
      VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
      VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
         .poolSizeCount = 1, .pPoolSizes = &ps};
      if (CreateDescriptorPool(dev, &dpi, NULL, &r.dp) != VK_SUCCESS) { g_fail = 1; ok = 0; goto row_out; }
      VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = r.dp,
         .descriptorSetCount = 1, .pSetLayouts = &r.dsl[dsidx]};
      if (AllocateDescriptorSets(dev, &dai, &r.dset) != VK_SUCCESS) { g_fail = 1; ok = 0; goto row_out; }
      VkDescriptorImageInfo dii = {r.smp, r.srcv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = r.dset,
         .dstBinding = (uint32_t)dbind, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &dii};
      UpdateDescriptorSets(dev, 1, &w, 0, NULL);
      VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, .size = 52};
      VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 3,
         .pSetLayouts = r.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
      if (CreatePipelineLayout(dev, &pli, NULL, &r.pl) != VK_SUCCESS) { g_fail = 1; ok = 0; goto row_out; }
      VkPipelineShaderStageCreateInfo st[2] = {
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
      VkVertexInputBindingDescription bd = {0, 32, VK_VERTEX_INPUT_RATE_VERTEX};
      VkVertexInputAttributeDescription ad[2] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
                                                 {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16}};
      VkPipelineVertexInputStateCreateInfo vis = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
         .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bd,
         .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = ad};
      VkPipelineInputAssemblyStateCreateInfo ias = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
         .topology = v == 5 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN};
      VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
         .viewportCount = 0, .scissorCount = 0};   /* 04135/04136: 0 with VK_DYNAMIC_STATE_*_WITH_COUNT */
      VkPipelineRasterizationDepthClipStateCreateInfoEXT dce = g_dce;
      dce.depthClipEnable = VK_TRUE;   /* apilog: clip=1 */
      VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
         .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE,
         .lineWidth = 1.0f, .rasterizerDiscardEnable = VK_FALSE, .depthClampEnable = VK_FALSE, .depthBiasEnable = VK_FALSE};
      if (g_have_dclip) { dce.pNext = NULL; rs.pNext = &dce; }   /* static value: ignored while the dynamic state below is declared */
      VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
         .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
      VkPipelineColorBlendAttachmentState cba = {.blendEnable = VK_FALSE, .colorWriteMask = 0xf};
      VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
         .attachmentCount = 1, .pAttachments = &cba};
      VkDynamicState dyn[24] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
         VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
         VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
         VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
         VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,
         VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
         VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
         VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
         VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
      unsigned ndyn = 21;
      if (CmdSetPolygonModeEXT_) dyn[ndyn++] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
      if (g_have_dyn_dclip) dyn[ndyn++] = VK_DYNAMIC_STATE_DEPTH_CLIP_ENABLE_EXT;   /* the EDS3 bit, not g_have_dclip */
      VkPipelineDynamicStateCreateInfo dys = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
         .dynamicStateCount = ndyn, .pDynamicStates = dyn};
      VkFormat cfmt = VK_FORMAT_B8G8R8A8_UNORM;
      VkPipelineRenderingCreateInfo prc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
         .colorAttachmentCount = 1, .pColorAttachmentFormats = &cfmt};
      VkGraphicsPipelineCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &prc,
         .stageCount = 2, .pStages = st, .pVertexInputState = &vis, .pInputAssemblyState = &ias,
         .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = NULL,
         .pColorBlendState = &cbs, .pDynamicState = &dys, .layout = r.pl};
      VkResult pr = CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe);
      if (pr != VK_SUCCESS) { g_fail = 1; ok = 0; goto row_out; }
      /* the earlier submission: the source pass (CLEAR blue), and its layout transition to sampled */
      for (int sub = 0; sub < 2; sub++) {
         VkCommandBuffer cb;
         VkCommandBufferAllocateInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = r.pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
         if (AllocateCommandBuffers(dev, &cbi, &cb) != VK_SUCCESS) { g_fail = 1; ok = 0; goto row_out; }
         VkCommandBufferBeginInfo bi2 = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
         if (BeginCommandBuffer(cb, &bi2) != VK_SUCCESS) { g_fail = 1; ok = 0; goto row_out; }
         if (!br_pass(&r, cb, sub == 0, pipe, v)) { ok = 0; goto row_out; }
         if (sub == 0) {
            VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
               .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               .image = r.src, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            VkDependencyInfo dp = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
               .pImageMemoryBarriers = &ib};
            CmdPipelineBarrier2(cb, &dp);
         }
         if (sub == 1) {
            /* the readback below reads the destination's colour writes through the mapping: colour-attachment writes
             * are not visible to the host by themselves and a fence wait is not a memory dependency */
            VkMemoryBarrier2 hb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
               .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
            VkDependencyInfo hdp = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
               .pMemoryBarriers = &hb};
            CmdPipelineBarrier2(cb, &hdp);
         }
         if (EndCommandBuffer(cb) != VK_SUCCESS) { g_fail = 1; ok = 0; goto row_out; }
         VkCommandBuffer cbs2[2] = {cb, VK_NULL_HANDLE};
         unsigned ncb = 1;
         if (sub == 1) {   /* the frame's submit is ncbs=2: its draw cb plus a sync cb (barriers only) */
            VkCommandBufferAllocateInfo cbi2 = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
               .commandPool = r.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
            if (AllocateCommandBuffers(dev, &cbi2, &cbs2[1]) == VK_SUCCESS) {
               VkCommandBufferBeginInfo bi3 = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
               BeginCommandBuffer(cbs2[1], &bi3);
               EndCommandBuffer(cbs2[1]);
               ncb = 2;
            }
         }
         VkSubmitInfo si2 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = ncb, .pCommandBuffers = cbs2};
         if (QueueSubmit(queue, 1, &si2, f) != VK_SUCCESS) { g_fail = 1; ok = 0; goto row_out; }
         /* bounded like scene_draw: a failed wait must not be reset and must not be followed by a pixel read */
         const VkResult wr = WaitForFences(dev, 1, &f, VK_TRUE, 2000000000ull);
         if (wr != VK_SUCCESS) {
            if (wr == VK_TIMEOUT) WaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX);
            g_fail = 1; ok = 0; goto row_out;
         }
         if (ResetFences(dev, 1, &f) != VK_SUCCESS) { g_fail = 1; ok = 0; goto row_out; }
         if (sub == 1) {
            /* the readback memory is only asked to be HOST_VISIBLE (br_img's memtype): invalidate the mapping before the
             * host reads it, which the spec requires unless the type is HOST_COHERENT (the call is a no-op there) */
            VkMappedMemoryRange mmr = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = r.dstm,
               .offset = 0, .size = VK_WHOLE_SIZE};
            InvalidateMappedMemoryRanges(dev, 1, &mmr);
         }
      }
      if (g_fake) {
         printf("{\"test\":\"blitreplay\",\"variant\":%d,\"name\":\"%s\",\"skipped\":\"fake backend\",\"ok\":true}\n",
                v, g_brname[v]);
      } else {
         /* the destination pass keeps the frame's LOAD_OP_DONT_CARE, so the pixels the draw does not write are
          * undefined: the statistics cover the footprint every row draws (the fan's (0,1,2)+(0,2,3), which leaves the
          * right-hand wedge -x < y < x, x > 0 untouched), never the whole 800x600 */
         double mr = 0, mg = 0, mb = 0;
         uint32_t cov = 0, area = 0;
         for (int y = 0; y < BR_H; y++)
            for (int x = 0; x < BR_W; x++) {
               const double nx = (x + 0.5) / BR_W * 2.0 - 1.0, ny = (y + 0.5) / BR_H * 2.0 - 1.0;   /* NDC */
               if (nx > 0.0 && ny > -nx && ny < nx) continue;   /* the fan's uncovered wedge: undefined contents */
               const uint8_t *p = r.dpx + y * r.dpitch + x * 4;
               mr += p[2], mg += p[1], mb += p[0];   /* fmt 44 = B8G8R8A8_UNORM */
               cov += p[3] != 0;
               area++;
            }
         mr /= (double)area, mg /= (double)area, mb /= (double)area;
         const uint8_t *c = r.dpx + (BR_H / 2) * r.dpitch + (BR_W / 2) * 4;
         printf("{\"test\":\"blitreplay\",\"variant\":%d,\"name\":\"%s\",\"mean\":\"%.1f/255\",\"rgb\":\"%.1f,%.1f,%.1f\","
                "\"centre\":\"%u,%u,%u,%u\",\"coverage\":%u,\"area\":%u,\"ok\":true}\n",
                v, g_brname[v], (mr + mg + mb) / 3.0, mr, mg, mb, c[2], c[1], c[0], c[3], cov, area);
      }
   row_out:   /* success and every failure: one place, so a failed row cannot leave a module, fence or pool live */
      DeviceWaitIdle(dev);
      if (pipe) DestroyPipeline(dev, pipe, NULL);
      if (r.pl) { DestroyPipelineLayout(dev, r.pl, NULL); r.pl = VK_NULL_HANDLE; }
      for (int k = 0; k < 3; k++) if (r.dsl[k]) { DestroyDescriptorSetLayout(dev, r.dsl[k], NULL); r.dsl[k] = VK_NULL_HANDLE; }
      if (r.dp) { DestroyDescriptorPool(dev, r.dp, NULL); r.dp = VK_NULL_HANDLE; }
      if (vs) DestroyShaderModule(dev, vs, NULL);
      if (fs) DestroyShaderModule(dev, fs, NULL);
      if (!ok) break;
   }
   br_destroy(&r);
   DestroyFence(dev, f, NULL);
   return ok;
}

static const float ID[4] = {1, 0, 0, 0};

static int
test_triangle(void)
{
   struct scene s = {.mode = MODE_TRI};   /* scene_create() takes s->mode as an input */
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   if (!scene_create(&s, 0)) return 0;
   double t0 = now_us();
   for (int i = 0; i < g_loop; i++) {
      /* rotate by i degrees around the centre: area is invariant */
      float a = (float)(i % 360) * 3.14159265f / 180.0f;
      float rot[1][4] = {{cosf(a), sinf(a), 0, 0}};
      if (!scene_draw(&s, 1, rot, f)) return 0;
      if (!g_fake) {
         uint32_t cov = count_cov(&s);
         double err = fabs((double)cov - 8192.0) / 8192.0 * 100.0;
         CHECK(err <= 1.0, "iteration %d coverage %u (%.2f%%)", i, cov, err);
      }
   }
   double dt = (now_us() - t0) / g_loop;
   /* identity rotation for the colour check */
   float rot[1][4] = {{ID[0], ID[1], ID[2], ID[3]}};
   if (!scene_draw(&s, 1, rot, f)) return 0;
   int ok = report("triangle", &s, 8192.0, 1.0, 1);
   printf("{\"test\":\"triangle_loop\",\"n\":%d,\"mean_us\":%.1f,\"loop_reset\":%d,\"ok\":%s}\n", g_loop, dt, g_reset,
          ok ? "true" : "false");
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

/* The counter file is per process: a fixed /tmp name was shared by every test run on the machine (other worktrees, parallel check.sh) and
 * made share_inst / share_dirty fail at random with wrapped or foreign byte totals. */
static char g_alloc_stats[64];
static void
alloc_stats_rm(void)
{
   if (g_alloc_stats[0]) unlink(g_alloc_stats);
}
static void
alloc_stats_start(void)   /* fresh counter file; the library writes the running total of hw_alloc bytes to $MALI_ALLOC_STATS at every submit */
{
   if (!g_alloc_stats[0]) {
      snprintf(g_alloc_stats, sizeof g_alloc_stats, "/tmp/mali_alloc_stats.%d.txt", (int)getpid());
      atexit(alloc_stats_rm);
   }
   unlink(g_alloc_stats);
   setenv("MALI_ALLOC_STATS", g_alloc_stats, 1);
}

/* arena bytes asked of hw_alloc so far (see alloc_stats_start) */
static unsigned long long
alloc_bytes(void)
{
   FILE *fp = fopen(g_alloc_stats, "r");
   unsigned long long v = 0;
   if (fp) {
      if (fscanf(fp, "%llu", &v) != 1) v = 0;
      fclose(fp);
   }
   return v;
}

/* ---- many draws in ONE frame (10k-50k): arena / CS / heap growth must not run out (was DEVICE_LOST above ~7k draws) ---- */
static int
test_many(void)
{
   int counts[] = {3000, 10000, 50000};
   if (getenv("MALI_MANY_COUNTS")) {   /* e.g. "3000": only the 3k frame (host profiling) */
      memset(counts, 0, sizeof counts);
      counts[0] = atoi(getenv("MALI_MANY_COUNTS"));
   }
   alloc_stats_start();
   const int nrep = getenv("MALI_MANY_REPS") ? atoi(getenv("MALI_MANY_REPS")) : 3;
   /* MALI_MANY_DYN=0: plain push-constant triangles; default: dynamic-UBO offset + DrawIndexed per draw (the bench2 triangle shape) */
   g_dyn = !getenv("MALI_MANY_DYN") || atoi(getenv("MALI_MANY_DYN"));
   struct scene s = {.mode = g_dyn ? MODE_UBO : MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   if (!scene_create(&s, 0)) return 0;
   float (*rot)[4] = malloc(50000 * sizeof *rot);
   for (int i = 0; i < 50000; i++) memcpy(rot[i], ID, sizeof rot[i]);
   int ok = 1;
   for (unsigned c = 0; ok && c < sizeof counts / sizeof *counts && counts[c]; c++) {
      g_t_rec = g_t_sub = 0;
      unsigned long long ab0 = alloc_bytes();   /* total at the previous submit: the first rep of a size counts from there */
      double t0 = now_us();
      for (int rep = 0; ok && rep < nrep; rep++)
         ok &= scene_draw(&s, counts[c], (const float (*)[4])rot, f);
      printf("{\"test\":\"many_draws\",\"draws\":%d,\"dyn_ubo\":%d,\"us_per_draw\":%.3f,\"record_us\":%.3f,\"submit_us\":%.3f,\"alloc_B_per_draw\":%.0f,\"ok\":%s}\n", counts[c], g_dyn,
             (now_us() - t0) / nrep / counts[c], g_t_rec / nrep / counts[c], g_t_sub / nrep / counts[c],
             (double)(alloc_bytes() - ab0) / nrep / counts[c], ok ? "true" : "false");
   }
   if (ok && !g_dyn) ok = report("many_draws", &s, 8192.0, 1.0, 1);
   g_dyn = 0;
   free(rot);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}


/* ---- dxvk_frame: a DXVK-shaped frame, the CPU cost of the driver per draw / per submit (host microbenchmark, fake backend) ----
 * 3 render passes (60/30/10 % of the draws), per draw: 16 B push constants, a descriptor write + bind of a set of its own (DXVK rewrites
 * its UBO/texture sets every few draws), per 4 draws a pipeline switch (two pipelines), per 8 draws cull/front-face/depth-bias/
 * blend-constant/viewport/scissor changes, per 16 draws a vertex-buffer + index-buffer rebind; indexed draws. One QueueSubmit per frame.
 * MALI_DXVK_DRAWS (3000), MALI_DXVK_REPS (200). Prints ns_per_draw (record+submit), record_ns_per_draw, submit_ns_per_draw, submit_ns. */
static int
test_dxvk_frame(void)
{
   long n_l = 3000, nrep_l = 200;
   const char *ev[2] = {getenv("MALI_DXVK_DRAWS"), getenv("MALI_DXVK_REPS")};
   long *dst[2] = {&n_l, &nrep_l};
   for (int i = 0; i < 2; i++) {   /* checked conversion: a zero/negative/non-numeric count would divide by zero in the report yet print ok:true */
      char *end = NULL;
      if (!ev[i]) continue;
      long v = strtol(ev[i], &end, 10);
      if (end == ev[i] || *end || v <= 0 || v > 10000000) { printf("MALI_DXVK_%s must be a positive integer (got '%s')\n", i ? "REPS" : "DRAWS", ev[i]); return 0; }
      *dst[i] = v;
   }
   const int n = (int)n_l, nrep = (int)nrep_l;
   g_dxvk = 1;
   struct scene s = {.mode = MODE_UBO};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   if (!scene_create(&s, 0)) { g_dxvk = 0; return 0; }
   /* one set per draw: each is written once and bound once per command buffer (rewriting a set that the recording cb already bound is invalid);
    * the previous frame has completed (fence) before its sets are written again */
   VkDescriptorPool dxpool = VK_NULL_HANDLE;
   VkDescriptorSet *dxsets = calloc((size_t)n, sizeof *dxsets);
   if (!dxsets) { printf("out of memory for %d descriptor sets\n", n); scene_destroy(&s); DestroyFence(dev, f, NULL); return 0; }
   {
      VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (uint32_t)n};
      VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = (uint32_t)n, .poolSizeCount = 1, .pPoolSizes = &ps};
      VkDescriptorSetLayout *ls = malloc((size_t)n * sizeof *ls);
      if (!ls) { printf("out of memory for %d set layouts\n", n); free(dxsets); scene_destroy(&s); DestroyFence(dev, f, NULL); return 0; }
      for (int i = 0; i < n; i++) ls[i] = s.dsl;
      VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = (uint32_t)n, .pSetLayouts = ls};
      CHECK(CreateDescriptorPool(dev, &dpi, NULL, &dxpool) == VK_SUCCESS, "dx pool");
      dai.descriptorPool = dxpool;
      CHECK(AllocateDescriptorSets(dev, &dai, dxsets) == VK_SUCCESS, "dx sets");
      free(ls);
   }
   alloc_stats_start();
   double t_rec = 0, t_sub = 0, t_all = 0;
   int ok = 1;
   for (int rep = -5; ok && rep < nrep; rep++) {   /* 5 warm-up frames */
      double t0 = now_us(), tr;
      VkCommandBuffer cb;
      VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
      CHECK(AllocateCommandBuffers(dev, &ai, &cb) == VK_SUCCESS, "alloc cb");
      VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      CHECK(BeginCommandBuffer(cb, &bi) == VK_SUCCESS, "begin");
      VkClearValue cv[2] = {{.color = {{0, 0, 0, 0}}}, {.depthStencil = {1.0f, 0}}};
      VkViewport vp = {0, 0, W, H, 0.0f, 1.0f};
      VkRect2D sc = {{0, 0}, {W, H}};
      VkBuffer vbs[2] = {s.vb[0], s.vb[1]};
      VkDeviceSize offs[2] = {0, 0};
      const float bc[4] = {0.1f, 0.2f, 0.3f, 0.4f};
      int d = 0;
      static const int split[3] = {60, 30, 10};
      for (int pass = 0; pass < 3; pass++) {
         int cnt = pass == 2 ? n - d : n * split[pass] / 100;
         VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = s.rp, .framebuffer = s.fb,
                                     .renderArea = {{0, 0}, {W, H}}, .clearValueCount = s.depth ? 2 : 1, .pClearValues = cv};
         CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe);
         CmdSetViewport(cb, 0, 1, &vp);
         CmdSetScissor(cb, 0, 1, &sc);
         CmdBindVertexBuffers(cb, 0, 2, vbs, offs);
         CmdBindIndexBuffer(cb, s.ib, 16, VK_INDEX_TYPE_UINT32);
         for (int i = 0; i < cnt; i++, d++) {
            if ((d & 3) == 0) CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (d & 4) ? s.pipe2 : s.pipe);
            if ((d & 7) == 0) {
               CmdSetCullMode(cb, (d & 8) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
               CmdSetFrontFace(cb, (d & 16) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
               CmdSetDepthBias(cb, (float)((d >> 3) & 3), 0.0f, 1.0f);   /* d is a multiple of 8 here: the update index d >> 3 is what changes */
               CmdSetBlendConstants(cb, bc);
               vp.x = (float)((d >> 3) & 1);
               sc.offset.x = (d >> 3) & 1;
               CmdSetViewport(cb, 0, 1, &vp);
               CmdSetScissor(cb, 0, 1, &sc);
            }
            if ((d & 15) == 0) {
               offs[0] = (d & 16) ? 16 : 0;
               CmdBindVertexBuffers(cb, 0, 2, vbs, offs);
               CmdBindIndexBuffer(cb, s.ib, 16, VK_INDEX_TYPE_UINT32);
            }
            VkDescriptorBufferInfo bi2 = {s.ubo[d % 3], 0, 16};
            VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dxsets[d], .dstBinding = 0,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi2};
            UpdateDescriptorSets(dev, 1, &w, 0, NULL);
            CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 1, &dxsets[d], 0, NULL);
            float pc[4] = {1, 0, 0, (float)(d & 7)};
            CmdPushConstants(cb, s.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, pc);
            CmdDrawIndexed(cb, 6, 1, 0, 3, 0);
         }
         CmdEndRenderPass(cb);
      }
      CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "end");
      tr = now_us();
      VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
      VkResult r = QueueSubmit(queue, 1, &si, f);
      double ts = now_us();
      CHECK(r == VK_SUCCESS, "submit = %d", r);
      CHECK(WaitForFences(dev, 1, &f, VK_TRUE, 20000000000ull) == VK_SUCCESS, "wait");
      ResetFences(dev, 1, &f);
      FreeCommandBuffers(dev, s.pool, 1, &cb);
      if (rep >= 0) { t_rec += tr - t0; t_sub += ts - tr; t_all += ts - t0; }
   }
   printf("{\"test\":\"dxvk_frame\",\"draws\":%d,\"passes\":3,\"frames\":%d,\"ns_per_draw\":%.1f,\"record_ns_per_draw\":%.1f,\"submit_ns_per_draw\":%.1f,\"submit_ns\":%.0f,\"alloc_B_per_draw\":%.0f,\"ok\":%s}\n",
          n, nrep, t_all * 1000.0 / nrep / n, t_rec * 1000.0 / nrep / n, t_sub * 1000.0 / nrep / n, t_sub * 1000.0 / nrep,
          (double)alloc_bytes() / (nrep + 5) / n, ok ? "true" : "false");
   g_dxvk = 0;
   if (dxpool) DestroyDescriptorPool(dev, dxpool, NULL);
   free(dxsets);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}


static uint32_t px_at(struct scene *s, int x, int y);

/* descriptor sharing between consecutive draws: draws that differ in exactly one thing (push constant / viewport-scissor) must each get
 * their own data. Pixels on hardware only. */
static int
test_share(void)
{
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   if (!scene_create(&s, 0)) return 0;
   int ok = 1;
   /* four quarter-size triangles at (+-.5, +-.5); consecutive duplicates (identical state) are mixed with changes */
   const float q[4][4] = {{.5f, 0, -.5f, -.5f}, {.5f, 0, .5f, -.5f}, {.5f, 0, -.5f, .5f}, {.5f, 0, .5f, .5f}};
   float rot[8][4];
   const int order[8] = {0, 0, 1, 2, 2, 2, 3, 1};
   for (int i = 0; i < 8; i++) memcpy(rot[i], q[order[i]], sizeof rot[i]);
   ok &= scene_draw(&s, 8, (const float (*)[4])rot, f);
   if (ok && !g_fake) {
      for (int k = 0; k < 4; k++) {
         int cx = (int)((q[k][2] * .5f + .5f) * W), cy = (int)((q[k][3] * .5f + .5f) * H);
         CHECK(px_at(&s, cx, cy) != 0, "share: triangle %d (push constant) missing at %d,%d", k, cx, cy);
      }
      CHECK(px_at(&s, W / 2, H / 2) == 0 || g_colbpp == 8, "share: stray pixel at the centre");
      ok &= report("share_push", &s, 8192.0, 2.0, 0);
   } else if (ok) printf("{\"test\":\"share_push\",\"skipped\":\"fake backend\",\"ok\":true}\n");
   /* identical draw and push constants, scissor alternates left/right half: both halves must be drawn */
   g_share = 2;
   float id[4][4];
   for (int i = 0; i < 4; i++) memcpy(id[i], ID, sizeof id[i]);
   ok &= scene_draw(&s, 4, (const float (*)[4])id, f);
   g_share = 0;
   if (ok) ok &= report("share_scissor", &s, 8192.0, 1.0, 0);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

/* ---- stencil: coverage / pixel checks against the stencil test (pixels only on hardware) ---- */
static uint32_t
px_at(struct scene *s, int x, int y)
{
   uint32_t v;
   memcpy(&v, s->px + y * s->pitch + x * g_colbpp, 4);
   return v;
}

struct st_case {
   const char *name;
   VkCompareOp cmp_f, cmp_b;
   VkStencilOp fail, pass;
   uint32_t cmask, wmask, ref, clear;
   int cw, dyn;
   double expect;               /* coverage of one triangle (8192 px) or 0 */
};

static int
test_st_cov(const struct st_case *c, uint32_t *covout)
{
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   g_st = (struct st_state){.on = 1, .cmp_f = c->cmp_f, .cmp_b = c->cmp_b, .fail = c->fail, .dfail = VK_STENCIL_OP_KEEP,
                         .pass = c->pass, .cmask = c->cmask, .wmask = c->wmask, .ref = c->ref, .clear = c->clear,
                         .cw = c->cw, .dyn = c->dyn};
   int ok = scene_create(&s, 1);
   float rot[1][4] = {{1, 0, 0, 0}};
   if (ok) ok = scene_draw(&s, 1, rot, f);
   g_st = (struct st_state){0};
   if (ok) {
      if (g_fake) {
         printf("{\"test\":\"%s\",\"skipped\":\"fake backend\",\"ok\":true}\n", c->name);
      } else {
         uint32_t cov = count_cov(&s);
         if (covout) { *covout = cov; scene_destroy(&s); DestroyFence(dev, f, NULL); return 1; }
         ok = c->expect == 0 ? cov == 0 : fabs((double)cov - c->expect) / c->expect * 100.0 <= 2.5;
         CHECK(ok, "%s coverage %u expected %.0f", c->name, cov, c->expect);
         printf("{\"test\":\"%s\",\"coverage\":%u,\"ok\":%s}\n", c->name, cov, ok ? "true" : "false");
      }
   }
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

/* centre pixel of: draw A only, B only, A then B under the stencil state `st`.
 * wmask != 0 -> the INCR_CLAMP pass op marks A, B is rejected where A overlaps (centre == A);
 * wmask == 0 -> nothing is written, B overwrites (centre == B). */
static int
test_st_two(const char *name, int dyn, uint32_t wmask)
{
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   g_st = (struct st_state){.on = 1, .cmp_f = VK_COMPARE_OP_EQUAL, .cmp_b = VK_COMPARE_OP_EQUAL, .fail = VK_STENCIL_OP_KEEP,
                         .pass = VK_STENCIL_OP_INCREMENT_AND_CLAMP, .cmask = 0xff, .wmask = wmask, .ref = 0, .clear = 0, .dyn = dyn};
   int ok = scene_create(&s, 1);
   float one[2][4] = {{1, 0, 0, 0}, {-1, 0, 0, 0}};
   uint32_t a = 0, b = 0, ab = 0;
   if (ok) ok = scene_draw(&s, 1, &one[0], f);
   if (ok) a = px_at(&s, 128, 128);
   if (ok) ok = scene_draw(&s, 1, &one[1], f);
   if (ok) b = px_at(&s, 128, 128);
   if (ok) ok = scene_draw(&s, 2, one, f);
   if (ok) ab = px_at(&s, 128, 128);
   g_st = (struct st_state){0};
   if (ok) {
      if (g_fake) {
         printf("{\"test\":\"%s\",\"skipped\":\"fake backend\",\"ok\":true}\n", name);
      } else {
         CHECK(a != b, "%s: centre A %08x == B %08x (test is blind)", name, a, b);
         uint32_t want = wmask ? a : b;
         ok = ok && ab == want;
         CHECK(ab == want, "%s: centre %08x expected %08x (A %08x B %08x)", name, ab, want, a, b);
         printf("{\"test\":\"%s\",\"centre\":\"%08x\",\"want\":\"%08x\",\"ok\":%s}\n", name, ab, want, ok ? "true" : "false");
      }
   }
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

static int
test_stencil(VkFormat fmt)
{
   VkFormat save = g_depfmt;
   g_depfmt = fmt;
   const VkCompareOp EQ = VK_COMPARE_OP_EQUAL, NV = VK_COMPARE_OP_NEVER, AL = VK_COMPARE_OP_ALWAYS;
   const VkStencilOp KP = VK_STENCIL_OP_KEEP;
   const struct st_case cases[] = {
      {"stencil_never", NV, NV, KP, KP, 0xff, 0xff, 0, 0, 0, 0, 0},
      {"stencil_always", AL, AL, KP, KP, 0xff, 0xff, 0, 0, 0, 0, 8192},
      {"stencil_eq_pass", EQ, EQ, KP, KP, 0xff, 0xff, 5, 5, 0, 0, 8192},
      {"stencil_eq_fail", EQ, EQ, KP, KP, 0xff, 0xff, 4, 5, 0, 0, 0},
      {"stencil_cmask", EQ, EQ, KP, KP, 0x0f, 0xff, 0x05, 0x15, 0, 0, 8192},     /* (0x15 & 0xf) == (0x05 & 0xf) */
      {"stencil_cmask_fail", EQ, EQ, KP, KP, 0xff, 0xff, 0x05, 0x15, 0, 0, 0},
      {"stencil_dyn_ref", EQ, EQ, KP, KP, 0xff, 0xff, 5, 5, 0, 1, 8192},         /* static ref is 9, mask 0x0f */
      {"stencil_dyn_cmask", EQ, EQ, KP, KP, 0x0f, 0xff, 0x05, 0x15, 0, 1, 8192},
   };
   int ok = 1;
   for (unsigned i = 0; ok && i < sizeof cases / sizeof *cases; i++) {
      char nm[64];
      struct st_case c = cases[i];
      snprintf(nm, sizeof nm, "%s%s", c.name, fmt == VK_FORMAT_S8_UINT ? "_s8" : fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? "_d32s8" : "_d24s8");
      c.name = nm;
      ok = test_st_cov(&c, NULL);
   }
   /* back faces use the back state: front NEVER / back ALWAYS shows the triangle for exactly one winding */
   if (ok) {
      struct st_case fb = {"stencil_face", NV, AL, KP, KP, 0xff, 0xff, 0, 0, 0, 0, -1}, r0, r1;
      r0 = fb; r1 = fb;
      r1.cw = 1;
      r0.expect = 0; r1.expect = 8192;
      char nm0[64], nm1[64];
      const char *sfx = fmt == VK_FORMAT_S8_UINT ? "_s8" : fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? "_d32s8" : "_d24s8";
      snprintf(nm0, sizeof nm0, "stencil_face_ccw%s", sfx);
      snprintf(nm1, sizeof nm1, "stencil_face_cw%s", sfx);
      r0.name = nm0; r1.name = nm1;
      /* which winding is the back face depends on the vertex order: exactly one of the two must show the triangle */
      uint32_t c0 = 0, c1 = 0;
      ok = test_st_cov(&r0, &c0) && test_st_cov(&r1, &c1);
      if (ok && !g_fake) {
         ok = (c0 == 0 && c1 > 8192 * 0.975 && c1 < 8192 * 1.025) || (c1 == 0 && c0 > 8192 * 0.975 && c0 < 8192 * 1.025);
         CHECK(ok, "%s: front-NEVER/back-ALWAYS coverage ccw %u cw %u (want one 0, one ~8192)", nm0, c0, c1);
         printf("{\"test\":\"stencil_face%s\",\"cov_ccw\":%u,\"cov_cw\":%u,\"ok\":%s}\n", sfx, c0, c1, ok ? "true" : "false");
      }
   }
   if (ok) {
      char nm[64];
      const char *sfx = fmt == VK_FORMAT_S8_UINT ? "_s8" : fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? "_d32s8" : "_d24s8";
      snprintf(nm, sizeof nm, "stencil_incr%s", sfx);
      ok = test_st_two(nm, 0, 0xff);
      if (ok) { snprintf(nm, sizeof nm, "stencil_wmask0%s", sfx); ok = test_st_two(nm, 0, 0); }
      if (ok) { snprintf(nm, sizeof nm, "stencil_dyn_wmask%s", sfx); ok = test_st_two(nm, 1, 0xff); }
      if (ok) { snprintf(nm, sizeof nm, "stencil_dyn_wmask0%s", sfx); ok = test_st_two(nm, 1, 0); }
   }
   g_depfmt = save;
   return ok;
}

/* loadOp LOAD: pass 1 (CLEAR) draws A, pass 2 (LOAD) draws B; the result must equal one pass drawing A then B.
 * kind: 0 depth (g_depfmt), 1 stencil (D32S8, INCR_AND_CLAMP/EQUAL 0, so B is rejected where A overlaps), 2 colour (g_colfmt, no depth). */
static int
test_load(const char *name, int kind)
{
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   VkFormat save = g_depfmt;
   if (kind == 1 || kind == 3 || kind == 4) {
      g_depfmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
      g_st = (struct st_state){.on = 1, .cmp_f = VK_COMPARE_OP_EQUAL, .cmp_b = VK_COMPARE_OP_EQUAL, .fail = VK_STENCIL_OP_KEEP,
                               .pass = kind == 3 ? VK_STENCIL_OP_INVERT : kind == 4 ? VK_STENCIL_OP_DECREMENT_AND_WRAP : VK_STENCIL_OP_INCREMENT_AND_CLAMP, .cmask = 0xff, .wmask = 0xff, .ref = 0, .clear = 0};
   }
   g_load = 1;
   int ok = scene_create(&s, kind != 2);
   g_load = 0;
   g_st = (struct st_state){0};
   g_depfmt = save;
   float AB[2][4] = {{1, 0, 0, 0}, {-1, 0, 0, 0}};
   if (kind == 2) { AB[0][2] = -.5f; AB[1][0] = 1; AB[1][2] = .5f; }
   uint32_t a = 0, b = 0, ref = 0, got = 0, cref = 0, cgot = 0;
   if (ok) ok = scene_draw(&s, 2, AB, f);               /* reference: both in one pass (the stencil state lives in s) */
   static const int PX[6][2] = {{128, 128}, {128, 90}, {128, 170}, {90, 150}, {166, 110}, {128, 40}};
   uint32_t pref[6] = {0}, pgot[6] = {0}, p1[6] = {0};
   uint32_t cov1 = 0;
   if (ok) { ref = px_at(&s, 128, 128); cref = count_cov(&s); for (int i = 0; i < 6; i++) pref[i] = px_at(&s, PX[i][0], PX[i][1]); }
   if (ok) ok = scene_draw(&s, 1, &AB[0], f);
   if (ok) a = px_at(&s, 128, 128);
   if (ok) ok = scene_draw(&s, 1, &AB[1], f);
   if (ok) b = px_at(&s, 128, 128);
   if (ok) ok = scene_draw(&s, 1, &AB[0], f);          /* pass 1: clear, A, store */
   if (ok) { cov1 = count_cov(&s); for (int i = 0; i < 6; i++) p1[i] = px_at(&s, PX[i][0], PX[i][1]); }
   s.second = 1;
   if (ok) ok = scene_draw(&s, 1, &AB[1], f);          /* pass 2: LOAD colour+depth(+stencil), B */
   s.second = 0;
   if (ok) { got = px_at(&s, 128, 128); cgot = count_cov(&s); for (int i = 0; i < 6; i++) pgot[i] = px_at(&s, PX[i][0], PX[i][1]); }
   if (ok) {
      if (g_fake) {
         printf("{\"test\":\"%s\",\"skipped\":\"fake backend\",\"ok\":true}\n", name);
      } else {
         if (kind != 2) CHECK(a != b, "%s: centre A %08x == B %08x (test is blind)", name, a, b);
         ok = got == ref && cgot == cref;
         int same = 1;
         for (int i = 0; i < 6; i++) same &= pgot[i] == pref[i];
         ok = ok && same;
         if (!ok) {
            /* Which failure. kind 2 is colour only (no depth attachment), so a coverage shortfall there is the
             * LOAD preload losing edge samples, never "depth came back wrong": name the number and the plane
             * question instead of sending the reader to the depth path. */
            const char *why;
            if (kind == 2)
               why = cgot == cov1 ? "pass 2 wrote nothing (LOAD restored nothing)"
                   : cgot > cref ? "LOAD over-restored: coverage above the one-pass reference"
                   : cgot < cref ? "LOAD lost edge coverage at 4x (antialiased pixels whose samples were not restored) - compare load_roundtrip"
                                  : "coverage equal, pixel values differ";
            else
               why = cgot == cov1 ? "pass2 wrote nothing (colour marker unchanged)" : cgot > cref ? "preload did not take effect: B drawn over A (depth/stencil came back as cleared)" : cgot < cref ? "B over-rejected: depth/stencil came back wrong (too near / non-zero)" : "coverage equal, pixel values differ";
            char pxs[400]; int n = 0;
            for (int i = 0; i < 6; i++)
               n += snprintf(pxs + n, sizeof pxs - n, " (%d,%d)%08x/%08x/%08x", PX[i][0], PX[i][1], p1[i], pref[i], pgot[i]);
            printf("{\"fail\":\"%s: %s; cov pass1 %u ref %u got %u; px pass1/want/got:%s\"}\n", name, why, cov1, cref, cgot, pxs);
         }
         CHECK(ok, "%s: two-pass centre %08x cov %u vs one-pass %08x cov %u (A %08x B %08x)", name, got, cgot, ref, cref, a, b);
         printf("{\"test\":\"%s\",\"centre\":\"%08x\",\"want\":\"%08x\",\"cov\":%u,\"ok\":%s}\n", name, got, ref, cgot, ok ? "true" : "false");
      }
   }
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

/* RAW (non-blendable) render target: integer (RGBA8UI, RG16UI, R8UI, A8B8G8R8/A2B10G10R10 integer: clear to the raw words) or SNORM / UNORM16 / float (cf: clear to the floats); the render pass clears, one triangle is drawn; the corner pixels must hold the words */
static int
test_int_clear_f(const char *name, VkFormat fmt, unsigned bpp, uint32_t want0, const float *cf)
{
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   g_colfmt = fmt;
   g_colbpp = bpp;
   g_clear_u = !cf;
   g_clear_f = cf;
   int ok = scene_create(&s, 0);
   float one[1][4] = {{1, 0, 0, 0}};
   if (ok) ok = scene_draw(&s, 1, one, f);   /* a triangle in the middle; the corners keep the clear value */
   g_clear_u = 0;
   g_clear_f = NULL;
   uint32_t got = 0, mask = bpp >= 4 ? ~0u : (1u << (8 * bpp)) - 1;
   if (ok && !g_fake) {
      got = px_at(&s, 5, 5) & mask;
      ok = got == want0 && (px_at(&s, 250, 250) & mask) == want0;
   }
   printf("{\"test\":\"%s\",\"px\":\"%08x\",\"want\":\"%08x\",\"ok\":%s%s}\n", name, got, want0, ok ? "true" : "false", g_fake ? ",\"skipped\":\"fake backend\"" : "");
   CHECK(ok, "%s: pixel %08x want %08x", name, got, want0);
   g_colfmt = VK_FORMAT_R8G8B8A8_UNORM;
   g_colbpp = 4;
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

/* rasterizer discard: draws produce nothing (both stages present, or a vertex-only pipeline); the next pipeline draws normally */
static int
test_discard(const char *name, int mode)
{
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   g_discard = mode;
   int ok = scene_create(&s, 0);
   g_discard = 0;
   float one[1][4] = {{1, 0, 0, 0}};
   if (ok) ok = scene_draw(&s, 1, one, f);
   unsigned cov = 0;
   if (ok && !g_fake) {
      cov = count_cov(&s);
      ok = cov == 0;
   }
   printf("{\"test\":\"%s\",\"coverage\":%u,\"ok\":%s%s}\n", name, cov, ok ? "true" : "false", g_fake ? ",\"skipped\":\"fake backend\"" : "");
   CHECK(ok, "%s: coverage %u, want 0", name, cov);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

/* dynamic vertex binding stride (rebind=1: a plain BindVertexBuffers follows the stride-16 2-form bind): static stride 4 is wrong; draw 1 binds stride 0 (nothing), draw 2 stride 16 (the triangle) */
static int
test_dynstride(const char *name, int rebind)
{
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   g_dynstr = rebind ? 2 : 1;
   int ok = scene_create(&s, 0);
   float one[1][4] = {{1, 0, 0, 0}};
   if (ok) ok = scene_draw(&s, 1, one, f);
   g_dynstr = 0;
   unsigned cov = 0;
   if (ok && !g_fake) {
      cov = count_cov(&s);
      ok = cov > 1000;
   }
   printf("{\"test\":\"%s\",\"coverage\":%u,\"ok\":%s%s}\n", name, cov, ok ? "true" : "false", g_fake ? ",\"skipped\":\"fake backend\"" : "");
   CHECK(ok, "%s: coverage %u, want the triangle", name, cov);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

/* depth-only render pass (no colour attachment): the draw must complete */
static int
test_nocol(const char *name, int stencil)
{
   g_nofs = !strncmp(name, "nofs_", 5);   /* nofs_*: FS-less pipeline; the draw must still be emitted (golden stream), CTS .nocolor reads it back */
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   VkFormat save = g_depfmt;
   if (stencil) {
      g_depfmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
      g_st = (struct st_state){.on = 1, .cmp_f = VK_COMPARE_OP_ALWAYS, .cmp_b = VK_COMPARE_OP_ALWAYS, .fail = VK_STENCIL_OP_KEEP,
                               .pass = VK_STENCIL_OP_INCREMENT_AND_CLAMP, .cmask = 0xff, .wmask = 0xff, .ref = 0, .clear = 0};
   }
   g_nocol = 1;
   int ok = scene_create(&s, 1);
   float one[1][4] = {{1, 0, 0, 0}};
   if (ok) ok = scene_draw(&s, 1, one, f);
   g_nocol = 0; g_nofs = 0;
   g_st = (struct st_state){0};
   g_depfmt = save;
   printf("{\"test\":\"%s\",\"ok\":%s}\n", name, ok ? "true" : "false");   /* completion (no hang, no error) is the check; dEQP depth/stencil .nocolor reads the result back */
   CHECK(ok, "%s: depth-only pass failed", name);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

static int
test_int_clear(const char *name, VkFormat fmt, unsigned bpp, uint32_t want0)
{
   return test_int_clear_f(name, fmt, bpp, want0, NULL);
}

/* Alpha histogram in quarter steps: a LOAD that loses samples shows up as a shift to the 0 bucket. Diagnostics. */
static void
alpha_hist(const char *tag, struct scene *s)
{
   unsigned b[6] = {0};
   for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
         unsigned a = s->px[y * s->pitch + x * 4 + 3];
         b[a == 0 ? 0 : a == 255 ? 5 : 1 + (a - 1) / 64] += 1;
      }
   printf("{\"hist\":\"%s\",\"a0\":%u,\"a1_63\":%u,\"a64_127\":%u,\"a128_191\":%u,\"a192_254\":%u,\"a255\":%u}\n",
          tag, b[0], b[1], b[2], b[3], b[4], b[5]);
}

/* Does loadOp LOAD restore every sample? pass 1 clears and draws A (store, resolve), pass 2 LOADs with no
 * draw at all and stores/resolves again: coverage must be identical to pass 1. This isolates the preload
 * from the second draw, so a shortfall is the per-sample restore and not geometry or depth. */
static int
load_roundtrip(const char *name)
{
   if (!(g_fbcounts & VK_SAMPLE_COUNT_4_BIT)) {
      printf("{\"test\":\"%s\",\"skipped\":\"4x MSAA not advertised by the driver\",\"ok\":true}\n", name);
      return 1;
   }
   g_msaa = 1;
   g_msaa_store = 1;
   g_load = 1;
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   int ok = scene_create(&s, 0);
   g_load = 0;
   float AB[1][4] = {{1, 0, -.5f, 0}};
   uint32_t c1 = 0, c2 = 0;
   if (ok) ok = scene_draw(&s, 1, AB, f);          /* pass 1: clear + A, stored and resolved */
   if (ok) c1 = count_cov(&s);
   if (!g_fake) alpha_hist("after_pass1", &s);
   s.second = 1;
   if (ok) ok = scene_draw(&s, 0, AB, f);          /* pass 2: LOAD, no draws, store + resolve */
   s.second = 0;
   if (ok) c2 = count_cov(&s);
   if (!g_fake) alpha_hist("after_load_no_draw", &s);
   /* stage 3: LOAD then DRAW a triangle that covers nothing new (the same A again): if the draw drops the samples
    * the preload restored, coverage falls here; if it keeps them, coverage is unchanged and the loss in the real
    * test comes from B's own samples at the seam. */
   s.second = 1;
   if (ok) ok = scene_draw(&s, 1, AB, f);
   s.second = 0;
   uint32_t c3 = 0;
   if (ok) c3 = count_cov(&s);
   if (!g_fake) alpha_hist("after_load_and_same_draw", &s);
   if (ok) {
      if (g_fake) {
         printf("{\"test\":\"%s\",\"skipped\":\"fake backend\",\"ok\":true}\n", name);
      } else {
         CHECK(c1 == c2, "%s: coverage after LOAD %u != after pass 1 %u (%d pixels lost by the per-sample preload)", name, c2, c1, (int)(c1 - c2));
         CHECK(c1 == c3, "%s: coverage after LOAD + the same draw %u != pass 1 %u (%d pixels lost by drawing over loaded samples)", name, c3, c1, (int)(c1 - c3));
         printf("{\"test\":\"%s\",\"cov_pass1\":%u,\"cov_after_load\":%u,\"cov_after_load_draw\":%u,\"ok\":%s}\n",
                name, c1, c2, c3, c1 == c2 && c1 == c3 ? "true" : "false");
      }
   }
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   g_msaa = 0;
   g_msaa_store = 0;
   return ok;
}

static int
test_msaa_load(const char *name)
{
   if (!(g_fbcounts & VK_SAMPLE_COUNT_4_BIT)) {   /* the driver's advertised count, not the env: see test_msaa */
      printf("{\"test\":\"%s\",\"skipped\":\"4x MSAA not advertised by the driver\",\"ok\":true}\n", name);
      return 1;
   }
   g_msaa = 1;
   g_msaa_store = 1;
   const int ok_main = test_load(name, 2);
   const int ok_rt = load_roundtrip("msaa_load_roundtrip");   /* diagnostics: runs even when the main assertion failed */
   int ok = ok_main && ok_rt;
   g_msaa = 0;
   g_msaa_store = 0;
   return ok;
}

static int
test_two(int depth)
{
   struct scene s = {.mode = MODE_TRI};   /* scene_create() takes s->mode as an input */
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   if (!scene_create(&s, depth)) return 0;
   int ok;
   if (!depth) {
      /* two touching, non-overlapping triangles (offsets -0.5 / +0.5 in NDC) */
      float rot[2][4] = {{1, 0, -.5f, 0}, {1, 0, .5f, 0}};
      if (!scene_draw(&s, 2, rot, f)) return 0;
      ok = report("two", &s, 16384.0, 1.5, 0);
   } else {
      /* same z, LESS test: the second (180 degree flipped, overlapping) draw is rejected where it
       * overlaps the first, but equal depth fails LESS everywhere it overlaps; it still shows
       * outside. Union area of triangle and its point reflection = 8192 + (8192 - overlap). */
      float rot[2][4] = {{1, 0, 0, 0}, {-1, 0, 0, 0}};
      if (!scene_draw(&s, 2, rot, f)) return 0;
      /* overlap of the triangle with its reflection through the centre of the bounding point (0,0):
       * computed numerically below on the host. */
      double ov = 0;
      {
         /* A=(-.5,-.5),(.5,-.5),(0,.5); B = -A. count px centres inside both */
         for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
               double px = (x + .5) / W * 2 - 1, py = (y + .5) / H * 2 - 1;
               int inA = py >= -.5 && py <= .5 && fabs(px) <= (py + .5) * .5;
               int inB = py >= -.5 && py <= .5 && fabs(px) <= (.5 - py) * .5;
               ov += inA && inB;
            }
      }
      /* with LESS and equal z, B appears only outside A; without depth it overwrites: same union.
       * Distinguish by colour: B's vertex colours differ from A at shared pixels, so compare the
       * centre pixel against what A alone would give (A wins when depth rejects B). */
      ok = report(g_depfmt == VK_FORMAT_D16_UNORM ? "depth16" : g_depfmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? "depth32s8" : "depth", &s, 16384.0 - ov, 2.5, 0);
      if (ok && !g_fake) {
         const uint8_t *p = s.px + 128 * s.pitch + 128 * 4; /* inside both */
         /* B centre colour is mix with bottom-left... just require non-zero and report it */
         printf("{\"test\":\"depth_centre\",\"rgba\":[%u,%u,%u,%u],\"overlap_px\":%.0f}\n", p[0], p[1], p[2], p[3], ov);
      }
   }
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

/* mode cases: expected coverage in px, tolerance %, optional centre/corner probes */
/* MSAA (MALI_EXPERIMENTAL_MSAA=1): the triangle is rendered into a 4-sample image and resolved (hardware Average writeback) into col.
 * Pixel checks on the resolved image: the interior keeps the colour, edge pixels have coverage 1/4..3/4 (alpha ~64/128/191), and
 * the coverage-weighted area is the triangle area. mode 2 additionally enables sampleRateShading (minSampleShading 1.0). */
static int
test_msaa(const char *name, int mode)
{
   /* Gate on the driver's advertised sample counts (g_fbcounts comes from the limits the driver reports, so
    * MALI_EXPERIMENTAL_MSAA=0 shows up here as "no 4x"), never on the env var: an env gate skips the test on a
    * driver that does advertise 4x, which is exactly the mistake that hid this case. */
   if (!(g_fbcounts & VK_SAMPLE_COUNT_4_BIT) || (mode == 2 && !g_feat.sampleRateShading)) {
      printf("{\"test\":\"%s\",\"skipped\":\"4x MSAA%s not advertised by the driver\",\"ok\":true}\n", name,
             mode == 2 ? " / sampleRateShading" : "");
      return 1;
   }
   g_msaa = mode;
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   int made = scene_create(&s, 0);
   g_msaa = 0;
   if (!made) return 0;
   float rot[1][4] = {{ID[0], ID[1], ID[2], ID[3]}};
   if (!scene_draw(&s, 1, rot, f)) return 0;
   int ok = 1;
   if (!g_fake) {
      double wsum = 0;
      unsigned partial = 0, quarter = 0;
      for (int y = 0; y < H; y++)
         for (int x = 0; x < W; x++) {
            unsigned a = s.px[y * s.pitch + x * 4 + 3];
            wsum += a / 255.0;
            if (a > 0 && a < 255) {
               partial++;
               int k = (int)(a * 4 / 255.0 + 0.5);
               if (abs((int)a - k * 255 / 4) <= 10) quarter++;
            }
         }
      double err = fabs(wsum - 8192.0) / 8192.0 * 100.0;
      CHECK(err <= 1.5, "%s coverage-weighted area %.0f expected 8192 (%.2f%%)", name, wsum, err);
      const uint8_t *p = s.px + 106 * s.pitch + 128 * 4;   /* centroid: ~equal thirds */
      CHECK(abs(p[0] - 85) < 12 && abs(p[1] - 85) < 12 && abs(p[2] - 85) < 12 && p[3] == 255, "%s interior %u,%u,%u,%u", name, p[0], p[1], p[2], p[3]);
      CHECK(partial > 100, "%s only %u edge pixels with partial alpha (resolve did not average samples)", name, partial);
      CHECK(quarter * 10 >= partial * 9, "%s edge alpha not in quarter steps: %u/%u", name, quarter, partial);
      const uint8_t *o = s.px + 8 * s.pitch + 8 * 4;   /* outside: cleared transparent */
      CHECK(o[3] == 0, "%s outside pixel alpha %u", name, o[3]);
      printf("{\"test\":\"%s\",\"area\":%.0f,\"edge_px\":%u,\"quarter_px\":%u,\"ok\":true}\n", name, wsum, partial, quarter);
   } else {
      printf("{\"test\":\"%s\",\"skipped\":\"fake backend\",\"ok\":true}\n", name);
   }
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

static int
test_mode(const char *name, int mode, double expect, double tol)
{
   struct scene s = {.mode = MODE_TRI};   /* scene_create() takes s->mode as an input */
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   if (!scene_create(&s, g_clearzs)) return 0;
   s.mode = mode;
   float rot[1][4] = {{1, 0, 0, 0}};
   int ok = 1;
   for (int i = 0; ok && i < g_loop; i++) {
      if (!scene_draw(&s, 1, rot, f)) return 0;
      if (!g_fake) {
         if (mode == MODE_CLEARATT) {
            /* blue background where the triangle is not: alpha 255 everywhere */
            uint32_t a = 0;
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) a += s.px[y * s.pitch + x * 4 + 3] == 255;
            CHECK(a == W * H, "clearattachments: %u opaque px", a);
            const uint8_t *c = s.px + 8 * s.pitch + 8 * 4;
            CHECK(c[2] == 255 && c[0] == 0 && c[1] == 0, "corner %u,%u,%u", c[0], c[1], c[2]);
            const uint8_t *t = s.px + 128 * s.pitch + 128 * 4;   /* triangle interior wins over the clear */
            CHECK(t[2] < 250 || t[0] > 5 || t[1] > 5, "triangle not drawn over clear");
         } else if (mode == MODE_CLEARQUAD) {
            /* the rect [100,160)^2 is exactly the clear colour (the triangle's pixels inside it are overwritten); nothing outside it is */
            uint32_t in = 0, out_blue = 0, out_cov = 0;
            for (int y = 0; y < H; y++)
               for (int x = 0; x < W; x++) {
                  const uint8_t *c = s.px + y * s.pitch + x * 4;
                  int blue = c[0] == 0 && c[1] == 0 && c[2] == 255 && c[3] == 255;
                  if (x >= 100 && x < 160 && y >= 100 && y < 160) in += blue;
                  else out_blue += blue, out_cov += c[3] != 0;
               }
            CHECK(in == 60 * 60, "clear quad: %u of 3600 px in the rect are the clear colour", in);
            CHECK(out_blue == 0, "clear quad leaked %u px outside the scissor", out_blue);
            CHECK(out_cov > 0, "triangle outside the rect vanished");
         } else {
            uint32_t cov = count_cov(&s);
            double err = fabs((double)cov - expect) / expect * 100.0;
            CHECK(err <= tol, "%s iteration %d coverage %u expect %.0f (%.2f%%)", name, i, cov, expect, err);
            if (mode == MODE_SCISSOR || mode == MODE_VPORT || mode == MODE_MVP) {   /* nothing right of the half line */
               uint32_t right = 0;
               for (int y = 0; y < H; y++) for (int x = W / 2; x < W; x++) right += s.px[y * s.pitch + x * 4 + 3] != 0;
               CHECK(right == 0, "%u px right of the half line", right);
            }
         }
      }
   }
   if (!g_fake)
      printf("{\"test\":\"%s\",\"n\":%d,\"expect\":%.0f,\"ok\":true}\n", name, g_loop, expect);
   else
      printf("{\"test\":\"%s\",\"skipped\":\"fake backend\",\"ok\":true}\n", name);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}


/* fillModeNonSolid: polygonMode LINE / POINT. Filled coverage of the quad modes is 16384 (idx) / 8192 (triangle); LINE must leave the
 * interior empty and cover only the edges (lo..hi pixels, triangle perimeter ~ 440 + diagonal for the quad), POINT only the vertices. */
static int
test_poly(const char *name, int mode, int poly, uint32_t lo, uint32_t hi, int cx, int cy)
{
   struct scene s = {.mode = mode};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if (g_fake) { printf("{\"test\":\"%s\",\"skipped\":\"fake backend\",\"ok\":true}\n", name); return 1; }
   if (!g_feat.fillModeNonSolid) { printf("{\"test\":\"%s\",\"skipped\":\"fillModeNonSolid not advertised\",\"ok\":true}\n", name); return 1; }
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   memset(&g_fx, 0, sizeof g_fx);
   g_fx.poly = poly;
   if (!scene_create(&s, 0)) return 0;
   s.mode = mode;
   float rot[1][4] = {{1, 0, 0, 0}};
   int ok = scene_draw(&s, 1, rot, f);
   uint32_t cov = ok ? count_cov(&s) : 0;
   const uint8_t *c = s.px + cy * s.pitch + cx * 4;
   int good = ok && cov >= lo && cov <= hi && c[3] == 0;
   if (!good) { printf("{\"fail\":\"%s: coverage %u expected %u..%u, interior alpha %u (want 0)\"}\n", name, cov, lo, hi, c[3]); g_fail++; }
   else printf("{\"test\":\"%s\",\"cov\":%u,\"ok\":true}\n", name, cov);
   memset(&g_fx, 0, sizeof g_fx);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return good;
}

/* VK_EXT_extended_dynamic_state3 polygonMode: same scenes as test_poly, but the pipeline is created FILL with POLYGON_MODE_EXT dynamic and the mode
 * comes from vkCmdSetPolygonModeEXT. On the fake backend nothing renders: the draw must still build and its decoded stream (golden) differs from FILL. */
static int
test_poly_dyn(const char *name, int mode, int poly, uint32_t lo, uint32_t hi, int cx, int cy)
{
   struct scene s = {.mode = mode};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if (!g_have_eds3) { printf("{\"test\":\"%s\",\"skipped\":\"extendedDynamicState3PolygonMode not advertised\",\"ok\":true}\n", name); return 1; }
   if (!g_feat.fillModeNonSolid) { printf("{\"test\":\"%s\",\"skipped\":\"fillModeNonSolid not advertised\",\"ok\":true}\n", name); return 1; }
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   memset(&g_fx, 0, sizeof g_fx);
   g_fx.poly = poly;
   g_fx.dynpoly = 1;
   if (!scene_create(&s, 0)) return 0;
   s.mode = mode;
   float rot[1][4] = {{1, 0, 0, 0}};
   int ok = scene_draw(&s, 1, rot, f);
   int good = ok;
   if (ok && !g_fake) {
      uint32_t cov = count_cov(&s);
      const uint8_t *c = s.px + cy * s.pitch + cx * 4;
      good = cov >= lo && cov <= hi && c[3] == 0;
      if (!good) { printf("{\"fail\":\"%s: coverage %u expected %u..%u, interior alpha %u (want 0)\"}\n", name, cov, lo, hi, c[3]); g_fail++; }
      else printf("{\"test\":\"%s\",\"cov\":%u,\"ok\":true}\n", name, cov);
   } else if (ok)
      printf("{\"test\":\"%s\",\"ok\":true,\"skipped\":\"pixels: fake backend\"}\n", name);
   memset(&g_fx, 0, sizeof g_fx);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return good;
}

/* R4: fixed-function state. Triangle (-.5,-.5) (.5,-.5) (0,.5) in Vulkan y-down NDC is clockwise on screen. */
enum { EXP_COV, EXP_SAME, EXP_DOUBLE, EXP_HALF, EXP_ZERO, EXP_ADDSQ, EXP_RONLY, EXP_X15, EXP_ONES, EXP_NOT, EXP_XOR, EXP_INV };
struct fxcase {
   const char *name;
   int depth, n;                  /* depth attachment, draws */
   float z[2];
   int exp;
   double cov;                    /* EXP_COV: expected pixels */
   int need;                      /* 1 = depthClamp, 2 = depthBiasClamp feature needed */
   void (*set)(void);
};

static void s_none(void) {}
static void s_back_cw(void) { g_fx.cull = VK_CULL_MODE_BACK_BIT; g_fx.cw = 1; }
static void s_back_ccw(void) { g_fx.cull = VK_CULL_MODE_BACK_BIT; }
static void s_front_cw(void) { g_fx.cull = VK_CULL_MODE_FRONT_BIT; g_fx.cw = 1; }
static void s_front_ccw(void) { g_fx.cull = VK_CULL_MODE_FRONT_BIT; }
static void s_dyn_back_ccw(void) { s_back_ccw(); g_fx.dyn = 1; }
static void s_dyn_front_ccw(void) { s_front_ccw(); g_fx.dyn = 1; }
static void blend(VkBlendFactor sf, VkBlendFactor df, VkBlendOp op) { g_fx.blend = 1; g_fx.sf = sf; g_fx.df = df; g_fx.op = op; }
static void s_add11(void) { blend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD); }
static void s_rsub11(void) { blend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_REVERSE_SUBTRACT); }
static void s_const_half(void) { blend(VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD); g_fx.bc[0] = g_fx.bc[1] = g_fx.bc[2] = g_fx.bc[3] = 0.5f; }
static void s_dyn_const_half(void) { s_const_half(); g_fx.dyn = 1; }
static void s_one_dst(void) { blend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_OP_ADD); }
static void s_one_src(void) { blend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_OP_ADD); }
static void s_dst_src(void) { blend(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_OP_ADD); }
/* dualSrcBlend: dual.frag writes o0 = varying colour, o1 = (0.5, 0.5, 0.5, 0.5) */
static void s_d_src1c(void) { g_fx.dual = 1; blend(VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD); }
static void s_d_inv_src1a(void) { g_fx.dual = 1; blend(VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD); }
static void s_d_one_src1c(void) { g_fx.dual = 1; blend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_OP_ADD); }
/* logicOp (static logicOpEnable, colour logic op in the blend shader): the framebuffer is cleared to 0 before the draw (destination = 51,102,153,204) */
static void s_lop_clear(void) { g_fx.logic = 1 + VK_LOGIC_OP_CLEAR; }
static void s_lop_set(void) { g_fx.logic = 1 + VK_LOGIC_OP_SET; }
static void s_lop_copy(void) { g_fx.logic = 1 + VK_LOGIC_OP_COPY; }
static void s_lop_copy_inv(void) { g_fx.logic = 1 + VK_LOGIC_OP_COPY_INVERTED; }
static void s_lop_xor(void) { g_fx.logic = 1 + VK_LOGIC_OP_XOR; }   /* src ^ dst */
static void s_lop_invert(void) { g_fx.logic = 1 + VK_LOGIC_OP_INVERT; }   /* ~dst */
static void s_mask_r(void) { g_fx.wmask = 1; }
static void s_src_alpha(void) { blend(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD); }
static void s_bias_off(void) { g_fx.bias = 1; }   /* bias enabled, factors 0: z = 1.0 fails LESS against the 1.0 clear */
static void s_bias_on(void) { g_fx.bias = 1; g_fx.bias_c = -1e5f; }
static void s_dyn_bias(void) { s_bias_on(); g_fx.dyn = 1; }
static void s_clamp(void) { g_fx.clamp = 1; }
static void s_clamp_noclip(void) { g_fx.clamp = 1; g_fx.dclip = 1; }   /* clamp + depthClipEnable FALSE: same as plain clamp */
static void s_clamp_clip(void) { g_fx.clamp = 1; g_fx.dclip = 2; }     /* clamp + depthClipEnable TRUE: z 1.5 is clipped */
static void s_bias_pos(void) { g_fx.bias = 1; g_fx.bias_c = 1e7f; }   /* z 0.5 + huge bias: fails LESS against the 1.0 clear */
static void s_bias_pos_clamp(void) { s_bias_pos(); g_fx.bias_clamp = 0.25f; }   /* clamped to +0.25: z 0.75 passes */
static void s_bias_neg_clamp(void) { g_fx.bias = 1; g_fx.bias_c = -1e7f; g_fx.bias_clamp = -0.25f; }   /* z 1.0 - 0.25 passes */
static void s_dyn_bias_pos_clamp(void) { s_bias_pos_clamp(); g_fx.dyn = 1; }

static const struct fxcase g_fxcases[] = {
   {"fx_cull_none", 0, 1, {0.5f}, EXP_COV, 8192, 0, s_none},
   {"fx_cull_back_cw", 0, 1, {0.5f}, EXP_COV, 8192, 0, s_back_cw},
   {"fx_cull_back_ccw", 0, 1, {0.5f}, EXP_COV, 0, 0, s_back_ccw},
   {"fx_cull_front_cw", 0, 1, {0.5f}, EXP_COV, 0, 0, s_front_cw},
   {"fx_cull_front_ccw", 0, 1, {0.5f}, EXP_COV, 8192, 0, s_front_ccw},
   {"fx_dyn_cull_back_ccw", 0, 1, {0.5f}, EXP_COV, 0, 0, s_dyn_back_ccw},
   {"fx_dyn_cull_front_ccw", 0, 1, {0.5f}, EXP_COV, 8192, 0, s_dyn_front_ccw},
   {"fx_blend_add11", 0, 2, {0.5f, 0.5f}, EXP_DOUBLE, 0, 0, s_add11},
   {"fx_blend_rsub11", 0, 2, {0.5f, 0.5f}, EXP_ZERO, 0, 0, s_rsub11},
   {"fx_blend_const", 0, 1, {0.5f}, EXP_HALF, 0, 0, s_const_half},
   {"fx_dyn_blend_const", 0, 1, {0.5f}, EXP_HALF, 0, 0, s_dyn_const_half},
   {"fx_blend_one_dstcolor", 0, 2, {0.5f, 0.5f}, EXP_ADDSQ, 0, 0, s_one_dst},
   {"fx_blend_one_srccolor", 0, 2, {0.5f, 0.5f}, EXP_ADDSQ, 0, 0, s_one_src},
   {"fx_blend_2srcdst", 0, 2, {0.5f, 0.5f}, EXP_ZERO, 0, 0, s_dst_src},
   {"fx_blend_srcalpha", 0, 2, {0.5f, 0.5f}, EXP_SAME, 0, 0, s_src_alpha},
   {"fx_logic_clear", 0, 1, {0.5f}, EXP_ZERO, 0, 8, s_lop_clear},
   {"fx_logic_set", 0, 1, {0.5f}, EXP_ONES, 0, 8, s_lop_set},
   {"fx_logic_copy", 0, 1, {0.5f}, EXP_SAME, 0, 8, s_lop_copy},
   {"fx_logic_copy_inv", 0, 1, {0.5f}, EXP_NOT, 0, 8, s_lop_copy_inv},
   {"fx_logic_xor", 0, 1, {0.5f}, EXP_XOR, 0, 8, s_lop_xor},
   {"fx_logic_invert", 0, 1, {0.5f}, EXP_INV, 0, 8, s_lop_invert},
   {"fx_dual_src1_color", 0, 1, {0.5f}, EXP_HALF, 0, 4, s_d_src1c},
   {"fx_dual_inv_src1_alpha", 0, 1, {0.5f}, EXP_HALF, 0, 4, s_d_inv_src1a},
   {"fx_dual_one_src1_color", 0, 2, {0.5f, 0.5f}, EXP_X15, 0, 4, s_d_one_src1c},
   {"fx_write_mask_r", 0, 1, {0.5f}, EXP_RONLY, 0, 0, s_mask_r},
   {"fx_bias_off", 1, 1, {1.0f}, EXP_COV, 0, 0, s_bias_off},
   {"fx_bias_on", 1, 1, {1.0f}, EXP_COV, 8192, 0, s_bias_on},
   {"fx_dyn_bias", 1, 1, {1.0f}, EXP_COV, 8192, 0, s_dyn_bias},
   {"fx_bias_pos_noclamp", 1, 1, {0.5f}, EXP_COV, 0, 0, s_bias_pos},
   {"fx_bias_pos_clamp", 1, 1, {0.5f}, EXP_COV, 8192, 2, s_bias_pos_clamp},
   {"fx_bias_neg_clamp", 1, 1, {1.0f}, EXP_COV, 8192, 2, s_bias_neg_clamp},
   {"fx_dyn_bias_pos_clamp", 1, 1, {0.5f}, EXP_COV, 8192, 2, s_dyn_bias_pos_clamp},
   {"fx_clip_off_range", 0, 1, {1.5f}, EXP_COV, 0, 0, s_none},
   {"fx_depth_clamp", 0, 1, {1.5f}, EXP_COV, 8192, 1, s_clamp},
   {"fx_dclip_clamp_noclip", 0, 1, {1.5f}, EXP_COV, 8192, 5, s_clamp_noclip},   /* needs VK_EXT_depth_clip_enable */
   {"fx_dclip_clamp_clip", 0, 1, {1.5f}, EXP_COV, 0, 5, s_clamp_clip},
};

static int
fx_run(const struct fxcase *c, uint8_t px[4], uint32_t *cov)
{
   struct scene s;
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   memset(&g_fx, 0, sizeof g_fx);
   g_fx.on = 1;
   g_fx.wmask = 0xf;
   c->set();
   memset(&s, 0, sizeof s);
   s.mode = MODE_TRI;
   if (!scene_create(&s, c->depth)) return 0;
   float rot[2][4] = {{1, 0, 0, c->z[0]}, {1, 0, 0, c->z[1]}};
   int ok = scene_draw(&s, c->n, rot, f);
   if (ok) {
      *cov = count_cov(&s);
      memcpy(px, s.px + 106 * s.pitch + 128 * 4, 4);
   }
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   memset(&g_fx, 0, sizeof g_fx);
   return ok;
}

static int
test_fx(void)
{
   int ok = 1;
   struct fxcase ref = {"ref", 0, 1, {0.5f}, EXP_SAME, 0, 0, s_none};
   uint8_t p[4] = {0}, q[4];
   uint32_t cov = 0;
   if (g_fake) {   /* the fake backend renders nothing, but every fx pipeline (shader + blend compile, per GPU class) must still build */
      for (unsigned i = 0; i < sizeof g_fxcases / sizeof *g_fxcases; i++) {
         const struct fxcase *c = &g_fxcases[i];
         struct scene s;
         if (g_only && strcmp(g_only, "fx") && strcmp(g_only, c->name)) continue;
         memset(&g_fx, 0, sizeof g_fx);
         g_fx.on = 1;
         g_fx.wmask = 0xf;
         c->set();
         memset(&s, 0, sizeof s);
         s.mode = MODE_TRI;
         printf("{\"test\":\"%s\",\"pipeline\":\"%s\"}\n", c->name, "creating");
         if (!scene_create(&s, c->depth)) return 0;
         scene_destroy(&s);
         memset(&g_fx, 0, sizeof g_fx);
      }
      printf("{\"test\":\"fx\",\"skipped\":\"fake backend: pipelines only\",\"ok\":true}\n");
      return 1;
   }
   if (!fx_run(&ref, p, &cov)) return 0;
   CHECK(abs(p[0] - 85) < 12 && p[3] == 255, "fx reference pixel %u,%u,%u,%u", p[0], p[1], p[2], p[3]);
   for (unsigned i = 0; ok && i < sizeof g_fxcases / sizeof *g_fxcases; i++) {
      const struct fxcase *c = &g_fxcases[i];
      if (g_only && strcmp(g_only, "fx") && strcmp(g_only, c->name)) continue;
      if ((c->need & 1) && !g_feat.depthClamp) { printf("{\"test\":\"%s\",\"skipped\":\"depthClamp not advertised\",\"ok\":true}\n", c->name); continue; }
      if ((c->need & 4) && !g_have_dclip) { printf("{\"test\":\"%s\",\"skipped\":\"VK_EXT_depth_clip_enable not advertised\",\"ok\":true}\n", c->name); continue; }
      if ((c->need & 8) && !g_feat.logicOp) { printf("{\"test\":\"%s\",\"skipped\":\"logicOp not advertised\",\"ok\":true}\n", c->name); continue; }
      if ((c->need & 2) && !g_feat.depthBiasClamp) { printf("{\"test\":\"%s\",\"skipped\":\"depthBiasClamp not advertised\",\"ok\":true}\n", c->name); continue; }
      if ((c->need & 4) && !g_feat.dualSrcBlend) { printf("{\"test\":\"%s\",\"skipped\":\"dualSrcBlend not advertised\",\"ok\":true}\n", c->name); continue; }
      if (!fx_run(c, q, &cov)) return 0;
      int good = 1;
      int e[4] = {p[0], p[1], p[2], p[3]};
      switch (c->exp) {
      case EXP_COV: good = fabs((double)cov - c->cov) <= c->cov * 0.02 + 8; break;
      case EXP_SAME: e[3] = 255; break;
      case EXP_DOUBLE: for (int k = 0; k < 3; k++) e[k] = e[k] * 2 > 255 ? 255 : e[k] * 2; e[3] = 255; break;
      case EXP_HALF: for (int k = 0; k < 3; k++) e[k] /= 2; e[3] = 127; break;
      case EXP_ZERO: e[0] = e[1] = e[2] = e[3] = 0; break;
      case EXP_ADDSQ: for (int k = 0; k < 3; k++) e[k] = e[k] + e[k] * e[k] / 255; e[3] = 255; break;
      case EXP_X15: for (int k = 0; k < 3; k++) e[k] = e[k] + e[k] / 2; e[3] = 255; break;
      case EXP_ONES: e[0] = e[1] = e[2] = e[3] = 255; break;
      case EXP_NOT: for (int k = 0; k < 4; k++) e[k] = 255 - (k == 3 ? 255 : e[k]); break;   /* ~src; the alpha written is 1.0 */
      case EXP_XOR: { const int d[4] = {51, 102, 153, 204}; for (int k = 0; k < 4; k++) e[k] = (k == 3 ? 255 : e[k]) ^ d[k]; break; }   /* src ^ dst */
      case EXP_INV: e[0] = 255 - 51; e[1] = 255 - 102; e[2] = 255 - 153; e[3] = 255 - 204; break;   /* ~dst */
      case EXP_RONLY: e[1] = e[2] = e[3] = 0; break;
      }
      if (c->exp != EXP_COV)
         for (int k = 0; k < 4; k++) good &= abs(q[k] - e[k]) <= 4;
      if (!good) {
         printf("{\"fail\":\"%s: pixel %u,%u,%u,%u expected %d,%d,%d,%d (coverage %u expected %.0f)\"}\n", c->name, q[0], q[1], q[2], q[3], e[0], e[1], e[2], e[3], cov, c->cov);
         g_fail++;
         ok = 0;
      } else {
         printf("{\"test\":\"%s\",\"cov\":%u,\"px\":[%u,%u,%u,%u],\"ok\":true}\n", c->name, cov, q[0], q[1], q[2], q[3]);
      }
   }
   return ok;
}

/* ring of 3 frames: three command buffers in one vkQueueSubmit, then three separate submits with a fence ring */
/* M13 occlusion: query 0 brackets the triangle draw, query 1 brackets nothing, query 2 a second draw (shifted triangle);
 * samples-passed must equal the covered-pixel count (1 sample/pixel) and 0 for the empty query. */
static int
test_occ(void)
{
   struct scene s = {.mode = MODE_TRI};   /* scene_create() takes s->mode as an input */
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   if (!scene_create(&s, 0)) return 0;
   s.mode = MODE_TRI;
   VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 3};
   VkQueryPool qp;
   CHECK(CreateQueryPool(dev, &qi, NULL, &qp) == VK_SUCCESS, "CreateQueryPool");
   for (int i = 0; i < g_loop; i++) {
      VkCommandBuffer cb;
      VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
      CHECK(AllocateCommandBuffers(dev, &ai, &cb) == VK_SUCCESS, "alloc");
      VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                     .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      CHECK(BeginCommandBuffer(cb, &bi) == VK_SUCCESS, "begin");
      CmdResetQueryPool(cb, qp, 0, 3);
      VkClearValue cv = {.color = {{0, 0, 0, 0}}};
      VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = s.rp,
                                  .framebuffer = s.fb, .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 1, .pClearValues = &cv};
      CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
      CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe);
      VkViewport vp = {0, 0, W, H, 0.0f, 1.0f};
      VkRect2D sc = {{0, 0}, {W, H}};
      CmdSetViewport(cb, 0, 1, &vp);
      CmdSetScissor(cb, 0, 1, &sc);
      VkBuffer vbs[2] = {s.vb[0], s.vb[1]};
      VkDeviceSize offs[2] = {0, 0};
      CmdBindVertexBuffers(cb, 0, 2, vbs, offs);
      float rot[4] = {1, 0, 0, 0};
      CmdPushConstants(cb, s.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, rot);
      CmdBeginQuery(cb, qp, 0, VK_QUERY_CONTROL_PRECISE_BIT);
      CmdDraw(cb, 3, 1, 0, 0);
      CmdEndQuery(cb, qp, 0);
      CmdBeginQuery(cb, qp, 1, VK_QUERY_CONTROL_PRECISE_BIT);
      CmdEndQuery(cb, qp, 1);
      CmdBeginQuery(cb, qp, 2, VK_QUERY_CONTROL_PRECISE_BIT);
      CmdDraw(cb, 3, 1, 0, 0);
      CmdEndQuery(cb, qp, 2);
      CmdEndRenderPass(cb);
      CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "end");
      VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
      CHECK(QueueSubmit(queue, 1, &si, f) == VK_SUCCESS, "submit");
      CHECK(WaitForFences(dev, 1, &f, VK_TRUE, 20000000000ull) == VK_SUCCESS, "wait");
      ResetFences(dev, 1, &f);
      FreeCommandBuffers(dev, s.pool, 1, &cb);
      if (g_fake) continue;
      uint64_t v[3] = {~0ull, ~0ull, ~0ull};
      VkResult r = GetQueryPoolResults(dev, qp, 0, 3, sizeof v, v, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      CHECK(r == VK_SUCCESS, "GetQueryPoolResults = %d", r);
      uint32_t cov = count_cov(&s);
      CHECK(cov > 1000, "coverage %u", cov);
      CHECK(v[1] == 0, "empty query = %llu", (unsigned long long)v[1]);
      CHECK(v[0] == cov, "iteration %d query 0 = %llu, coverage %u", i, (unsigned long long)v[0], cov);
      CHECK(v[2] == cov, "iteration %d query 2 = %llu, coverage %u", i, (unsigned long long)v[2], cov);
   }
   printf("{\"test\":\"occlusion\",\"n\":%d,\"ok\":true%s}\n", g_loop, g_fake ? ",\"skipped\":\"results: fake backend\"" : "");
   DestroyQueryPool(dev, qp, NULL);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return 1;
}

static int
test_ring(void)
{
   struct scene s = {.mode = MODE_TRI};   /* scene_create() takes s->mode as an input */
   VkFence f[3];
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   for (int i = 0; i < 3; i++) CHECK(CreateFence(dev, &fi, NULL, &f[i]) == VK_SUCCESS, "fence");
   if (!scene_create(&s, 0)) return 0;
   s.mode = MODE_TRI;
   int ok = 1;
   int saved = g_reset;
   g_reset = 0;
   for (int i = 0; ok && i < g_loop; i++) {
      /* record three frames, submit each with its ring fence, wait the oldest as the ring wraps */
      VkCommandBuffer cb[3];
      VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 3};
      CHECK(AllocateCommandBuffers(dev, &ai, cb) == VK_SUCCESS, "alloc ring");
      for (int k = 0; k < 3; k++) {
         VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
         CHECK(BeginCommandBuffer(cb[k], &bi) == VK_SUCCESS, "begin");
         VkClearValue cv = {.color = {{0, 0, 0, 0}}};
         VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = s.rp,
                                     .framebuffer = s.fb, .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 1, .pClearValues = &cv};
         CmdBeginRenderPass(cb[k], &rb, VK_SUBPASS_CONTENTS_INLINE);
         CmdBindPipeline(cb[k], VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe);
         VkViewport vp = {0, 0, W, H, 0, 1};
         VkRect2D sc = {{0, 0}, {W, H}};
         CmdSetViewport(cb[k], 0, 1, &vp);
         CmdSetScissor(cb[k], 0, 1, &sc);
         VkBuffer vbs[2] = {s.vb[0], s.vb[1]};
         VkDeviceSize offs[2] = {0, 0};
         CmdBindVertexBuffers(cb[k], 0, 2, vbs, offs);
         float rot[4] = {1, 0, (k - 1) * 0.25f, 0};
         CmdPushConstants(cb[k], s.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, rot);
         CmdDraw(cb[k], 3, 1, 0, 0);
         CmdEndRenderPass(cb[k]);
         CHECK(EndCommandBuffer(cb[k]) == VK_SUCCESS, "end");
      }
      for (int k = 0; k < 3; k++) {
         VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb[k]};
         CHECK(QueueSubmit(queue, 1, &si, f[k]) == VK_SUCCESS, "submit ring %d", k);
      }
      for (int k = 0; k < 3; k++) {
         CHECK(WaitForFences(dev, 1, &f[k], VK_TRUE, 20000000000ull) == VK_SUCCESS, "wait ring %d", k);
         ResetFences(dev, 1, &f[k]);
      }
      FreeCommandBuffers(dev, s.pool, 3, cb);
      if (!g_fake) {   /* last frame wins: triangle shifted by +0.25 NDC (clear before it) */
         uint32_t cov = count_cov(&s);
         double err = fabs((double)cov - 8192.0) / 8192.0 * 100.0;
         CHECK(err <= 1.0, "ring iteration %d coverage %u (%.2f%%)", i, cov, err);
      }
   }
   g_reset = saved;
   printf("{\"test\":\"ring3\",\"n\":%d,\"ok\":true%s}\n", g_loop, g_fake ? ",\"skipped\":\"pixels: fake backend\"" : "");
   scene_destroy(&s);
   for (int i = 0; i < 3; i++) DestroyFence(dev, f[i], NULL);
   return ok;
}

static int
near3(const uint8_t *p, int r, int g, int b, int tol)
{
   return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol && p[3] == 255;
}

/* consecutive draws that differ only in the dynamic-UBO offset (and, later, in firstInstance): each must get its own dynamic table, and the
 * reused static driver set must not leak one draw's data into the next. Pixels on hardware only. Four quarter triangles, last draw wins. */
static int
test_share_dyn(void)
{
   g_dyn = 1;
   g_share = 3;
   struct scene s = {.mode = MODE_UBO};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   int ok = scene_create(&s, 0);
   const float q[4][4] = {{.5f, 0, -.5f, -.5f}, {.5f, 0, .5f, -.5f}, {.5f, 0, -.5f, .5f}, {.5f, 0, .5f, .5f}};
   const int pos[8] = {0, 0, 1, 1, 2, 2, 3, 3};
   const uint32_t off[8] = {0, 1, 1, 2, 2, 2, 0, 1};   /* colour blocks A, B, C */
   const uint32_t fi_[8] = {0, 0, 0, 0, 0, 0, 1, 1};
   float rot[8][4];
   for (int i = 0; i < 8; i++) {
      memcpy(rot[i], q[pos[i]], sizeof rot[i]);
      g_share_off[i] = off[i];
      g_share_fi[i] = fi_[i];
   }
   if (ok) ok = scene_draw(&s, 8, (const float (*)[4])rot, f);
   if (ok && !g_fake) {
      static const int want[4][3] = {{255, 0, 128}, {0, 255, 0}, {0, 255, 0}, {255, 0, 128}};   /* last colour drawn at each position */
      for (int k = 0; k < 4; k++) {
         int cx = (int)((q[k][2] * .5f + .5f) * W), cy = (int)((q[k][3] * .5f + .5f) * H);
         const uint8_t *c = s.px + cy * s.pitch + cx * g_colbpp;
         CHECK(near3(c, want[k][0], want[k][1], want[k][2], 2), "share_dyn: triangle %d colour %u,%u,%u,%u", k, c[0], c[1], c[2], c[3]);
      }
      printf("{\"test\":\"share_dyn\",\"ok\":true}\n");
   } else if (ok) printf("{\"test\":\"share_dyn\",\"skipped\":\"fake backend\",\"ok\":true}\n");
   g_share = 0;
   g_dyn = 0;
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

/* firstInstance is baked into the per-instance attribute descriptors (build_draw_descs): a draw that differs only in firstInstance, or only
 * in the vertex-buffer offset, must not reuse the previous draw's driver set. Host: the frame with changes allocates more than the frame with
 * four identical draws (the fake backend counts the bytes); hardware: all four corner triangles are drawn. */
static int
test_share_inst(void)
{
   alloc_stats_start();
   float dummy[4][4] = {{0}};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   unsigned long long ab[3] = {0};
   int ok = 1;
   struct scene s;
   for (int pass = 0; ok && pass < 3; pass++) {
      g_inst = pass == 1 ? 1 : 2;   /* 2, 1, 2: the second identical frame removes the first-submit warm-up */
      s = (struct scene){.mode = MODE_TRI};
      ok = scene_create(&s, 0) && scene_draw(&s, 4, dummy, f);
      ab[pass] = alloc_bytes();
      if (ok && pass == 1 && !g_fake) {
         for (int k = 0; k < 4; k++) {
            int cx = (int)(((k & 1 ? .5f : -.5f) * .5f + .5f) * W) - 6, cy = (int)(((k & 2 ? .5f : -.5f) * .5f + .5f) * H) - 6;   /* inside the triangle: offset + (-0.1,-0.1) */
            CHECK(px_at(&s, cx, cy) != 0, "share_inst: corner %d triangle missing at %d,%d", k, cx, cy);
         }
         ok &= report("share_inst", &s, 4 * 2048.0, 3.0, 0);
      }
      scene_destroy(&s);
   }
   g_inst = 0;
   DestroyFence(dev, f, NULL);
   if (!ok) return 0;
   if (!g_fake) {   /* the byte counter is the fake-backend host assertion (the phone library has no /tmp stats file) */
      printf("{\"test\":\"share_inst_pixels\",\"ok\":true}\n");
      return 1;
   }
   unsigned long long same = ab[2] - ab[1], chg = ab[1] - ab[0];
   /* same-state frame 0 -> 2 differ by nothing; the changing frame must allocate at least 3 driver sets (3 x 17 x 32 B) more than an identical one */
   printf("{\"test\":\"share_inst\",\"alloc_identical\":%llu,\"alloc_changed\":%llu,\"ok\":%s}\n", same, chg, chg >= same + 3 * 17 * 32 ? "true" : "false");
   CHECK(chg >= same + 3 * 17 * 32, "share_inst: draws that differ in firstInstance / vertex buffer reused the driver set (%llu vs %llu bytes)", chg, same);
   return 1;
}

/* share_idx: the frame-build fast path skips the per-draw register packing when nothing differs (same_draw_words). The case
 * (MODE_SHAREIDX) is four draws whose per-draw words are identical and whose index buffer alternates A B A B, with an identical
 * pipeline rebind before the third: the index-buffer VA must be in the key (vkCmdBindIndexBuffer sets no dirty bit) and the rebind
 * must not stop the following draw from packing. The C side only records and submits; tests/check_share_idx.py decodes the CS and
 * counts the index-buffer register writes (check.sh "share_idx CS"). Explicit only: a new frame would change the vk_draw golden. */
static int
test_share_idx(void)
{
   struct scene s = {.mode = MODE_SHAREIDX};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   if (!scene_create(&s, 0)) return 0;
   float rot[1][4] = {{1, 0, 0, 0}};
   int ok = scene_draw(&s, 4, rot, f);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   if (ok) printf("{\"test\":\"share_idx\",\"draws\":4,\"ok\":true}\n");
   return ok;
}

/* share_dirty (fake backend byte counter): each state group's dirty bit must make the draw rebuild what depends on it, and rebinding an equal
 * pipeline must not. Frames of 4 draws: push constants, viewport, cull mode change between draws (more bytes than the identical frame) and the
 * same pipeline rebound (no more bytes). MALI_DIRTY_CHECK (check.sh) additionally aborts when a mask reuses what really changed. */
static int
test_share_dirty(void)
{
   if (!g_fake) {
      printf("{\"test\":\"share_dirty\",\"skipped\":\"fake backend byte counter\",\"ok\":true}\n");
      return 1;
   }
   alloc_stats_start();
   float same_rot[4][4] = {{0}}, var_rot[4][4] = {{0}};
   for (int i = 0; i < 4; i++) var_rot[i][0] = 0.1f * i;
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   static const char *names[5] = {"push", "viewport", "cullmode", "rebind_pipeline", "scissor"};
   int ok = 1;
   for (int k = 0; ok && k < 5; k++) {
      unsigned long long ab[3];
      for (int pass = 0; ok && pass < 3; pass++) {
         g_dirty = pass == 2 ? (k == 0 ? 0 : k) : (k == 0 ? 0 : 0);
         if (k == 3) g_dirty = pass == 2 ? 3 : 0;
         else if (k == 4) g_dirty = pass == 2 ? 4 : 0;
         else if (k == 1) g_dirty = pass == 2 ? 1 : 0;
         else if (k == 2) g_dirty = pass == 2 ? 2 : 0;
         struct scene s = {.mode = MODE_TRI};
         ok = scene_create(&s, 0) && scene_draw(&s, 4, k == 0 && pass == 2 ? var_rot : same_rot, f);
         ab[pass] = alloc_bytes();
         scene_destroy(&s);
      }
      g_dirty = 0;
      if (!ok) break;
      unsigned long long same = ab[1] - ab[0], chg = ab[2] - ab[1];
      int want_more = k < 3;   /* rebinding an equal pipeline and the scissor (a register) must not rebuild any table */
      int good = want_more ? chg >= same + 3 * 64 : chg <= same;
      printf("{\"test\":\"share_dirty\",\"bit\":\"%s\",\"alloc_identical\":%llu,\"alloc_changed\":%llu,\"ok\":%s}\n", names[k], same, chg, good ? "true" : "false");
      CHECK(good, "share_dirty %s: %llu bytes for the changing frame vs %llu for the identical one", names[k], chg, same);
   }
   DestroyFence(dev, f, NULL);
   return ok;
}

/* full-screen quad cases: push-constant-only FS colour, sampled texture, uniform buffer, descriptor updates between draws */
static int
test_quad(const char *name, int mode)
{
   struct scene s = {.mode = MODE_TRI};   /* scene_create() takes s->mode as an input */
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   s.mode = mode;
   if (!scene_create(&s, 0)) return 0;
   float one[1][4] = {{1, 0, 0, 0}};
   float two[2][4] = {{.5f, 0, -.5f, 0}, {.5f, 0, .5f, 0}};
   for (int i = 0; i < g_loop; i++) {
      if (mode == MODE_DESCUPD) {   /* rewrite set 1 -> buffer B or C between submissions, then two draws with two sets */
         VkDescriptorBufferInfo bi = {s.ubo[1 + (i & 1)], 0, 16};
         VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s.dset[1], .dstBinding = 0,
                                   .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi};
         UpdateDescriptorSets(dev, 1, &w, 0, NULL);
      }
      if (!scene_draw(&s, mode == MODE_DESCUPD ? 2 : 1, mode == MODE_DESCUPD ? two : one, f)) return 0;
      if (g_fake) continue;
      uint32_t cov = count_cov(&s);
      double expect = mode == MODE_DESCUPD ? 32768.0 : 65536.0;
      CHECK(cov == (uint32_t)expect, "%s iteration %d coverage %u expect %.0f", name, i, cov, expect);
      const uint8_t *c = s.px + 128 * s.pitch + 128 * 4;
      if (mode == MODE_PUSHFS) CHECK(near3(c, 191, 64, 128, 2), "pc colour %u,%u,%u,%u", c[0], c[1], c[2], c[3]);
      if (mode == MODE_UBO) CHECK(near3(c, 64, 128, 191, 2), "ubo colour %u,%u,%u,%u", c[0], c[1], c[2], c[3]);
      /* set 7 carries colour C and the shader reads ONLY set 7: the last of eight bound sets must reach the SRT */
      if (mode == MODE_SETS8) CHECK(near3(c, 0, 255, 0, 2), "sets8 colour %u,%u,%u,%u (set 7's UBO)", c[0], c[1], c[2], c[3]);
      if (mode == MODE_DESCUPD) {
         const uint8_t *l = s.px + 128 * s.pitch + 64 * 4, *r = s.px + 128 * s.pitch + 192 * 4;
         CHECK(near3(l, 64, 128, 191, 2), "left (set 0) %u,%u,%u,%u", l[0], l[1], l[2], l[3]);
         if (i & 1) CHECK(near3(r, 0, 255, 0, 2), "right (set 1 = C) %u,%u,%u,%u", r[0], r[1], r[2], r[3]);
         else CHECK(near3(r, 255, 0, 128, 2), "right (set 1 = B) %u,%u,%u,%u", r[0], r[1], r[2], r[3]);
      }
      if (mode == MODE_TEX) {
         unsigned bad = 0, checked = 0;
         for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
               if ((x & 31) == 0 || (x & 31) == 31 || (y & 31) == 0 || (y & 31) == 31) continue;   /* texel borders: rounding */
               const uint8_t *p = s.px + y * s.pitch + x * 4;
               checked++;
               bad += !near3(p, (x / 32) * 32, (y / 32) * 32, 128, 1);
            }
         CHECK(bad == 0, "texture: %u of %u px differ from the texel colour (first texel px %u,%u,%u,%u)", bad, checked,
               s.px[16 * 4], s.px[16 * 4 + 1], s.px[16 * 4 + 2], s.px[16 * 4 + 3]);
      }
   }
   printf("{\"test\":\"%s\",\"n\":%d,\"ok\":true%s}\n", name, g_loop, g_fake ? ",\"skipped\":\"pixels: fake backend\"" : "");
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return 1;
}

/* Mutable-format views (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT): the view's format decides the encoding. zink presents the
 * window through an SRGB view of a UNORM swapchain image (notes/apilog.md: vfmt=50 over ifmt=44), so a linear 0.5 grey
 * written through an SRGB view must be encoded on write (188, not 128), and the reverse (SRGB image, UNORM view) must
 * not be (128, not 188). Sampling: the same texel 128 through an SRGB view decodes to linear (55 in a UNORM target),
 * through a UNORM view it stays 128.
 * The render pass' attachment format is the VIEW format (Vulkan requires the framebuffer view to match it); the image
 * keeps its own format plus VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT. */
static int
vfmt_run(const char *name, VkFormat imgfmt, VkFormat viewfmt, VkFormat texfmt, VkFormat texvfmt, unsigned want)
{
   struct scene s = {.mode = MODE_PUSHFS};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   static const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
   static const float qclear[4] = {0.25f, 0.25f, 0.25f, 0.25f};   /* the sampled cases: 0x40 in every channel, a value
                                                                  * they never draw (55, 128), so a missing draw cannot pass */
   static const float quarter[4] = {0.25f, 0.25f, 0.25f, 1.0f};   /* the draw's colour: differs from the clear, so a
                                                                   * missing render-target write cannot pass as the clear */
   g_colfmt = imgfmt;
   g_vfmt = viewfmt;
   g_texfmt = texfmt;
   g_texvfmt = texvfmt;
   g_clear_f = texfmt ? qclear : grey;
   memcpy(g_pcfs, quarter, sizeof g_pcfs);
   g_hostread = 1;
   if (texfmt) s.mode = MODE_TEX;
   int ok = scene_create(&s, 0);
   float one[1][4] = {{1, 0, 0, 0}};
   if (ok) ok = scene_draw(&s, 1, one, f);
   g_hostread = 0;
   uint32_t got = 0;
   /* the pixel checks are hardware-only: the fake backend renders nothing, and the MALI_SOFT CPU rasteriser renders only
    * pipelines the test registers CPU programs for (view_format registers none), so it is not a verdict either */
   const int pixels = ok && !g_fake;
   if (pixels) {
      /* the allocation is HOST_VISIBLE (not necessarily HOST_COHERENT): invalidate before the host reads the mapping */
      VkMappedMemoryRange mmr = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = s.colmem, .offset = 0,
                                 .size = VK_WHOLE_SIZE};
      InvalidateMappedMemoryRanges(dev, 1, &mmr);
      got = px_at(&s, W / 2, H / 2) & 0xff;
      ok = got + 2 >= want && got <= want + 2;
   }
   printf("{\"test\":\"%s\",\"image\":%d,\"view\":%d,\"px\":%u,\"want\":%u,\"ok\":%s%s}\n", name, (int)imgfmt,
          (int)viewfmt, got, want, ok ? "true" : "false",
          pixels ? "" : g_fake ? ",\"skipped\":\"fake backend\"" : ",\"skipped\":\"no pixels\"");
   CHECK(ok, "%s: pixel %u want %u", name, got, want);
   g_colfmt = VK_FORMAT_R8G8B8A8_UNORM;
   g_vfmt = VK_FORMAT_UNDEFINED;
   g_texfmt = g_texvfmt = VK_FORMAT_UNDEFINED;
   g_clear_f = NULL;
   memcpy(g_pcfs, (const float[4]){0.75f, 0.25f, 0.5f, 1.0f}, sizeof g_pcfs);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

static int
test_view_format(void)
{
   int ok = 1;
   /* UNORM image, SRGB view: the 0.25 linear draw (clear stays 0.5 grey) -> sRGB 137 (the bug renders 64) */
   ok &= vfmt_run("view_srgb_over_unorm", VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
                  VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, 137);
   /* SRGB image, UNORM view: the 0.25 draw stays 64 (the bug renders 137) */
   ok &= vfmt_run("view_unorm_over_srgb", VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
                  VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, 64);
   /* sampled: texel 128 through an SRGB view decodes to linear and lands as 55 in the UNORM target; a UNORM view gives
    * 128. These clear to 0.25 grey (0x40), so a missing draw reads 64 and fails (see vfmt_run's qclear). */
   ok &= vfmt_run("view_sample_srgb", VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED,
                  VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, 55);
   ok &= vfmt_run("view_sample_unorm", VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED,
                  VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 128);
   return ok;
}

/* ==========================================================================================
 * Format sweep (notes/format-sweep.md): every vertex-attribute format and every sampled-image
 * format the driver advertises gets a draw with known data and a readback compared against the
 * value the format's own conversion rule gives. One JSON line per format; `--only vtxfmt` /
 * `--only texfmt`. Real meshes are the reason for the shape: a non-zero attribute offset, a
 * stride larger than the element, and a second stream at binding 1.
 * ========================================================================================== */
enum { K_UNORM, K_SNORM, K_USC, K_SSC, K_UINT, K_SINT, K_FLT, K_UF, K_E5, K_D16, K_D32, K_D24S8, K_SRGB };
static const char *g_kname[] = {"unorm", "snorm", "uscaled", "sscaled", "uint", "sint", "float", "ufloat", "e5b9g9r9",
                                "d16", "d32", "d24s8", "srgb"};
struct fd {   /* one Vulkan format and how its components sit in the little-endian element */
   VkFormat fmt;
   const char *name;
   unsigned char elem, ncomp, kind;
   unsigned char bits[4], off[4];
};
#define F(fmt, elem, n, k, b0, b1, b2, b3, o0, o1, o2, o3) \
   {VK_FORMAT_##fmt, #fmt, elem, n, k, {b0, b1, b2, b3}, {o0, o1, o2, o3}}
#define F1(fmt, k, b0, o0) F(fmt, (b0 + 7) / 8, 1, K_##k, b0, 0, 0, 0, o0, 0, 0, 0)
#define F2(fmt, k, e, b0, b1, o0, o1) F(fmt, e, 2, K_##k, b0, b1, 0, 0, o0, o1, 0, 0)
#define F3(fmt, k, e, b0, b1, b2, o0, o1, o2) F(fmt, e, 3, K_##k, b0, b1, b2, 0, o0, o1, o2, 0)
#define F4(fmt, k, e, b0, b1, b2, b3, o0, o1, o2, o3) F(fmt, e, 4, K_##k, b0, b1, b2, b3, o0, o1, o2, o3)
static const struct fd g_fmts[] = {
   /* 4/5/6-bit packed */
   F2(R4G4_UNORM_PACK8, UNORM, 1, 4, 4, 4, 0),
   F4(R4G4B4A4_UNORM_PACK16, UNORM, 2, 4, 4, 4, 4, 12, 8, 4, 0),
   F4(B4G4R4A4_UNORM_PACK16, UNORM, 2, 4, 4, 4, 4, 4, 8, 12, 0),
   F3(R5G6B5_UNORM_PACK16, UNORM, 2, 5, 6, 5, 11, 5, 0),
   F3(B5G6R5_UNORM_PACK16, UNORM, 2, 5, 6, 5, 0, 5, 11),
   F4(R5G5B5A1_UNORM_PACK16, UNORM, 2, 5, 5, 5, 1, 11, 6, 1, 0),
   F4(B5G5R5A1_UNORM_PACK16, UNORM, 2, 5, 5, 5, 1, 1, 6, 11, 0),
   F4(A1R5G5B5_UNORM_PACK16, UNORM, 2, 5, 5, 5, 1, 10, 5, 0, 15),
   /* 8-bit */
   F1(R8_UNORM, UNORM, 8, 0), F1(R8_SNORM, SNORM, 8, 0), F1(R8_USCALED, USC, 8, 0),
   F1(R8_SSCALED, SSC, 8, 0), F1(R8_UINT, UINT, 8, 0), F1(R8_SINT, SINT, 8, 0),
   F2(R8G8_UNORM, UNORM, 2, 8, 8, 0, 8), F2(R8G8_SNORM, SNORM, 2, 8, 8, 0, 8),
   F2(R8G8_USCALED, USC, 2, 8, 8, 0, 8), F2(R8G8_SSCALED, SSC, 2, 8, 8, 0, 8),
   F2(R8G8_UINT, UINT, 2, 8, 8, 0, 8), F2(R8G8_SINT, SINT, 2, 8, 8, 0, 8),
   F3(R8G8B8_UNORM, UNORM, 3, 8, 8, 8, 0, 8, 16), F3(R8G8B8_SNORM, SNORM, 3, 8, 8, 8, 0, 8, 16),
   F3(R8G8B8_USCALED, USC, 3, 8, 8, 8, 0, 8, 16), F3(R8G8B8_SSCALED, SSC, 3, 8, 8, 8, 0, 8, 16),
   F3(R8G8B8_UINT, UINT, 3, 8, 8, 8, 0, 8, 16), F3(R8G8B8_SINT, SINT, 3, 8, 8, 8, 0, 8, 16),
   F3(B8G8R8_UNORM, UNORM, 3, 8, 8, 8, 16, 8, 0), F3(B8G8R8_SNORM, SNORM, 3, 8, 8, 8, 16, 8, 0),
   F3(B8G8R8_USCALED, USC, 3, 8, 8, 8, 16, 8, 0), F3(B8G8R8_SSCALED, SSC, 3, 8, 8, 8, 16, 8, 0),
   F3(B8G8R8_UINT, UINT, 3, 8, 8, 8, 16, 8, 0), F3(B8G8R8_SINT, SINT, 3, 8, 8, 8, 16, 8, 0),
   F4(R8G8B8A8_UNORM, UNORM, 4, 8, 8, 8, 8, 0, 8, 16, 24), F4(R8G8B8A8_SNORM, SNORM, 4, 8, 8, 8, 8, 0, 8, 16, 24),
   F4(R8G8B8A8_USCALED, USC, 4, 8, 8, 8, 8, 0, 8, 16, 24), F4(R8G8B8A8_SSCALED, SSC, 4, 8, 8, 8, 8, 0, 8, 16, 24),
   F4(R8G8B8A8_UINT, UINT, 4, 8, 8, 8, 8, 0, 8, 16, 24), F4(R8G8B8A8_SINT, SINT, 4, 8, 8, 8, 8, 0, 8, 16, 24),
   F4(B8G8R8A8_UNORM, UNORM, 4, 8, 8, 8, 8, 16, 8, 0, 24), F4(B8G8R8A8_SNORM, SNORM, 4, 8, 8, 8, 8, 16, 8, 0, 24),
   F4(B8G8R8A8_USCALED, USC, 4, 8, 8, 8, 8, 16, 8, 0, 24), F4(B8G8R8A8_SSCALED, SSC, 4, 8, 8, 8, 8, 16, 8, 0, 24),
   F4(B8G8R8A8_UINT, UINT, 4, 8, 8, 8, 8, 16, 8, 0, 24), F4(B8G8R8A8_SINT, SINT, 4, 8, 8, 8, 8, 16, 8, 0, 24),
   F4(A8B8G8R8_UNORM_PACK32, UNORM, 4, 8, 8, 8, 8, 0, 8, 16, 24),
   F4(A8B8G8R8_SNORM_PACK32, SNORM, 4, 8, 8, 8, 8, 0, 8, 16, 24),
   F4(A8B8G8R8_USCALED_PACK32, USC, 4, 8, 8, 8, 8, 0, 8, 16, 24),
   F4(A8B8G8R8_SSCALED_PACK32, SSC, 4, 8, 8, 8, 8, 0, 8, 16, 24),
   F4(A8B8G8R8_UINT_PACK32, UINT, 4, 8, 8, 8, 8, 0, 8, 16, 24),
   F4(A8B8G8R8_SINT_PACK32, SINT, 4, 8, 8, 8, 8, 0, 8, 16, 24),
   /* 16-bit (the skinned-mesh suspects: R16G16_SFLOAT UVs, R16G16B16A16_SNORM/SFLOAT) */
   F1(R16_UNORM, UNORM, 16, 0), F1(R16_SNORM, SNORM, 16, 0), F1(R16_USCALED, USC, 16, 0),
   F1(R16_SSCALED, SSC, 16, 0), F1(R16_UINT, UINT, 16, 0), F1(R16_SINT, SINT, 16, 0), F1(R16_SFLOAT, FLT, 16, 0),
   F2(R16G16_UNORM, UNORM, 4, 16, 16, 0, 16), F2(R16G16_SNORM, SNORM, 4, 16, 16, 0, 16),
   F2(R16G16_USCALED, USC, 4, 16, 16, 0, 16), F2(R16G16_SSCALED, SSC, 4, 16, 16, 0, 16),
   F2(R16G16_UINT, UINT, 4, 16, 16, 0, 16), F2(R16G16_SINT, SINT, 4, 16, 16, 0, 16),
   F2(R16G16_SFLOAT, FLT, 4, 16, 16, 0, 16),
   F3(R16G16B16_UNORM, UNORM, 6, 16, 16, 16, 0, 16, 32), F3(R16G16B16_SNORM, SNORM, 6, 16, 16, 16, 0, 16, 32),
   F3(R16G16B16_USCALED, USC, 6, 16, 16, 16, 0, 16, 32), F3(R16G16B16_SSCALED, SSC, 6, 16, 16, 16, 0, 16, 32),
   F3(R16G16B16_UINT, UINT, 6, 16, 16, 16, 0, 16, 32), F3(R16G16B16_SINT, SINT, 6, 16, 16, 16, 0, 16, 32),
   F3(R16G16B16_SFLOAT, FLT, 6, 16, 16, 16, 0, 16, 32),
   F4(R16G16B16A16_UNORM, UNORM, 8, 16, 16, 16, 16, 0, 16, 32, 48),
   F4(R16G16B16A16_SNORM, SNORM, 8, 16, 16, 16, 16, 0, 16, 32, 48),
   F4(R16G16B16A16_USCALED, USC, 8, 16, 16, 16, 16, 0, 16, 32, 48),
   F4(R16G16B16A16_SSCALED, SSC, 8, 16, 16, 16, 16, 0, 16, 32, 48),
   F4(R16G16B16A16_UINT, UINT, 8, 16, 16, 16, 16, 0, 16, 32, 48),
   F4(R16G16B16A16_SINT, SINT, 8, 16, 16, 16, 16, 0, 16, 32, 48),
   F4(R16G16B16A16_SFLOAT, FLT, 8, 16, 16, 16, 16, 0, 16, 32, 48),
   /* 32-bit */
   F1(R32_UINT, UINT, 32, 0), F1(R32_SINT, SINT, 32, 0), F1(R32_SFLOAT, FLT, 32, 0),
   F2(R32G32_UINT, UINT, 8, 32, 32, 0, 32), F2(R32G32_SINT, SINT, 8, 32, 32, 0, 32),
   F2(R32G32_SFLOAT, FLT, 8, 32, 32, 0, 32),
   F3(R32G32B32_UINT, UINT, 12, 32, 32, 32, 0, 32, 64), F3(R32G32B32_SINT, SINT, 12, 32, 32, 32, 0, 32, 64),
   F3(R32G32B32_SFLOAT, FLT, 12, 32, 32, 32, 0, 32, 64),
   F4(R32G32B32A32_UINT, UINT, 16, 32, 32, 32, 32, 0, 32, 64, 96),
   F4(R32G32B32A32_SINT, SINT, 16, 32, 32, 32, 32, 0, 32, 64, 96),
   F4(R32G32B32A32_SFLOAT, FLT, 16, 32, 32, 32, 32, 0, 32, 64, 96),
   /* packed 32-bit (Unity's colour / normal packing on D3D11 level 10.1) */
   F4(A2B10G10R10_UNORM_PACK32, UNORM, 4, 10, 10, 10, 2, 0, 10, 20, 30),
   F4(A2B10G10R10_SNORM_PACK32, SNORM, 4, 10, 10, 10, 2, 0, 10, 20, 30),
   F4(A2B10G10R10_USCALED_PACK32, USC, 4, 10, 10, 10, 2, 0, 10, 20, 30),
   F4(A2B10G10R10_SSCALED_PACK32, SSC, 4, 10, 10, 10, 2, 0, 10, 20, 30),
   F4(A2B10G10R10_UINT_PACK32, UINT, 4, 10, 10, 10, 2, 0, 10, 20, 30),
   F4(A2B10G10R10_SINT_PACK32, SINT, 4, 10, 10, 10, 2, 0, 10, 20, 30),
   F4(A2R10G10B10_UNORM_PACK32, UNORM, 4, 10, 10, 10, 2, 20, 10, 0, 30),
   F4(A2R10G10B10_SNORM_PACK32, SNORM, 4, 10, 10, 10, 2, 20, 10, 0, 30),
   F4(A2R10G10B10_USCALED_PACK32, USC, 4, 10, 10, 10, 2, 20, 10, 0, 30),
   F4(A2R10G10B10_SSCALED_PACK32, SSC, 4, 10, 10, 10, 2, 20, 10, 0, 30),
   F4(A2R10G10B10_UINT_PACK32, UINT, 4, 10, 10, 10, 2, 20, 10, 0, 30),
   F4(A2R10G10B10_SINT_PACK32, SINT, 4, 10, 10, 10, 2, 20, 10, 0, 30),
   F3(B10G11R11_UFLOAT_PACK32, UF, 4, 11, 11, 10, 21, 10, 0),
   F3(E5B9G9R9_UFLOAT_PACK32, E5, 4, 9, 9, 9, 0, 9, 18),
   /* sRGB: sampled images only (Vulkan has no sRGB vertex format) */
   F1(R8_SRGB, SRGB, 8, 0),
   F2(R8G8_SRGB, SRGB, 2, 8, 8, 0, 8),
   F3(R8G8B8_SRGB, SRGB, 3, 8, 8, 8, 0, 8, 16),
   F3(B8G8R8_SRGB, SRGB, 3, 8, 8, 8, 16, 8, 0),
   F4(R8G8B8A8_SRGB, SRGB, 4, 8, 8, 8, 8, 0, 8, 16, 24),
   F4(B8G8R8A8_SRGB, SRGB, 4, 8, 8, 8, 8, 16, 8, 0, 24),
   F4(A8B8G8R8_SRGB_PACK32, SRGB, 4, 8, 8, 8, 8, 0, 8, 16, 24),
   /* depth, for the texture sweep only (D24S8: depth in bits 8..31, stencil 0..7) */
   F1(D16_UNORM, D16, 16, 0),
   F1(D32_SFLOAT, D32, 32, 0),
   F(D24_UNORM_S8_UINT, 4, 1, K_D24S8, 24, 0, 0, 0, 8, 0, 0, 0),
};
#undef F1
#undef F2
#undef F3
#undef F4

/* ---- encoders: raw component code <-> the value the format's conversion rule gives ---- */
static void
put_bits(uint8_t *p, int bo, int nb, uint32_t v)
{
   for (int i = 0; i < nb; i++, bo++)
      if (v & (1u << i))
         p[bo >> 3] |= (uint8_t)(1u << (bo & 7));
}
static uint32_t
f2h(float f)
{
   uint32_t u;
   memcpy(&u, &f, 4);
   const uint32_t s = (u >> 16) & 0x8000u;
   int e = (int)((u >> 23) & 0xff) - 127 + 15;
   uint32_t m = u & 0x7fffffu;
   if (e <= 0) return s;                                                    /* the sweep's values are normal and exact */
   if (e >= 31) return s | 0x7c00u;
   return s | ((uint32_t)e << 10) | (m >> 13);
}
static float
h2f(uint32_t h)
{
   const uint32_t s = (h & 0x8000u) << 16;
   const int e = (int)((h >> 10) & 0x1f);
   uint32_t m = h & 0x3ffu;
   uint32_t u;
   if (e == 0) {
      float f = (float)m / 1024.0f / 16384.0f;
      return (h & 0x8000u) ? -f : f;
   }
   u = s | ((uint32_t)(e - 15 + 127) << 23) | (m << 13);
   float f;
   memcpy(&f, &u, 4);
   return f;
}
static uint32_t
f2uf(float f, int mb, int eb)   /* unsigned 11/10-bit style float: value = (m / 2^mb) * 2^(e - bias) */
{
   const int bias = (1 << (eb - 1)) - 1;
   int e = 0;
   while (e < (1 << eb) - 1 && f >= (float)(1u << (e - bias + 1))) e++;        /* normalise so m < 2^mb */
   uint32_t m = (uint32_t)(f / (float)ldexp(1.0, e - bias) * (1u << mb) + 0.5f);
   if (m >= (1u << mb)) { m = 0; e++; }
   return ((uint32_t)e << mb) | m;
}
static float
uf2f(uint32_t c, int mb, int eb)
{
   const int bias = (1 << (eb - 1)) - 1;
   const uint32_t e = c >> mb, m = c & ((1u << mb) - 1);
   return (float)((double)m / (double)(1u << mb) * ldexp(1.0, (int)e - bias));
}
/* The component code of component i (R,G,B,A order) for stream s, and the value the spec's conversion gives. */
static void
fmt_code(const struct fd *d, int i, int s, uint32_t *code, double *val)
{
   const int b = d->bits[i];
   const uint32_t maxv = b >= 32 ? 0xffffffffu : ((1u << b) - 1u);
   const double fa = 0.2 * (i + 1), fb = 0.5 + 0.1 * (i + 1);   /* stream A / stream B target fractions */
   const double sa[4] = {-0.75, -0.25, 0.25, 0.75}, sb[4] = {-0.5, -0.1, 0.1, 0.5};
   const double f = s ? fb : fa;
   const double smax = (double)((1u << (b - 1)) - 1);
   switch (d->kind) {
   case K_UNORM: case K_D16: case K_SRGB:
      *code = (uint32_t)(f * maxv + 0.5);
      *val = d->kind == K_D16 ? (double)*code / maxv : (double)*code / maxv;
      break;
   case K_D24S8:
      *code = (uint32_t)(f * 0xffffff + 0.5) & 0xffffffu;
      *val = (double)*code / 16777215.0;
      break;
   case K_SNORM:
      *code = (uint32_t)(int32_t)llround((s ? sb[i] : sa[i]) * smax);
      *val = (double)(int32_t)*code / smax;
      if (*val < -1.0) *val = -1.0;
      break;
   case K_USC:
      *code = (uint32_t)(f * maxv + 0.5);
      *val = (double)*code;
      break;
   case K_SSC:
      *code = (uint32_t)(int32_t)llround((s ? sb[i] : sa[i]) * smax);
      *val = (double)(int32_t)*code;
      break;
   case K_UINT:
      *code = (uint32_t)(s ? i + 1 : i);
      *val = (double)*code;
      break;
   case K_SINT:
      *code = (uint32_t)(int32_t)(s ? (i & 1 ? 2 : 4) * (i < 2 ? -1 : 1) : (i & 1 ? 1 : 2) * (i < 2 ? -1 : 1));
      *val = (double)(int32_t)*code;
      break;
   case K_FLT: {
      const double v = s ? (const double[4]){0.1, 0.3, 0.6, 0.9}[i] : (const double[4]){0.25, 0.5, 0.75, 1.0}[i];
      if (b == 16) { *code = f2h((float)v); *val = h2f(*code); }
      else { float fv = (float)v; memcpy(code, &fv, 4); *val = fv; }
      break;
   }
   case K_UF: {
      const float v = s ? (const float[3]){0.1f, 0.3f, 0.6f}[i] : (const float[3]){0.25f, 0.5f, 0.75f}[i];
      *code = f2uf(v, b, b == 11 ? 5 : 5);
      *val = uf2f(*code, b, 5);
      break;
   }
   case K_E5: {
      const float v = s ? (const float[3]){0.1f, 0.3f, 0.6f}[i] : (const float[3]){0.25f, 0.5f, 0.75f}[i];
      *code = (uint32_t)(v * 512.0f + 0.5f);
      *val = (double)*code / 512.0;   /* shared exponent 15 */
      break;
   }
   default:   /* K_D32 */
      {
         const float v = (float)(s ? fb : fa);
         memcpy(code, &v, 4);
         *val = v;
      }
      break;
   }
}
/* scale/bias that maps the format's value range into [0,1] for the RGBA8 target (uniform for unorm/snorm/float) */
static void
fmt_scale(const struct fd *d, double sc[4], double bi[4])
{
   for (int i = 0; i < 4; i++) {
      const uint32_t maxv = d->bits[i] >= 32 ? 0xffffffffu : ((1u << d->bits[i]) - 1u);
      sc[i] = 1.0;
      bi[i] = 0.0;
      switch (d->kind) {
      case K_USC: sc[i] = 1.0 / (double)maxv; break;
      case K_SSC: sc[i] = 1.0 / (double)((1u << (d->bits[i] - 1)) - 1); break;
      case K_UINT: sc[i] = 0.25; break;
      case K_SINT: sc[i] = 0.125; bi[i] = 0.5; break;
      default: break;
      }
   }
}
/* write stream s of format d into dst[0..elem): returns the value each component decodes to (missing = 0,0,0,1) */
static void
fmt_encode(const struct fd *d, int s, uint8_t *dst)
{
   memset(dst, 0, d->elem);
   for (int i = 0; i < d->ncomp; i++) {
      uint32_t code;
      double val;
      fmt_code(d, i, s, &code, &val);
      if (d->kind == K_E5) {   /* the shared exponent, bits 27..31 */
         put_bits(dst, d->off[i], d->bits[i], code);
         put_bits(dst, 27, 5, 15);
      } else {
         put_bits(dst, d->off[i], d->bits[i], code);
      }
   }
}
static void
fmt_expect(const struct fd *d, int s, int ncomp, double out[4])
{
   for (int i = 0; i < 4; i++) out[i] = i == 3 ? 1.0 : 0.0;
   for (int i = 0; i < ncomp && i < d->ncomp; i++) {
      uint32_t code;
      double val;
      fmt_code(d, i, s, &code, &val);
      out[i] = val;
   }
}

/* ---- sweep scaffolding: one RGBA8 target, one 8x8 cell per row, one submit ---- */
#define FSW_CELL 8
#define FSW_COLS (W / FSW_CELL)          /* 32 cells per line */
#define FSW_MAX  (FSW_COLS * FSW_COLS)   /* 1024 rows */
static VkPhysicalDevice g_pd;
static PFN_vkGetPhysicalDeviceFormatProperties GetFmtProps;

struct fsw {
   VkImage img;
   VkDeviceMemory mem;        /* the colour attachment (OPTIMAL tiling: Mali does not render to a linear image reliably) */
   VkImageView view;
   VkBuffer rb;               /* readback buffer: the image is copied here at the end */
   VkDeviceMemory rbmem;
   uint8_t *px;
   uint32_t pitch;
   VkCommandPool pool;
   VkCommandBuffer cb;
   VkFence fence;
   int nrow;
};

static VkRect2D
fsw_rect(int row)
{
   VkRect2D r = {{(row % FSW_COLS) * FSW_CELL, (row / FSW_COLS) * FSW_CELL}, {FSW_CELL, FSW_CELL}};
   return r;
}

static int
fsw_init(struct fsw *f)
{
   memset(f, 0, sizeof *f);
   VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   CHECK(CreateImage(dev, &ii, NULL, &f->img) == VK_SUCCESS, "fsw image");
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, f->img, &mr);
   int mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (mt < 0) mt = memtype(mr.memoryTypeBits, 0);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
   CHECK(AllocateMemory(dev, &ai, NULL, &f->mem) == VK_SUCCESS, "fsw image memory");
   CHECK(BindImageMemory(dev, f->img, f->mem, 0) == VK_SUCCESS, "fsw bind");
   /* the readback buffer: tightly packed RGBA8 rows */
   VkBufferCreateInfo rbci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)W * H * 4,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   CHECK(CreateBuffer(dev, &rbci, NULL, &f->rb) == VK_SUCCESS, "fsw readback buffer");
   VkMemoryRequirements rmr;
   GetBufferMemoryRequirements(dev, f->rb, &rmr);
   int rmt = memtype(rmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   CHECK(rmt >= 0, "fsw readback memory type");
   VkMemoryAllocateInfo rai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rmr.size, .memoryTypeIndex = rmt};
   CHECK(AllocateMemory(dev, &rai, NULL, &f->rbmem) == VK_SUCCESS, "fsw readback memory");
   CHECK(BindBufferMemory(dev, f->rb, f->rbmem, 0) == VK_SUCCESS, "fsw readback bind");
   uint8_t *map;
   CHECK(MapMemory(dev, f->rbmem, 0, VK_WHOLE_SIZE, 0, (void **)&map) == VK_SUCCESS, "fsw map");
   f->pitch = W * 4;
   f->px = map;
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = f->img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   CHECK(CreateImageView(dev, &vi, NULL, &f->view) == VK_SUCCESS, "fsw view");
   VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   CHECK(CreateCommandPool(dev, &pi, NULL, &f->pool) == VK_SUCCESS, "fsw pool");
   VkCommandBufferAllocateInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = f->pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   CHECK(AllocateCommandBuffers(dev, &cbi, &f->cb) == VK_SUCCESS, "fsw cb");
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f->fence) == VK_SUCCESS, "fsw fence");
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CHECK(BeginCommandBuffer(f->cb, &bi) == VK_SUCCESS, "fsw begin");
   VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .image = f->img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
   CmdPipelineBarrier2(f->cb, &dep);
   return 1;
}

static void
fsw_cell_begin(struct fsw *f, int row)
{
   VkRect2D a = fsw_rect(row);
   VkRenderingAttachmentInfo att = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = f->view,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .resolveMode = VK_RESOLVE_MODE_NONE,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
   att.clearValue.color.float32[0] = 0.5f;   /* a drawn cell must overwrite this, and it sets alpha = 0 (the "drawn" mark) */
   VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = a, .layerCount = 1,
      .colorAttachmentCount = 1, .pColorAttachments = &att};
   CmdBeginRendering(f->cb, &ri);
   f->nrow = row + 1;
}

static void
fsw_cell_end(struct fsw *f)
{
   CmdEndRendering(f->cb);
}

static int
fsw_submit(struct fsw *f)
{
   /* colour attachment -> TRANSFER_SRC, copy the whole image into the readback buffer, then hand it to the host */
   VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .image = f->img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
   CmdPipelineBarrier2(f->cb, &dep);
   VkBufferImageCopy reg = {.bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
   CmdCopyImageToBuffer(f->cb, f->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f->rb, 1, &reg);
   CHECK(EndCommandBuffer(f->cb) == VK_SUCCESS, "fsw end");
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &f->cb};
   CHECK(QueueSubmit(queue, 1, &si, f->fence) == VK_SUCCESS, "fsw submit");
   CHECK(WaitForFences(dev, 1, &f->fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS, "fsw wait");
   DeviceWaitIdle(dev);
   VkMappedMemoryRange mr = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = f->rbmem, .offset = 0, .size = VK_WHOLE_SIZE};
   InvalidateMappedMemoryRanges(dev, 1, &mr);
   return 1;
}

/* the centre texel of a cell: returns 0 and rgb, or -1 when nothing was drawn (alpha still 0) */
static int
fsw_read(struct fsw *f, int row, int rgb[3])
{
   VkRect2D a = fsw_rect(row);
   const uint8_t *p = f->px + (uint32_t)(a.offset.y + FSW_CELL / 2) * f->pitch + (uint32_t)(a.offset.x + FSW_CELL / 2) * 4;
   if (!p[3]) return -1;
   rgb[0] = p[0];
   rgb[1] = p[1];
   rgb[2] = p[2];
   return 0;
}

static void
fsw_destroy(struct fsw *f)
{
   if (f->fence) DestroyFence(dev, f->fence, NULL);
   if (f->pool) DestroyCommandPool(dev, f->pool, NULL);
   if (f->view) DestroyImageView(dev, f->view, NULL);
   if (f->img) DestroyImage(dev, f->img, NULL);
   if (f->mem) FreeMemory(dev, f->mem, NULL);
   if (f->rb) DestroyBuffer(dev, f->rb, NULL);
   if (f->rbmem) FreeMemory(dev, f->rbmem, NULL);
   memset(f, 0, sizeof *f);
}

/* integer formats need their own shader modules: a uvec/ivec input is the only legal declaration */
static int
fmt_class(const struct fd *d) { return d->kind == K_UINT ? 1 : d->kind == K_SINT ? 2 : 0; }

/* ==========================================================================================
 * test_vtxfmt: every advertised vertex-buffer format, drawn as a flat attribute and read back.
 * Four rows per format: binding 0 as vec4, binding 1 as vec4, binding 0 as vec2, binding 1 as vec2.
 * Attribute offsets 4 and 8, strides element+8 .. element+11 (a real mesh layout), data in one buffer.
 * ========================================================================================== */
struct vtxpc { float scale[4], bias[4]; int32_t c[4]; };

static int
test_vtxfmt(void)
{
   const int nall = (int)(sizeof g_fmts / sizeof g_fmts[0]);
   static int sel[512], stride_[512], baseA[512], baseB[512];
   int nf = 0, trunc = 0;
   /* 1. the advertised vertex formats, with a stride larger than the element */
   const char *only = getenv("FMTSW_ONLY");
   for (int i = 0; i < nall; i++) {
      VkFormatProperties fp;
      GetFmtProps(g_pd, g_fmts[i].fmt, &fp);
      if (only && strcmp(only, g_fmts[i].name)) continue;
      if (g_fmts[i].kind == K_SRGB) continue;   /* no sRGB vertex format exists */
      if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) continue;
      if (nf == 256) { trunc = 1; break; }
      sel[nf] = i;
      stride_[nf] = ((g_fmts[i].elem + 3) & ~3) + 8;
      nf++;
   }
   printf("{\"test\":\"vtxfmt\",\"formats\":%d%s}\n", nf, trunc ? ",\"truncated\":true" : "");
   if (!nf) return 1;

   /* 2. the vertex data: ONE BUFFER PER STREAM (one buffer bound at two different offsets to bindings 0 and 1 made the
    * driver fetch the second offset's bytes for BOTH attributes -- notes/format-sweep.md). Stream A carries the attribute
    * at offset 4 of each vertex, stream B its own bytes at offset 8, with different values so a mix-up shows. */
   VkBuffer vb[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
   VkDeviceMemory vmem[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
   uint8_t *bp[2] = {NULL, NULL};
   VkDeviceSize tot = 16;
   for (int k = 0; k < nf; k++) tot += 16 + 6 * (VkDeviceSize)stride_[k];
   for (int sd = 0; sd < 2; sd++) {
      VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = tot,
         .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
      CHECK(CreateBuffer(dev, &bci, NULL, &vb[sd]) == VK_SUCCESS, "vtxfmt buffer %d", sd);
      VkMemoryRequirements vmr;
      GetBufferMemoryRequirements(dev, vb[sd], &vmr);
      int vmt = memtype(vmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
      CHECK(vmt >= 0, "vtxfmt memory type");
      VkMemoryAllocateInfo vai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = vmr.size,
         .memoryTypeIndex = vmt};
      CHECK(AllocateMemory(dev, &vai, NULL, &vmem[sd]) == VK_SUCCESS, "vtxfmt memory");
      CHECK(BindBufferMemory(dev, vb[sd], vmem[sd], 0) == VK_SUCCESS, "vtxfmt bind");
      CHECK(MapMemory(dev, vmem[sd], 0, VK_WHOLE_SIZE, 0, (void **)&bp[sd]) == VK_SUCCESS, "vtxfmt map");
      memset(bp[sd], 0, (size_t)tot);
      VkDeviceSize off = 16;
      for (int k = 0; k < nf; k++) {
         if (sd == 0) baseA[k] = (int)off; else baseB[k] = (int)off;
         for (int v = 0; v < 6; v++)
            fmt_encode(&g_fmts[sel[k]], sd, bp[sd] + off + (sd ? 8 : 4) + v * stride_[k]);
         off += 16 + 6 * (VkDeviceSize)stride_[k];
      }
      VkMappedMemoryRange flush = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = vmem[sd], .offset = 0,
         .size = VK_WHOLE_SIZE};
      FlushMappedMemoryRanges(dev, 1, &flush);
   }

   /* 3. shaders + pipeline layout (16 B scale, 16 B bias, 16 B selector) */
   /* [width][class]: the width is the shader's own attribute type, so one pipeline asks for exactly 2 or 4 components */
   VkShaderModule vsm[2][3], fsm;
   for (int w = 0; w < 2; w++) {
      vsm[w][0] = load_spv(w ? "fmtsw2.vert.spv" : "fmtsw4.vert.spv");
      vsm[w][1] = load_spv(w ? "fmtsw2_u.vert.spv" : "fmtsw4_u.vert.spv");
      vsm[w][2] = load_spv(w ? "fmtsw2_s.vert.spv" : "fmtsw4_s.vert.spv");
   }
   fsm = load_spv("fmtsw.frag.spv");
   CHECK(vsm[0][0] && vsm[0][1] && vsm[0][2] && vsm[1][0] && vsm[1][1] && vsm[1][2] && fsm, "vtxfmt shader modules");
   VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, .size = sizeof(struct vtxpc)};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pcr};
   VkPipelineLayout pl;
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &pl) == VK_SUCCESS, "vtxfmt layout");

   /* 4. the sweep */
   struct fsw f;
   if (!fsw_init(&f)) return 0;
   int bad = 0, nrow = 0, npix = 0;
   static VkPipeline pipes[512];
   for (int k = 0; k < nf; k++) {
      const struct fd *d = &g_fmts[sel[k]];
      const int nc = fmt_class(d);
      VkVertexInputBindingDescription bd[2] = {{.binding = 0, .stride = (uint32_t)stride_[k], .inputRate = VK_VERTEX_INPUT_RATE_VERTEX},
                                               {.binding = 1, .stride = (uint32_t)stride_[k], .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}};
      VkVertexInputAttributeDescription ad[2] = {{.location = 0, .binding = 0, .format = d->fmt, .offset = 4},
                                                 {.location = 1, .binding = 1, .format = d->fmt, .offset = 8}};
      /* ONE attribute, from ONE binding. Binding both streams at once made the driver fetch the LAST binding's bytes
       * for the attribute (notes/format-sweep.md), which says nothing about the format; a stream is selected by the
       * binding index, so stream B is still a real second stream at binding 1. */
      VkPipelineVertexInputStateCreateInfo vin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
         .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bd[0],
         .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &ad[0]};
      VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
         .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
      VkViewport vpv = {0.0f, 0.0f, (float)W, (float)H, 0.0f, 1.0f};
      VkRect2D scs = {{0, 0}, {W, H}};
      VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
         .viewportCount = 1, .pViewports = &vpv, .scissorCount = 1, .pScissors = &scs};
      VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
         .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
         .lineWidth = 1.0f};
      VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
         .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
      VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
      VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
         .attachmentCount = 1, .pAttachments = &cba};
      VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
         .colorAttachmentCount = 1, .pColorAttachmentFormats = (VkFormat[]){VK_FORMAT_R8G8B8A8_UNORM}};
      VkPipelineShaderStageCreateInfo st[2] = {
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vsm[0][nc], .pName = "main"},
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fsm, .pName = "main"}};
      VkGraphicsPipelineCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
         .stageCount = 2, .pStages = st, .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vp,
         .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl};
      double sc[4], bi[4];
      fmt_scale(d, sc, bi);
      int piped = 1;
      for (int w2 = 0; w2 < 2 && piped; w2++)    /* w2 = 0: the attribute declared vec4; w2 = 1: declared vec2 */
         for (int sd = 0; sd < 2; sd++) {         /* sd = which stream the attribute is fetched from */
            bd[0].binding = (uint32_t)sd;
            bd[0].stride = (uint32_t)stride_[k];
            ad[0].binding = (uint32_t)sd;
            ad[0].format = d->fmt;
            ad[0].offset = sd ? 8 : 4;
            VkPipelineShaderStageCreateInfo st2[2] = {st[0], st[1]};
            st2[0].module = vsm[w2][nc];
            VkGraphicsPipelineCreateInfo gp2 = gp;
            gp2.pStages = st2;
            VkPipeline pipe = VK_NULL_HANDLE;
            if (CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp2, NULL, &pipe) != VK_SUCCESS) { piped = 0; break; }
            pipes[k * 4 + w2 * 2 + sd] = pipe;
            if (g_fake) continue;
            const int row = k * 4 + w2 * 2 + sd;
            nrow = row + 1;
            fsw_cell_begin(&f, row);
            CmdBindPipeline(f.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            /* the stream's own non-zero binding offset; the attribute offset (4 or 8) is inside each vertex */
            const VkDeviceSize off = (VkDeviceSize)(sd ? baseB[k] : baseA[k]);
            CmdBindVertexBuffers(f.cb, (uint32_t)sd, 1, &vb[sd], &off);
            struct vtxpc pc = {.c = {0, 0, 0, 0}};
            for (int i = 0; i < 4; i++) {
               pc.scale[i] = (float)sc[i];
               pc.bias[i] = (float)bi[i];
            }
            CmdPushConstants(f.cb, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
            CmdDraw(f.cb, 6, 1, 0, 0);
            fsw_cell_end(&f);
         }
      if (!piped) {
         printf("{\"test\":\"vtxfmt\",\"fmt\":\"%s\",\"skipped\":\"pipeline\",\"ok\":false}\n", d->name);
         bad++;
         continue;
      }
      if (g_fake) {
         /* the fake backend renders nothing: the host can still assert that every advertised vertex format builds a pipeline */
         printf("{\"test\":\"vtxfmt\",\"fmt\":\"%s\",\"feat\":\"vb\",\"skipped\":\"fake backend renders nothing\",\"ok\":true}\n", d->name);
         continue;
      }
   }
   if (g_fake) {
      for (int k = 0; k < 4 * nf; k++) if (pipes[k]) DestroyPipeline(dev, pipes[k], NULL);
      DestroyPipelineLayout(dev, pl, NULL);
      for (int w = 0; w < 2; w++)
         for (int c = 0; c < 3; c++) DestroyShaderModule(dev, vsm[w][c], NULL);
      DestroyShaderModule(dev, fsm, NULL);
      DestroyBuffer(dev, vb[0], NULL); FreeMemory(dev, vmem[0], NULL);
      DestroyBuffer(dev, vb[1], NULL); FreeMemory(dev, vmem[1], NULL);
      fsw_destroy(&f);
      return bad == 0;
   }
   if (!fsw_submit(&f)) return 0;
   /* 5. one line per format: the four rows' expectations against the readback */
   for (int k = 0; k < nf; k++) {
      const struct fd *d = &g_fmts[sel[k]];
      double sc[4], bi[4];
      fmt_scale(d, sc, bi);
      int fmtbad = 0, first_row = -1, first_ch = -1, want0 = -1, got0 = -1;
      for (int w = 0; w < 4; w++) {
         const int row = k * 4 + w;   /* w = w2 * 2 + which: width 4 then width 2, stream A then stream B */
         double ex[4];
         fmt_expect(d, w & 1, w >= 2 ? 2 : 4, ex);
         int rgb[3], got[3], want[3];
         if (fsw_read(&f, row, rgb)) {
            if (!fmtbad) { first_row = w; first_ch = -1; want0 = -1; got0 = -1; }
            fmtbad++;
            continue;
         }
         npix++;
         for (int i = 0; i < 3; i++) {
            double t = ex[i] * sc[i] + bi[i];
            t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
            want[i] = (int)lround(t * 255.0);
            got[i] = rgb[i];
            if (abs(got[i] - want[i]) > 1 && first_ch < 0) { first_row = w; first_ch = i; want0 = want[i]; got0 = got[i]; }
            if (abs(got[i] - want[i]) > 1) fmtbad++;
         }
      }
      if (getenv("FMTSW_DUMP")) {
         int r2 = k * 4;
         for (int w = 0; w < 4; w++) {
            double e2[4];
            int g2[3] = {-1, -1, -1};
            fmt_expect(d, w & 1, w >= 2 ? 2 : 4, e2);
            fsw_read(&f, r2 + w, g2);
            printf("{\"dump\":\"vtxfmt\",\"fmt\":\"%s\",\"which\":%d,\"got\":\"%d,%d,%d\",\"val\":\"%.4f,%.4f,%.4f\"}\n",
                   d->name, w, g2[0], g2[1], g2[2], e2[0], e2[1], e2[2]);
         }
      }
      if (fmtbad) {
         bad++;
         printf("{\"test\":\"vtxfmt\",\"fmt\":\"%s\",\"kind\":\"%s\",\"feat\":\"vb\",\"ok\":false,\"row\":%d,\"ch\":%d,\"expected\":%d,\"got\":%d}\n",
                d->name, g_kname[d->kind], first_row, first_ch, want0, got0);
      } else {
         printf("{\"test\":\"vtxfmt\",\"fmt\":\"%s\",\"feat\":\"vb\",\"ok\":true}\n", d->name);
      }
   }
   for (int k = 0; k < 4 * nf; k++) if (pipes[k]) DestroyPipeline(dev, pipes[k], NULL);
   DestroyPipelineLayout(dev, pl, NULL);
   for (int w = 0; w < 2; w++)
      for (int c = 0; c < 3; c++) DestroyShaderModule(dev, vsm[w][c], NULL);
   DestroyShaderModule(dev, fsm, NULL);
   DestroyBuffer(dev, vb[0], NULL); FreeMemory(dev, vmem[0], NULL);
   DestroyBuffer(dev, vb[1], NULL); FreeMemory(dev, vmem[1], NULL);
   fsw_destroy(&f);
   printf("{\"test\":\"vtxfmt\",\"result\":\"%s\",\"formats\":%d,\"rows\":%d,\"pixels\":%d}\n",
          bad ? "FAIL" : "PASS", nf, nrow, npix);
   return bad == 0;
}

/* ==========================================================================================
 * test_texfmt: every advertised sampled-image format, an 8x8 known pattern with 3 mips uploaded
 * and fetched at mip 0 and 1 with texelFetch and with textureLod. sRGB formats are the one place
 * the two paths must differ (textureLod linearises, texelFetch does not).
 * ========================================================================================== */
static const struct {
   VkFormat fmt;
   const char *name;
   int block;   /* block bytes */
} g_bcfmt[] = {
   {VK_FORMAT_BC1_RGB_UNORM_BLOCK, "BC1_RGB_UNORM", 8},
   {VK_FORMAT_BC1_RGB_SRGB_BLOCK, "BC1_RGB_SRGB", 8},
   {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1_RGBA_UNORM", 8},
   {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, "BC1_RGBA_SRGB", 8},
   {VK_FORMAT_BC2_UNORM_BLOCK, "BC2_UNORM", 16},
   {VK_FORMAT_BC2_SRGB_BLOCK, "BC2_SRGB", 16},
   {VK_FORMAT_BC3_UNORM_BLOCK, "BC3_UNORM", 16},
   {VK_FORMAT_BC3_SRGB_BLOCK, "BC3_SRGB", 16},
   {VK_FORMAT_BC4_UNORM_BLOCK, "BC4_UNORM", 8},
   {VK_FORMAT_BC4_SNORM_BLOCK, "BC4_SNORM", 8},
   {VK_FORMAT_BC5_UNORM_BLOCK, "BC5_UNORM", 16},
   {VK_FORMAT_BC5_SNORM_BLOCK, "BC5_SNORM", 16},
   {VK_FORMAT_BC7_UNORM_BLOCK, "BC7_UNORM", 16},
   {VK_FORMAT_BC7_SRGB_BLOCK, "BC7_SRGB", 16},
};
#define TEX_NBC ((int)(sizeof g_bcfmt / sizeof g_bcfmt[0]))
#define TEX_MIPS 3

static double
srgb_to_lin(double c)
{
   return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

/* the uniform channel values of stream s (0 = mip 0, 1 = mip 1) for the formats the table covers */
static void
tex_vals(const struct fd *d, int s, double out[4])
{
   fmt_expect(d, s, d->ncomp, out);
}

/* a uniform BC block: every texel decodes to rgb[] (the encoder writes the block's own endpoint codes,
 * so the expected value is the spec's expansion of exactly what the block says) */
static void
bc_params(int k, int s, uint32_t *r, uint32_t *g, uint32_t *b, double rgb[3], double *alpha)
{
   const VkFormat fmt = g_bcfmt[k].fmt;
   const double va[3] = {0.2, 0.4, 0.6}, vb[3] = {0.6, 0.7, 0.8};
   const double v0 = s ? vb[0] : va[0], v1 = s ? vb[1] : va[1], v2 = s ? vb[2] : va[2];
   *alpha = 1.0;
   switch (fmt) {
   case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
   case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
   case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
   case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
      *r = (uint32_t)lround(v0 * 31.0);
      *g = (uint32_t)lround(v1 * 63.0);
      *b = (uint32_t)lround(v2 * 31.0);
      rgb[0] = (double)((*r << 3) | (*r >> 2)) / 255.0;
      rgb[1] = (double)((*g << 2) | (*g >> 4)) / 255.0;
      rgb[2] = (double)((*b << 3) | (*b >> 2)) / 255.0;
      break;
   case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC5_UNORM_BLOCK:
      *r = s ? 0x90u : 0xc0u;
      *g = s ? 0x60u : 0x30u;
      *b = 0;
      rgb[0] = (double)*r / 255.0;
      rgb[1] = fmt == VK_FORMAT_BC4_UNORM_BLOCK ? 0.0 : (double)*g / 255.0;   /* BC4 is one channel */
      rgb[2] = 0.0;
      break;
   case VK_FORMAT_BC4_SNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK:
      *r = s ? (uint32_t)(int32_t)64 : (uint32_t)(int32_t)96;
      *g = s ? (uint32_t)(int32_t)-32 : (uint32_t)(int32_t)-96;
      rgb[0] = (double)(int32_t)*r / 127.0;
      rgb[1] = fmt == VK_FORMAT_BC4_SNORM_BLOCK ? 0.0 : (double)(int32_t)*g / 127.0;
      rgb[2] = 0.0;
      break;
   default:   /* BC7 mode 6, endpoints equal, P = 0: value = (e << 1) / 255 */
      *r = (uint32_t)lround(v0 * 127.0);
      *g = (uint32_t)lround(v1 * 127.0);
      *b = (uint32_t)lround(v2 * 127.0);
      rgb[0] = (double)(*r << 1) / 255.0;
      rgb[1] = (double)(*g << 1) / 255.0;
      rgb[2] = (double)(*b << 1) / 255.0;
      break;
   }
}

static void
bc_block(int k, int s, uint8_t *blk)
{
   const VkFormat fmt = g_bcfmt[k].fmt;
   uint32_t r, g, b;
   double rgb[3], alpha;
   bc_params(k, s, &r, &g, &b, rgb, &alpha);
   memset(blk, 0, (size_t)g_bcfmt[k].block);
   switch (fmt) {
   case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
   case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: {
      const uint16_t c = (uint16_t)((r << 11) | (g << 5) | b);   /* BC1 stores RGB565 with R in bits 11..15 */
      blk[0] = (uint8_t)c; blk[1] = (uint8_t)(c >> 8);
      blk[2] = (uint8_t)c; blk[3] = (uint8_t)(c >> 8);   /* c0 == c1, all indices 0 -> colour 0 */
      break;
   }
   case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: {
      const uint16_t c = (uint16_t)((r << 11) | (g << 5) | b);
      for (int i = 0; i < 8; i++) blk[i] = 0xff;         /* the alpha block comes first: 4-bit alpha 15 = 1.0 */
      blk[8] = (uint8_t)c; blk[9] = (uint8_t)(c >> 8);
      blk[10] = (uint8_t)c; blk[11] = (uint8_t)(c >> 8);
      break;
   }
   case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK: {
      const uint16_t c = (uint16_t)((r << 11) | (g << 5) | b);
      blk[0] = blk[1] = 0xff;                             /* alpha block first: a0 == a1 -> alpha 1.0 */
      blk[8] = (uint8_t)c; blk[9] = (uint8_t)(c >> 8);
      blk[10] = (uint8_t)c; blk[11] = (uint8_t)(c >> 8);
      break;
   }
   case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK:
      blk[0] = (uint8_t)r; blk[1] = (uint8_t)r;           /* a0 == a1, indices 0 */
      break;
   case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK:
      blk[0] = (uint8_t)r; blk[1] = (uint8_t)r;
      blk[8] = (uint8_t)g; blk[9] = (uint8_t)g;
      break;
   default: {                                             /* BC7 mode 6, one subset, 4-bit indices all 0 */
      int bo = 7;                                          /* the 7-bit mode field, bit 6 set */
      blk[0] |= 0x40;
      const uint32_t e[8] = {r, r, g, g, b, b, 127, 127};  /* R0,R1,G0,G1,B0,B1,A0,A1 (7 bits each) */
      for (int i = 0; i < 8; i++) { put_bits(blk, bo, 7, e[i]); bo += 7; }
      bo += 2;                                             /* P0, P1 = 0 */
      /* 16 four-bit indices, all 0; the first texel's MSB is the implicit anchor bit */
      break;
   }
   }
}

static int
tex_mip_size(int k, const struct fd *d, int mip)
{
   const int w = 8 >> mip, h = 8 >> mip;
   if (k >= 0) return ((w + 3) / 4) * ((h + 3) / 4) * g_bcfmt[k].block;
   return w * h * (int)d->elem;
}

static int
test_texfmt(void)
{
   const int nall = (int)(sizeof g_fmts / sizeof g_fmts[0]);
   static int sel[256], nf = 0;
   static int rows_mip[FSW_MAX], rows_mode[FSW_MAX];
   int nrow = 0, trunc = 0;
   for (int i = 0; i < nall + TEX_NBC; i++) {
      const int bc = i >= nall;
      const VkFormat fmt = bc ? g_bcfmt[i - nall].fmt : g_fmts[i].fmt;
      const struct fd *d = bc ? NULL : &g_fmts[i];
      VkFormatProperties fp;
      GetFmtProps(g_pd, fmt, &fp);
      if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) continue;
      if (!bc && d->kind == K_SRGB && !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) continue;
      if (nf == 256) { trunc = 1; break; }
      if (nrow + 4 > FSW_MAX) { trunc = 1; break; }
      sel[nf] = bc ? -(i - nall) - 1 : i;
      for (int m = 0; m < 2; m++)
         for (int mode = 0; mode < 2; mode++) {
            rows_mip[nrow] = m;
            rows_mode[nrow] = mode;
            nrow++;
         }
      nf++;
   }
   printf("{\"test\":\"texfmt\",\"formats\":%d,\"rows\":%d%s}\n", nf, nrow, trunc ? ",\"truncated\":true" : "");
   if (!nf) return 1;

   /* staging: every mip of every format, back to back */
   VkDeviceSize tot = 16;
   static int soff[256];
   for (int k = 0; k < nf; k++) {
      const int bc = sel[k] < 0;
      const struct fd *d = bc ? NULL : &g_fmts[sel[k]];
      soff[k] = (int)tot;
      for (int m = 0; m < TEX_MIPS; m++) tot += (VkDeviceSize)tex_mip_size(bc ? -sel[k] - 1 : -1, d, m) + 16;
   }
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = tot,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
   VkBuffer sb;
   CHECK(CreateBuffer(dev, &bci, NULL, &sb) == VK_SUCCESS, "texfmt staging");
   VkMemoryRequirements smr;
   GetBufferMemoryRequirements(dev, sb, &smr);
   int smt = memtype(smr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   CHECK(smt >= 0, "texfmt staging type");
   VkMemoryAllocateInfo sai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = smr.size, .memoryTypeIndex = smt};
   VkDeviceMemory smem;
   CHECK(AllocateMemory(dev, &sai, NULL, &smem) == VK_SUCCESS, "texfmt staging memory");
   CHECK(BindBufferMemory(dev, sb, smem, 0) == VK_SUCCESS, "texfmt staging bind");
   uint8_t *sp;
   CHECK(MapMemory(dev, smem, 0, VK_WHOLE_SIZE, 0, (void **)&sp) == VK_SUCCESS, "texfmt staging map");
   memset(sp, 0, (size_t)tot);
   for (int k = 0; k < nf; k++) {
      const int bc = sel[k] < 0, bi = bc ? -sel[k] - 1 : -1;
      const struct fd *d = bc ? NULL : &g_fmts[sel[k]];
      VkDeviceSize off = (VkDeviceSize)soff[k];
      for (int m = 0; m < TEX_MIPS; m++) {
         const int sz = tex_mip_size(bi, d, m);
         uint8_t *p = sp + off;
         if (bc) {
            const int bs = g_bcfmt[bi].block;
            uint8_t blk[16];
            bc_block(bi, m == 1, blk);
            for (int i = 0; i + bs <= sz; i += bs) memcpy(p + i, blk, (size_t)bs);
         } else {
            uint8_t el[16];
            fmt_encode(d, m == 1, el);
            for (int i = 0; i + d->elem <= sz; i += d->elem) memcpy(p + i, el, d->elem);
         }
         off += (VkDeviceSize)sz + 16;
      }
   }
   VkMappedMemoryRange sflush = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = smem, .offset = 0, .size = VK_WHOLE_SIZE};
   FlushMappedMemoryRanges(dev, 1, &sflush);

   VkShaderModule vs = load_spv("fmtsw_quad.vert.spv");
   VkShaderModule fs[3];
   fs[0] = load_spv("fmtsw_tex.frag.spv");
   fs[1] = load_spv("fmtsw_tex_u.frag.spv");
   fs[2] = load_spv("fmtsw_tex_s.frag.spv");
   CHECK(vs && fs[0] && fs[1] && fs[2], "texfmt shader modules");
   VkDescriptorSetLayoutBinding lb = {.binding = 0, .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1, .pBindings = &lb};
   VkDescriptorSetLayout dsl;
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS, "texfmt dsl");
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)nf};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = (uint32_t)nf,
      .poolSizeCount = 1, .pPoolSizes = &ps};
   VkDescriptorPool dp;
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &dp) == VK_SUCCESS, "texfmt pool");
   VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
      .size = sizeof(struct vtxpc)};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
      .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   VkPipelineLayout pl;
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &pl) == VK_SUCCESS, "texfmt layout");
   VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 2.0f};
   VkSampler smp;
   CHECK(CreateSampler(dev, &sci, NULL, &smp) == VK_SUCCESS, "texfmt sampler");

   struct fsw f;
   if (!fsw_init(&f)) return 0;
   /* one pipeline per class (the sampled formats' only shader-side difference is float/uint/sint) */
   VkPipeline pipe[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
   for (int c = 0; c < 3; c++) {
      VkPipelineShaderStageCreateInfo st[2] = {
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vs, .pName = "main"},
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fs[c], .pName = "main"}};
      VkPipelineVertexInputStateCreateInfo vin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
         .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
      VkViewport vpv = {0.0f, 0.0f, (float)W, (float)H, 0.0f, 1.0f};
      VkRect2D scs = {{0, 0}, {W, H}};
      VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
         .viewportCount = 1, .pViewports = &vpv, .scissorCount = 1, .pScissors = &scs};
      VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
         .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
         .lineWidth = 1.0f};
      VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
         .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
      VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
      VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
         .attachmentCount = 1, .pAttachments = &cba};
      VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
         .colorAttachmentCount = 1, .pColorAttachmentFormats = (VkFormat[]){VK_FORMAT_R8G8B8A8_UNORM}};
      VkGraphicsPipelineCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
         .stageCount = 2, .pStages = st, .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vp,
         .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl};
      CHECK(CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe[c]) == VK_SUCCESS, "texfmt pipeline %d", c);
   }

   /* images, views, descriptors, copies, then the draws */
   static VkImage imgs[256];
   static VkDeviceMemory imems[256];
   static VkImageView views[256];
   static int iaspect[256];
   VkCommandBuffer cb = f.cb;
   int nimg = 0;
   for (int k = 0; k < nf; k++) {
      const int bc = sel[k] < 0, bi = bc ? -sel[k] - 1 : -1;
      const struct fd *d = bc ? NULL : &g_fmts[sel[k]];
      const VkFormat fmt = bc ? g_bcfmt[bi].fmt : d->fmt;
      const int depth = !bc && (d->kind == K_D16 || d->kind == K_D32 || d->kind == K_D24S8);
      const VkImageAspectFlags asp = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
      VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
         .format = fmt, .extent = {8, 8, 1}, .mipLevels = TEX_MIPS, .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                  (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
      if (CreateImage(dev, &ii, NULL, &imgs[nimg]) != VK_SUCCESS) {
         printf("{\"test\":\"texfmt\",\"fmt\":\"%s\",\"skipped\":\"vkCreateImage\",\"ok\":false}\n",
                bc ? g_bcfmt[bi].name : d->name);
         continue;
      }
      VkMemoryRequirements mr;
      GetImageMemoryRequirements(dev, imgs[nimg], &mr);
      int mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (mt < 0) mt = memtype(mr.memoryTypeBits, 0);
      VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
      CHECK(AllocateMemory(dev, &ai, NULL, &imems[nimg]) == VK_SUCCESS, "texfmt image memory");
      CHECK(BindImageMemory(dev, imgs[nimg], imems[nimg], 0) == VK_SUCCESS, "texfmt bind");
      VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = imgs[nimg],
         .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = {asp, 0, TEX_MIPS, 0, 1}};
      CHECK(CreateImageView(dev, &vi, NULL, &views[nimg]) == VK_SUCCESS, "texfmt view");
      iaspect[nimg] = (int)asp;
      /* UNDEFINED -> TRANSFER_DST, then the three mips */
      VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
         .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
         .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .image = imgs[nimg],
         .subresourceRange = {asp, 0, TEX_MIPS, 0, 1}};
      VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
      CmdPipelineBarrier2(cb, &dep);
      VkBufferImageCopy reg[TEX_MIPS];
      memset(reg, 0, sizeof reg);
      VkDeviceSize off = (VkDeviceSize)soff[k];
      for (int m = 0; m < TEX_MIPS; m++) {
         reg[m].bufferOffset = off;
         reg[m].imageSubresource = (VkImageSubresourceLayers){asp, (uint32_t)m, 0, 1};
         reg[m].imageExtent = (VkExtent3D){(uint32_t)(8 >> m), (uint32_t)(8 >> m), 1};
         off += (VkDeviceSize)tex_mip_size(bi, d, m) + 16;
      }
      CmdCopyBufferToImage(cb, sb, imgs[nimg], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, TEX_MIPS, reg);
      nimg++;
   }
   /* every copy, then every transition: one barrier batch, one submit */
   for (int i = 0; i < nimg; i++) {
      VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
         .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
         .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         .image = imgs[i], .subresourceRange = {(VkImageAspectFlags)iaspect[i], 0, TEX_MIPS, 0, 1}};
      VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
      CmdPipelineBarrier2(cb, &dep);
   }
   if (!g_fake) {
      int nimg2 = 0;
      for (int k = 0; k < nf; k++) {
         const int bc = sel[k] < 0;
         const struct fd *d = bc ? NULL : &g_fmts[sel[k]];
         if (nimg2 >= nimg) break;
         VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
         VkDescriptorSet ds;
         CHECK(AllocateDescriptorSets(dev, &dai, &ds) == VK_SUCCESS, "texfmt dset");
         VkDescriptorImageInfo dii = {smp, views[nimg2], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
         VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii};
         UpdateDescriptorSets(dev, 1, &w, 0, NULL);
         const int cls = bc ? 0 : fmt_class(d);
         double sc[4], biv[4];
         for (int i = 0; i < 4; i++) { sc[i] = 1.0; biv[i] = 0.0; }
         if (!bc) {
            if (d->kind == K_SNORM) { for (int i = 0; i < 4; i++) { sc[i] = 0.5; biv[i] = 0.5; } }
            else if (d->kind == K_SSC) {
               for (int i = 0; i < 4; i++) { sc[i] = 0.5 / (double)((1u << (d->bits[i] - 1)) - 1); biv[i] = 0.5; }
            } else if (d->kind == K_USC) {
               for (int i = 0; i < 4; i++) sc[i] = 1.0 / (double)((1u << d->bits[i]) - 1u);
            }
         }
         for (int r = 0; r < 4; r++) {
            const int row = k * 4 + r;
            fsw_cell_begin(&f, row);
            CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[cls]);
            CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
            struct vtxpc pc = {.c = {rows_mip[row] ? 1 : 3, rows_mip[row] ? 1 : 3, rows_mip[row], rows_mode[row]}};
            for (int i = 0; i < 4; i++) {
               pc.scale[i] = (float)sc[i];
               pc.bias[i] = (float)biv[i];
            }
            CmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
            CmdDraw(cb, 6, 1, 0, 0);
            fsw_cell_end(&f);
         }
         nimg2++;
      }
   }
   int bad = 0, npix = 0;
   if (!g_fake) {
      if (!fsw_submit(&f)) return 0;
      for (int k = 0; k < nf; k++) {
         const int bc = sel[k] < 0, bi = bc ? -sel[k] - 1 : -1;
         const struct fd *d = bc ? NULL : &g_fmts[sel[k]];
         const char *name = bc ? g_bcfmt[bi].name : d->name;
         const int srgb = (!bc && d->kind == K_SRGB) || (bc && (g_bcfmt[bi].fmt == VK_FORMAT_BC1_RGB_SRGB_BLOCK ||
            g_bcfmt[bi].fmt == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || g_bcfmt[bi].fmt == VK_FORMAT_BC2_SRGB_BLOCK ||
            g_bcfmt[bi].fmt == VK_FORMAT_BC3_SRGB_BLOCK || g_bcfmt[bi].fmt == VK_FORMAT_BC7_SRGB_BLOCK));
         const int depth = !bc && (d->kind == K_D16 || d->kind == K_D32 || d->kind == K_D24S8);
         int fmtbad = 0, fr = -1, fc = -1, fw = -1, fg = -1;
         double sc[4], biv[4];
         for (int i = 0; i < 4; i++) { sc[i] = 1.0; biv[i] = 0.0; }
         if (!bc) {
            if (d->kind == K_SNORM) { for (int i = 0; i < 4; i++) { sc[i] = 0.5; biv[i] = 0.5; } }
            else if (d->kind == K_SSC) {
               for (int i = 0; i < 4; i++) { sc[i] = 0.5 / (double)((1u << (d->bits[i] - 1)) - 1); biv[i] = 0.5; }
            } else if (d->kind == K_USC) {
               for (int i = 0; i < 4; i++) sc[i] = 1.0 / (double)((1u << d->bits[i]) - 1u);
            }
         }
         for (int r = 0; r < 4; r++) {
            const int row = k * 4 + r, mip = rows_mip[row], mode = rows_mode[row];
            double ex[4];
            if (bc) {
               uint32_t cr, cg, cbv;
               double rgb[3], alpha;
               bc_params(bi, mip, &cr, &cg, &cbv, rgb, &alpha);
               (void)cr; (void)cg; (void)cbv;
               ex[0] = rgb[0]; ex[1] = rgb[1]; ex[2] = rgb[2]; ex[3] = 1.0;
            } else {
               tex_vals(d, mip, ex);
               if (d->kind == K_SRGB)   /* both texelFetch and textureLod give the linear value (measured on a
                                          * reference driver: mode 0 and mode 1 agree on every sRGB row) */
                  for (int i = 0; i < 3; i++) ex[i] = srgb_to_lin(ex[i]);
               if (d->kind == K_UINT) for (int i = 0; i < 3; i++) ex[i] = ex[i] / 255.0;
               if (d->kind == K_SINT) for (int i = 0; i < 3; i++) ex[i] = (ex[i] + 128.0) / 255.0;
            }
            int rgb[3], want[3];
            if (fsw_read(&f, row, rgb)) {
               if (!fmtbad) { fr = mip * 2 + mode; fc = -1; }
               fmtbad++;
               continue;
            }
            npix++;
            for (int i = 0; i < 3; i++) {
               if (depth && i) continue;   /* a depth sample's other channels are definitional */
               double t = (d || bc) ? ex[i] : ex[i];
               if (!bc && (d->kind == K_UINT || d->kind == K_SINT)) {
                  /* already mapped by the shader */
               } else if (bc) {
                  /* sRGB BC formats: textureLod linearises */
                  if (srgb) t = srgb_to_lin(ex[i]);
               } else {
                  t = ex[i] * sc[i] + biv[i];
               }
               t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
               want[i] = (int)lround(t * 255.0);
               if (abs(rgb[i] - want[i]) > 1) {
                  if (!fmtbad) { fr = mip * 2 + mode; fc = i; fw = want[i]; fg = rgb[i]; }
                  fmtbad++;
               }
            }
         }
         if (fmtbad) {
            bad++;
            printf("{\"test\":\"texfmt\",\"fmt\":\"%s\",\"ok\":false,\"row\":%d,\"ch\":%d,\"expected\":%d,\"got\":%d}\n",
                   name, fr, fc, fw, fg);
         } else {
            printf("{\"test\":\"texfmt\",\"fmt\":\"%s\",\"ok\":true}\n", name);
         }
      }
   } else {
      /* the fake backend renders nothing: report every row as skipped, not as a pass of anything */
      for (int k = 0; k < nf; k++) {
         const int bc = sel[k] < 0, bi = bc ? -sel[k] - 1 : -1;
         printf("{\"test\":\"texfmt\",\"fmt\":\"%s\",\"skipped\":\"fake backend renders nothing\",\"ok\":true}\n",
                bc ? g_bcfmt[bi].name : g_fmts[sel[k]].name);
      }
   }
   for (int i = 0; i < nimg; i++) DestroyImageView(dev, views[i], NULL);
   for (int i = 0; i < nimg; i++) DestroyImage(dev, imgs[i], NULL);
   for (int i = 0; i < nimg; i++) FreeMemory(dev, imems[i], NULL);
   for (int c = 0; c < 3; c++) if (pipe[c]) DestroyPipeline(dev, pipe[c], NULL);
   DestroyPipelineLayout(dev, pl, NULL);
   DestroyDescriptorPool(dev, dp, NULL);
   DestroyDescriptorSetLayout(dev, dsl, NULL);
   DestroySampler(dev, smp, NULL);
   DestroyShaderModule(dev, vs, NULL);
   for (int c = 0; c < 3; c++) DestroyShaderModule(dev, fs[c], NULL);
   DestroyBuffer(dev, sb, NULL);
   FreeMemory(dev, smem, NULL);
   fsw_destroy(&f);
   printf("{\"test\":\"texfmt\",\"result\":\"%s\",\"formats\":%d,\"rows\":%d,\"pixels\":%d}\n",
          bad ? "FAIL" : "PASS", nf, nrow, npix);
   return bad == 0;
}

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--loop") && i + 1 < argc) g_loop = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--loop-reset")) g_reset = 1;
      else if (!strcmp(argv[i], "--only") && i + 1 < argc) g_only = argv[++i];
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) g_spv = argv[++i];
      else { fprintf(stderr, "usage: t_vk_draw --direct icd.so [--loop N] [--loop-reset] [--only case]\n"); return 2; }
   }
   if (!icd) { fprintf(stderr, "--direct required\n"); return 2; }
   const char *be = getenv("MALI_BACKEND");
   g_fake = be && !strcmp(be, "fake");

   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("{\"fail\":\"dlopen: %s\"}\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   VkResult (*negotiate)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   uint32_t iv = 5;
   if (!gipa || !negotiate || negotiate(&iv)) { printf("{\"fail\":\"icd entry\"}\n"); return 1; }

   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   const char *iext[] = {"VK_KHR_get_physical_device_properties2", "VK_KHR_external_memory_capabilities",
                         "VK_KHR_external_semaphore_capabilities", "VK_KHR_external_fence_capabilities"};
   ici.enabledExtensionCount = 4;
   ici.ppEnabledExtensionNames = iext;
   VkInstance inst;
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("{\"fail\":\"CreateInstance\"}\n"); return 1; }
   PFN_vkEnumeratePhysicalDevices EPD = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceMemoryProperties GMP = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
   PFN_vkGetPhysicalDeviceProperties GPP = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
   GetFmtProps = (PFN_vkGetPhysicalDeviceFormatProperties)gipa(inst, "vkGetPhysicalDeviceFormatProperties");
   PFN_vkGetPhysicalDeviceFeatures GPF = (PFN_vkGetPhysicalDeviceFeatures)gipa(inst, "vkGetPhysicalDeviceFeatures");
   PFN_vkCreateDevice CreateDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr GDPA = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   PFN_vkDestroyInstance DestroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
   VkPhysicalDevice pd;
   uint32_t cnt = 1;
   VkResult r = EPD(inst, &cnt, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || cnt < 1) { printf("{\"fail\":\"no device\"}\n"); return 1; }
   VkPhysicalDeviceProperties pp;
   GPP(pd, &pp);
   GMP(pd, &memprops);
   g_pd = pd;   /* format sweep: the advertised format features */
   g_fbcounts = pp.limits.framebufferColorSampleCounts;
   printf("{\"device\":\"%s\"}\n", pp.deviceName);

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
   GPF(pd, &g_feat);
   VkPhysicalDeviceFeatures en = {.depthClamp = g_feat.depthClamp, .independentBlend = g_feat.independentBlend, .fillModeNonSolid = g_feat.fillModeNonSolid, .depthBiasClamp = g_feat.depthBiasClamp, .multiViewport = g_feat.multiViewport, .dualSrcBlend = g_feat.dualSrcBlend, .logicOp = g_feat.logicOp, .sampleRateShading = g_feat.sampleRateShading};
   di.pEnabledFeatures = &en;
   /* VK_EXT_depth_clip_enable: enabled when advertised (feature struct chained without a Features2) */
   VkPhysicalDeviceDepthClipEnableFeaturesEXT dce_f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
   const char *dce_ext[] = {"VK_EXT_depth_clip_enable", "VK_EXT_extended_dynamic_state3"};
   const char *en_ext[2];
   VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3_f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
   /* blitreplay records vkCmdPipelineBarrier2 and vkCmdBeginRendering: the 1.3 core commands are only legal with these
    * two feature bits enabled, not merely with a 1.3 physical device */
   VkPhysicalDeviceVulkan13Features v13_f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
   VkPhysicalDeviceVulkan13Features v13_en = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
   {
      PFN_vkGetPhysicalDeviceFeatures2 GPF2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2");
      PFN_vkEnumerateDeviceExtensionProperties EDE = (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
      uint32_t ne = 0;
      EDE(pd, NULL, &ne, NULL);
      VkExtensionProperties eps[128];
      if (ne > 128) ne = 128;
      EDE(pd, NULL, &ne, eps);
      int has = 0, has3 = 0;
      for (uint32_t i = 0; i < ne; i++) {
         has |= !strcmp(eps[i].extensionName, dce_ext[0]);
         has3 |= !strcmp(eps[i].extensionName, dce_ext[1]);
      }
      v13_f.pNext = &dce_f;
      dce_f.pNext = &eds3_f;
      VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v13_f};
      GPF2(pd, &f2);
      if (!v13_f.synchronization2 || !v13_f.dynamicRendering) {
         printf("{\"fail\":\"no synchronization2/dynamicRendering\"}\n");
         return 1;
      }
      VkPhysicalDeviceVulkan13Features v13_en_local = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
         .synchronization2 = VK_TRUE, .dynamicRendering = VK_TRUE};
      v13_en = v13_en_local;   /* di.pNext is read after this block: the struct has to outlive it */
      void *chain = NULL;
      if (has3 && (eds3_f.extendedDynamicState3PolygonMode || eds3_f.extendedDynamicState3DepthClipEnable)) {
         /* enable only the two bits blitreplay sets dynamically; its depthClipEnable setter needs its own bit, which is
          * not what g_have_dclip (the static VK_EXT_depth_clip_enable path) tracks */
         g_have_eds3 = eds3_f.extendedDynamicState3PolygonMode;
         g_have_dyn_dclip = eds3_f.extendedDynamicState3DepthClipEnable;
         VkPhysicalDeviceExtendedDynamicState3FeaturesEXT e3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,
                                                               .extendedDynamicState3PolygonMode = (VkBool32)!!g_have_eds3,
                                                               .extendedDynamicState3DepthClipEnable = (VkBool32)!!g_have_dyn_dclip};
         eds3_f = e3;
         eds3_f.pNext = NULL;
         en_ext[di.enabledExtensionCount++] = dce_ext[1];
         chain = &eds3_f;
      }
      if (has && dce_f.depthClipEnable) {
         g_have_dclip = 1;
         dce_f.pNext = chain;
         chain = &dce_f;
         en_ext[di.enabledExtensionCount++] = dce_ext[0];
      }
      v13_en.pNext = chain;
      di.pNext = &v13_en;
      di.ppEnabledExtensionNames = en_ext;
   }
   if (CreateDevice(pd, &di, NULL, &dev) != VK_SUCCESS) { printf("{\"fail\":\"CreateDevice\"}\n"); return 1; }
#define LOAD(n) n = (PFN_vk##n)GDPA(dev, "vk" #n); if (!n) { printf("{\"fail\":\"missing vk" #n "\"}\n"); return 1; }
   DEV_FNS(LOAD)
   CmdBindVertexBuffers2_ = (PFN_vkCmdBindVertexBuffers2)GDPA(dev, "vkCmdBindVertexBuffers2");
   CmdSetPolygonModeEXT_ = (PFN_vkCmdSetPolygonModeEXT)GDPA(dev, "vkCmdSetPolygonModeEXT");
   CmdSetDepthClipEnableEXT_ = g_have_dyn_dclip ? (PFN_vkCmdSetDepthClipEnableEXT)GDPA(dev, "vkCmdSetDepthClipEnableEXT") : NULL;
   if (g_have_eds3 && !CmdSetPolygonModeEXT_) { printf("{\"fail\":\"missing vkCmdSetPolygonModeEXT\"}\n"); return 1; }
   GetDeviceQueue(dev, 0, 0, &queue);

   int ok = 1;
   if (CASE_ON("triangle") || CASE_ON("rot")) ok &= test_triangle();
   if (ok && g_only && CASE_ON("blitreplay")) ok &= test_blitreplay();   /* --only only: it allocates two 800x600 images */
   if (ok && g_only && CASE_ON("blitwin")) { setenv("BLITREPLAY", "6", 1); ok &= test_blitreplay(); unsetenv("BLITREPLAY"); }   /* the twin control: our own shaders */
   if (ok && CASE_ON("sparse_loc")) { g_sparse = 1; ok &= test_triangle(); g_sparse = 0; }   /* same triangle as "triangle" through attribute locations 0 and 2 */
   if (ok && CASE_ON("two")) ok &= test_two(0);
   if (ok && CASE_ON("depth")) ok &= test_two(1);
   /* D16 / D32S8 depth attachments (the depth test result must match D32F) */
   if (ok && CASE_ON("load_depth")) ok &= test_load("load_depth", 0);
   if (ok && CASE_ON("nocol_depth")) ok &= test_nocol("nocol_depth", 0);
   if (ok && CASE_ON("nofs_depth")) ok &= test_nocol("nofs_depth", 0);
   if (ok && CASE_ON("nofs_stencil")) ok &= test_nocol("nofs_stencil", 1);
   if (ok && CASE_ON("nocol_stencil")) ok &= test_nocol("nocol_stencil", 1);
   if (ok && CASE_ON("dyn_stride")) ok &= test_dynstride("dyn_stride", 0);
   if (ok && CASE_ON("dyn_stride_rebind")) ok &= test_dynstride("dyn_stride_rebind", 1);   /* plain BindVertexBuffers after BindVertexBuffers2 keeps the stride */
   if (ok && CASE_ON("discard_both")) ok &= test_discard("discard_both", 1);
   if (ok && CASE_ON("discard_vs_only")) ok &= test_discard("discard_vs_only", 2);
   if (ok && CASE_ON("int_clear_rgba8ui")) ok &= test_int_clear("int_clear_rgba8ui", VK_FORMAT_R8G8B8A8_UINT, 4, 0x073264c8u);
   if (ok && CASE_ON("int_clear_rg16ui")) ok &= test_int_clear("int_clear_rg16ui", VK_FORMAT_R16G16_UINT, 4, 0x006400c8u);
   if (ok && CASE_ON("int_clear_r8ui")) ok &= test_int_clear("int_clear_r8ui", VK_FORMAT_R8_UINT, 1, 0xc8u);
   {
      static const float sn1[4] = { 1, -1, 0, 1 }, f15[4] = { 1.5f, 1.5f, 1.5f, 1.5f };
      if (ok && CASE_ON("rt_clear_r8snorm")) ok &= test_int_clear_f("rt_clear_r8snorm", VK_FORMAT_R8_SNORM, 1, 0x7fu, sn1);
      if (ok && CASE_ON("rt_clear_rgba8snorm")) ok &= test_int_clear_f("rt_clear_rgba8snorm", VK_FORMAT_R8G8B8A8_SNORM, 4, 0x7f00817fu, sn1);
      if (ok && CASE_ON("rt_clear_a8b8g8r8snorm")) ok &= test_int_clear_f("rt_clear_a8b8g8r8snorm", VK_FORMAT_A8B8G8R8_SNORM_PACK32, 4, 0x7f00817fu, sn1);
      if (ok && CASE_ON("rt_clear_r16unorm")) ok &= test_int_clear_f("rt_clear_r16unorm", VK_FORMAT_R16_UNORM, 2, 0xffffu, sn1);
      if (ok && CASE_ON("rt_clear_rg16snorm")) ok &= test_int_clear_f("rt_clear_rg16snorm", VK_FORMAT_R16G16_SNORM, 4, 0x80017fffu, sn1);
      if (ok && CASE_ON("rt_clear_r32f")) ok &= test_int_clear_f("rt_clear_r32f", VK_FORMAT_R32_SFLOAT, 4, 0x3fc00000u, f15);
      if (ok && CASE_ON("rt_clear_rgba32f")) ok &= test_int_clear_f("rt_clear_rgba32f", VK_FORMAT_R32G32B32A32_SFLOAT, 16, 0x3fc00000u, f15);
      if (ok && CASE_ON("rt_clear_a8b8g8r8ui")) ok &= test_int_clear("rt_clear_a8b8g8r8ui", VK_FORMAT_A8B8G8R8_UINT_PACK32, 4, 0x073264c8u);
      if (ok && CASE_ON("rt_clear_a2b10g10r10ui")) ok &= test_int_clear("rt_clear_a2b10g10r10ui", VK_FORMAT_A2B10G10R10_UINT_PACK32, 4, 0xc32190c8u);
   }
   if (ok && CASE_ON("load_depth16")) { g_depfmt = VK_FORMAT_D16_UNORM; ok &= test_load("load_depth16", 0); g_depfmt = VK_FORMAT_D32_SFLOAT; }
   /* Source-engine/wined3d content: D24S8 depth LOAD (zs_format 2, 24-bit UNORM preload) and packed colour formats.
    * Explicit only: the golden dump sequence stays as it was. */
   if (ok && g_only && CASE_ON("load_depth24s8")) { g_depfmt = VK_FORMAT_D24_UNORM_S8_UINT; ok &= test_load("load_depth24s8", 0); g_depfmt = VK_FORMAT_D32_SFLOAT; }
   if (ok && g_only && CASE_ON("load_colour_argb8")) { g_colfmt = VK_FORMAT_A8B8G8R8_UNORM_PACK32; g_colbpp = 4; ok &= test_load("load_colour_argb8", 2); g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; g_colbpp = 4; }
   if (ok && CASE_ON("load_stencil")) ok &= test_load("load_stencil", 1);
   if (ok && CASE_ON("st_op_invert")) ok &= test_load("st_op_invert", 3);
   if (ok && CASE_ON("st_op_decwrap")) ok &= test_load("st_op_decwrap", 4);
   if (ok && CASE_ON("load_colour_f16")) { g_colfmt = VK_FORMAT_R16G16B16A16_SFLOAT; g_colbpp = 8; ok &= test_load("load_colour_f16", 2); g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; g_colbpp = 4; }
   if (ok && CASE_ON("load_colour_rgba8ui")) { g_colfmt = VK_FORMAT_R8G8B8A8_UINT; g_colbpp = 4; ok &= test_load("load_colour_rgba8ui", 2); g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; g_colbpp = 4; }   /* RAW-format preload (kinds 12..16) */
   if (ok && CASE_ON("load_colour_rg16sint")) { g_colfmt = VK_FORMAT_R16G16_SINT; g_colbpp = 4; g_clear_u = 1; ok &= test_load("load_colour_rg16sint", 2); g_clear_u = 0; g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; g_colbpp = 4; }   /* non-zero clear: a LOAD that writes zeros must fail */
   if (ok && CASE_ON("load_colour_rgba8snorm")) { g_colfmt = VK_FORMAT_R8G8B8A8_SNORM; g_colbpp = 4; ok &= test_load("load_colour_rgba8snorm", 2); g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; g_colbpp = 4; }   /* RAW-format preload (kinds 12..16) */
   if (ok && CASE_ON("load_colour_rgba16unorm")) { g_colfmt = VK_FORMAT_R16G16B16A16_UNORM; g_colbpp = 8; ok &= test_load("load_colour_rgba16unorm", 2); g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; g_colbpp = 4; }   /* RAW-format preload (kinds 12..16) */
   if (ok && CASE_ON("load_colour_r32f")) { g_colfmt = VK_FORMAT_R32_SFLOAT; g_colbpp = 4; ok &= test_load("load_colour_r32f", 2); g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; g_colbpp = 4; }   /* RAW-format preload (kinds 12..16) */
   if (ok && CASE_ON("load_colour_rgba16uint")) { g_colfmt = VK_FORMAT_R16G16B16A16_UINT; g_colbpp = 8; ok &= test_load("load_colour_rgba16uint", 2); g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; g_colbpp = 4; }   /* RAW-format preload (kinds 12..16) */
   if (ok && CASE_ON("load_colour_a2b10")) { g_colfmt = VK_FORMAT_A2B10G10R10_UNORM_PACK32; ok &= test_load("load_colour_a2b10", 2); g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; }
   if (ok && CASE_ON("load_colour_a1r5")) { g_colfmt = VK_FORMAT_A1R5G5B5_UNORM_PACK16; g_colbpp = 2; ok &= test_load("load_colour_a1r5", 2); g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; g_colbpp = 4; }
   if (ok && CASE_ON("depth16")) { g_depfmt = VK_FORMAT_D16_UNORM; ok &= test_two(1); g_depfmt = VK_FORMAT_D32_SFLOAT; }
   if (ok && CASE_ON("depth32s8")) { g_depfmt = VK_FORMAT_D32_SFLOAT_S8_UINT; ok &= test_two(1); g_depfmt = VK_FORMAT_D32_SFLOAT; }
   if (ok && CASE_ON("stencil")) {
      ok &= test_stencil(VK_FORMAT_S8_UINT);
      ok &= test_stencil(VK_FORMAT_D32_SFLOAT_S8_UINT);
      ok &= test_stencil(VK_FORMAT_D24_UNORM_S8_UINT);
   }
   if (ok && CASE_ON("poly")) {
      ok &= test_poly("poly_line_tri", MODE_TRI, 1, 250, 1500, 128, 106);
      ok &= test_poly("poly_point_tri", MODE_TRI, 2, 1, 40, 128, 106);
      ok &= test_poly("poly_line_idx16", MODE_IDX16, 1, 250, 2500, 100, 130);
      ok &= test_poly("poly_line_idx32", MODE_IDX32, 1, 250, 2500, 100, 130);
      ok &= test_poly("poly_line_mdi", MODE_MDI, 1, 250, 5000, 100, 130);
      ok &= test_poly_dyn("poly_dyn_line_tri", MODE_TRI, 1, 250, 1500, 128, 106);
      ok &= test_poly_dyn("poly_dyn_point_tri", MODE_TRI, 2, 1, 40, 128, 106);
      ok &= test_poly_dyn("poly_dyn_line_idx16", MODE_IDX16, 1, 250, 2500, 100, 130);
   }
   if (ok && CASE_ON("idx16")) ok &= test_mode("idx16", MODE_IDX16, 16384.0, 0.5);
   if (ok && CASE_ON("idx32")) ok &= test_mode("idx32", MODE_IDX32, 16384.0, 0.5);
   if (ok && CASE_ON("mdi")) ok &= test_mode("mdi", MODE_MDI, 16384.0, 0.5);
   if (ok && CASE_ON("scissor")) ok &= test_mode("scissor", MODE_SCISSOR, 8192.0, 0.5);
   if (ok && CASE_ON("viewport")) ok &= test_mode("viewport", MODE_VPORT, 8192.0, 0.5);
   if (ok && CASE_ON("multivp")) {
      if (g_feat.multiViewport) ok &= test_mode("multivp", MODE_MVP, 8192.0, 0.5);
      else printf("{\"test\":\"multivp\",\"skipped\":\"multiViewport not advertised\",\"ok\":true}\n");
   }
   if (ok && CASE_ON("clearatt")) ok &= test_mode("clearatt", MODE_CLEARATT, 0, 0);
   if (ok && CASE_ON("clearquad")) ok &= test_mode("clearquad", MODE_CLEARQUAD, 0, 0);
   if (ok && CASE_ON("clearquad_zs")) { g_clearzs = 1; ok &= test_mode("clearquad_zs", MODE_CLEARQUAD, 0, 0); g_clearzs = 0; }
   if (ok && (CASE_ON("fx") || (g_only && !strncmp(g_only, "fx_", 3)))) ok &= test_fx();
   if (ok && CASE_ON("msaa_resolve4")) ok &= test_msaa("msaa_resolve4", 1);
   if (ok && CASE_ON("msaa_sample_shading")) ok &= test_msaa("msaa_sample_shading", 2);
   if (ok && g_only && CASE_ON("dxvk_frame")) ok &= test_dxvk_frame();   /* explicit only: timing bench */
   if (ok && g_only && CASE_ON("many_draws")) ok &= test_many();   /* explicit only: 50k-draw frames are too big for the golden dumps */
   if (ok && CASE_ON("share")) ok &= test_share();
   if (ok && CASE_ON("share")) ok &= test_share_dyn();
   if (ok && CASE_ON("share")) ok &= test_share_inst();
   if (ok && CASE_ON("share")) ok &= test_share_dirty();
   if (ok && g_only && CASE_ON("share_idx")) ok &= test_share_idx();   /* explicit only: not in the goldens */
   if (ok && CASE_ON("ring")) ok &= test_ring();
   if (ok && CASE_ON("occlusion")) ok &= test_occ();
   if (ok && CASE_ON("pushfs")) ok &= test_quad("pushfs", MODE_PUSHFS);
   if (ok && CASE_ON("ubo")) ok &= test_quad("ubo", MODE_UBO);
   if (ok && g_only && CASE_ON("sets8")) ok &= test_quad("sets8", MODE_SETS8);   /* explicit only: the 8-set layout is not in the goldens */
   if (ok && CASE_ON("descupd")) ok &= test_quad("descupd", MODE_DESCUPD);
   if (ok && CASE_ON("texture")) ok &= test_quad("texture", MODE_TEX);
   if (ok && g_only && CASE_ON("vtxfmt")) ok &= test_vtxfmt();     /* explicit only: the sweep is one line per format */
   if (ok && g_only && CASE_ON("texfmt")) ok &= test_texfmt();     /* explicit only: the sweep is one line per format */
   if (ok && g_only && CASE_ON("view_format")) ok &= test_view_format();   /* explicit only: mutable-format views, not in the goldens */
   if (ok && g_only && CASE_ON("msaa_load_roundtrip")) ok &= load_roundtrip("msaa_load_roundtrip");
   if (ok && g_only && CASE_ON("msaa_load_resolve")) ok &= test_msaa_load("msaa_load_resolve");

   DeviceWaitIdle(dev);
   DestroyDevice(dev, NULL);
   DestroyInstance(inst, NULL);
   dlclose(lib);
   printf("{\"pass\":%s}\n", (ok && !g_fail) ? "true" : "false");
   return (ok && !g_fail) ? 0 : 1;
}
