// SPDX-License-Identifier: MIT
/* M8 test (offline): records the libkbase t_uniform scenes through the Vulkan entrypoints (command pool/buffer, vkCmdBeginRendering,
 * vkCmdBindPipeline, vkCmdSetViewport/Scissor, vkCmdBindVertexBuffers, vkCmdPushConstants, vkCmdDraw, vkCmdEndRendering) on the fake
 * ICD backend, submits them through libkbase's FAKE kbase backend (KB_FAKE_DUMP) and prints JSON lines. tests/cmp_cmd.py then diffs
 * the dump against the stream of libkbase's t_uniform / t_uniform_v13 and runs decode_stream.py's static rules.
 * Built twice: -DARCH=11 / -DARCH=13 (the core switches arch at runtime; only this test's shader tables are per arch).
 *   t_cmdNN --scene N        1 = uniform_scale_offset_colour, 2 = uniform_two_draws (t_uniform's scenes)
 *   t_cmdNN --extra          additional checks (clear-only frame, ClearAttachments, unsupported-feature errors, descriptor set, v-offset)
 * Env: KB_FAKE_DUMP / KB_FAKE_ARCH (libkbase backend_fake.c). */
#include <errno.h>
#include <sys/ioctl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mali_cmd.h"
#include "mali_query_core.h"
#include "mali_descriptor.h"
#include "vk_buffer.h"
#include "vk_command_buffer.h"

#include "kbase.h"
#include "kbase_uapi.h"
#include "kbase_draw.h"
#if ARCH == 13
#include "../../libkbase/tests/shaders_v13.h"
#else
#include "../../libkbase/tests/shaders_v11.h"
#endif

extern uint64_t kb_bo_extra_flags;

static int g_fail;
#define CHECK(cond, ...)                                                                                                       \
   do {                                                                                                                        \
      if (!(cond)) {                                                                                                           \
         g_fail++;                                                                                                             \
         printf("{\"test\":\"t_cmd%d\",\"pass\":false,\"error\":\"" #cond " | ", ARCH);                                        \
         printf(__VA_ARGS__);                                                                                                  \
         printf("\"}\n");                                                                                                      \
      }                                                                                                                        \
   } while (0)

#define W 256
#define H 256
static const float k_pos[12] = {-0.5f, -0.5f, 0, 1, 0.5f, -0.5f, 0, 1, 0, 0.5f, 0, 1};

struct draw { float so[4]; float z; float col[4]; };
struct scene { unsigned nd; struct draw d[2]; };
static const struct scene k_scenes[2] = {
   {1, {{{0.5f, 0.5f, 0.25f, -0.25f}, 0.5f, {0.8f, 0.4f, 0.2f, 1.0f}}}},
   {2, {{{0.5f, 0.5f, -0.5f, 0.0f}, 0.5f, {0.2f, 0.6f, 1.0f, 1.0f}}, {{0.5f, 0.5f, 0.5f, 0.0f}, 0.5f, {1.0f, 0.5f, 0.0f, 1.0f}}}},
};

/* ---- libkbase side: arena, queue, heap, shader BO, render target ---- */
struct fx {
   struct kb_ctx *ctx;
   struct kb_arena ar;
   struct kb_bo geo, shd, sync, rt, vb;
   struct kb_tiler_heap heap;
   struct kb_queue q;
   unsigned nsubmit;
};
static struct fx F;

static void *hw_alloc(void *u, size_t n, size_t al, uint64_t *va) { return kb_arena_alloc(&((struct fx *)u)->ar, n, al, va); }
static void hw_job_begin(void *u) { kb_arena_begin(&((struct fx *)u)->ar); }
static int hw_submit(void *u, const uint64_t *cs, unsigned n)
{
   struct fx *f = u;
   f->nsubmit++;
#if ARCH == 13
   /* v13 BOs are COHERENT_LOCAL: CPU->device MEM_SYNC before every submit (V13-NEXT2.md), as rung2_v13.h */
   struct kb_bo *bs[3] = {&f->ar.bo, &f->geo, &f->vb};
   for (int i = 0; i < 3; i++) {
      struct { uint64_t handle, user_addr, size; uint8_t type, pad[7]; } ms = {bs[i]->gpu_va, (uint64_t)(uintptr_t)bs[i]->cpu, bs[i]->size, 1, {0}};
      ioctl(kb_fd(f->ctx), _IOW(KBASE_IOCTL_TYPE, 15, ms), &ms);
   }
#endif
   return kb_queue_submit(&f->q, cs, n, KB_KICK_IOCTL);
}

/* ---- fake resolvers: handles are small integers, never dereferenced ---- */
static struct mali_compiled g_vs, g_fs;
static struct mali_imm g_vimm[4], g_fimm[4];
static unsigned g_vp_bytes = 20, g_fp_bytes = 16;

static int res_pipeline(VkPipeline p, struct mali_gfx_state *o)
{
   if ((uintptr_t)p != 1)
      return -1;
   memset(o, 0, sizeof *o);
   o->vs = &g_vs;
   o->fs = &g_fs;
   o->vs_va = F.shd.gpu_va;
   o->fs_va = F.shd.gpu_va + 512;
   o->nattr = 1;
   o->attr[0] = (struct mali_attr){KB_PIXFMT_RGBA32F, 0, 0, 16};
   o->depth_func = KB_FUNC_ALWAYS;
   return 0;
}
static int res_view(VkImageView v, struct mali_cmd_target *o)
{
   if ((uintptr_t)v != 1)
      return -1;
   *o = (struct mali_cmd_target){F.rt.gpu_va, W * 4, W, H, 0};
   return 0;
}
static int res_image(VkImage i, struct mali_cmd_target *o) { return res_view((VkImageView)i, o); }
static int res_shader(const struct vk_shader *s, const struct mali_compiled **c, uint64_t *va) { return -1; }
static const struct mali_cmd_res_ops k_res = {res_pipeline, res_view, res_image, res_shader};

/* ---- Vulkan helpers (through the ICD's own dispatch) ---- */
static PFN_vkVoidFunction (*gipa)(VkInstance, const char *);
static PFN_vkGetDeviceProcAddr gdpa;
static VkDevice dev;
#define DFN(n) PFN_vk##n n = (PFN_vk##n)gdpa(dev, "vk" #n); if (!n) { printf("{\"error\":\"no vk" #n "\"}\n"); exit(1); }

static struct vk_buffer *make_buffer(uint64_t va, uint64_t size)
{
   struct mali_device *md = mali_device_from_handle(dev);
   struct vk_buffer *b = calloc(1, sizeof *b);
   VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
   vk_buffer_init(&md->vk, b, &ci);
   b->device_address = va;
   b->size = size;
   return b;
}

static struct vk_command_buffer *new_cmd(VkCommandPool pool, VkCommandBuffer *h)
{
   DFN(AllocateCommandBuffers);
   DFN(BeginCommandBuffer);
   VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                     .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VkResult r = AllocateCommandBuffers(dev, &ai, h);
   CHECK(r == VK_SUCCESS, "alloc cmd %d", r);
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   r = BeginCommandBuffer(*h, &bi);
   CHECK(r == VK_SUCCESS, "begin %d", r);
   struct vk_command_buffer *vc = vk_command_buffer_from_handle(*h);
   vc->dynamic_graphics_state.rs.depth_clip_enable = VK_MESA_DEPTH_CLIP_ENABLE_NOT_CLAMP;   /* Vulkan default (no pipeline here to set it) */
   return vc;
}

static VkRenderingInfo rendering_info(VkRenderingAttachmentInfo *ca, bool clear)
{
   *ca = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = (VkImageView)(uintptr_t)1,
                                     .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
   /* t_uniform clear colour: bytes 0x10,0x20,0x40,0x80 */
   ca->clearValue.color.float32[0] = 0x10 / 255.0f;
   ca->clearValue.color.float32[1] = 0x20 / 255.0f;
   ca->clearValue.color.float32[2] = 0x40 / 255.0f;
   ca->clearValue.color.float32[3] = 0x80 / 255.0f;
   return (VkRenderingInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {W, H}}, .layerCount = 1,
                            .colorAttachmentCount = 1, .pColorAttachments = ca};
}

static bool g_ops_in_pass;
static uint64_t g_occ_va;   /* --m13: occlusion counter address set after BeginRendering (mali_cmd_set_occlusion) */

static void record_scene(VkCommandBuffer cb, const struct scene *sc, VkBuffer vbuf)
{
   DFN(CmdSetViewport);
   DFN(CmdSetScissor);
   DFN(CmdBindVertexBuffers);
   DFN(CmdPushConstants);
   DFN(CmdDraw);
   DFN(CmdBindPipeline);
   VkRenderingAttachmentInfo ca;
   VkRenderingInfo ri = rendering_info(&ca, true);
   mali_CmdBeginRendering(cb, &ri);
   if (g_occ_va)
      mali_cmd_set_occlusion(mali_cmd_buffer_from_vk(vk_command_buffer_from_handle(cb)), g_occ_va, MALI_OCC_MODE_COUNTER);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)(uintptr_t)1);
   VkViewport vp = {0, 0, W, H, 0.0f, 1.0f};
   VkRect2D scr = {{0, 0}, {W, H}};
   CmdSetViewport(cb, 0, 1, &vp);
   CmdSetScissor(cb, 0, 1, &scr);
   VkDeviceSize off = 0;
   CmdBindVertexBuffers(cb, 0, 1, &vbuf, &off);
   for (unsigned i = 0; i < sc->nd; i++) {
      /* VS push block (scale/offset vec4 + z), FS push block (colour vec4): 20 and 16 bytes, like the t_uniform shaders */
      float vpush[5] = {sc->d[i].so[0], sc->d[i].so[1], sc->d[i].so[2], sc->d[i].so[3], sc->d[i].z};
      CmdPushConstants(cb, VK_NULL_HANDLE, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof vpush, vpush);
      CmdPushConstants(cb, VK_NULL_HANDLE, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof sc->d[i].col, sc->d[i].col);
      CmdDraw(cb, 3, 1, 0, 0);
   }
   if (g_ops_in_pass) {
      struct mali_op e = {.kind = MALI_OP_AVAIL, .va = F.vb.gpu_va + 448, .val = 1};
      mali_cmd_push_ops(mali_cmd_buffer_from_vk(vk_command_buffer_from_handle(cb)), &e, 1);
   }
   mali_CmdEndRendering(cb);
}

/* scene with an op (SYNC_SET32 availability, the EndQuery op) recorded inside the render pass: it must retire after the pass frame */
static void record_scene_ops(VkCommandBuffer cb, const struct scene *sc, VkBuffer vbuf)
{
   g_ops_in_pass = true;
   record_scene(cb, sc, vbuf);
   g_ops_in_pass = false;
}

static void fill_compiled(void)
{
   memset(&g_vs, 0, sizeof g_vs);
   memset(&g_fs, 0, sizeof g_fs);
   g_vs.arch = g_fs.arch = ARCH;
   g_vs.bin = (uint8_t *)vs_uni_bin;
   g_vs.bin_size = VS_UNI_SIZE;
   g_vs.work_reg_count = VS_UNI_WORK_REG_COUNT;
   g_vs.preload = VS_UNI_PRELOAD;
   g_vs.fau_count = VS_UNI_FAU_COUNT;
   g_vs.fau_reserved = VS_UNI_FAU_RESERVED;
   g_vs.nimm = VS_UNI_NIMM;
   g_vs.push_const_bytes = g_vp_bytes;
   g_vs.vs_secondary_enable = VS_UNI_SECONDARY_ENABLE;
   g_vs.vs_secondary_offset = VS_UNI_SECONDARY_OFFSET;
   g_vs.vs_secondary_work_reg_count = VS_UNI_SECONDARY_WORK_REG_COUNT;
   g_vs.vs_secondary_preload = VS_UNI_SECONDARY_PRELOAD;
   g_vs.generic_size_B = VS_UNI_GENERIC_SIZE_B;
   for (unsigned i = 0; i < VS_UNI_NIMM; i++)
      g_vimm[i] = (struct mali_imm){vs_uni_imm[i][0], vs_uni_imm[i][1]};
   g_vs.imm = g_vimm;
   g_fs.bin = (uint8_t *)fs_uni_bin;
   g_fs.bin_size = FS_UNI_SIZE;
   g_fs.work_reg_count = FS_UNI_WORK_REG_COUNT;
   g_fs.preload = FS_UNI_PRELOAD;
   g_fs.fau_count = FS_UNI_FAU_COUNT;
   g_fs.fau_reserved = FS_UNI_FAU_RESERVED;
   g_fs.nimm = FS_UNI_NIMM;
   g_fs.push_const_bytes = g_fp_bytes;
   g_fs.imm = g_fimm;
}

static int setup_kb(void)
{
   int r = kb_open(&F.ctx, NULL);
   if (r) { printf("{\"error\":\"kb_open %d %s\"}\n", r, kb_last_error()); return -1; }
#if ARCH == 13
   kb_bo_extra_flags = 0x800;
#endif
   int e = kb_bo_alloc(F.ctx, 64 * 1024, KB_BO_GPU_RD | KB_BO_GPU_WR, &F.ar.bo);
   e |= kb_bo_alloc(F.ctx, 4096 + 65536, KB_BO_GPU_RD | KB_BO_GPU_WR, &F.geo);
   e |= kb_bo_alloc(F.ctx, 4096, KB_BO_GPU_RD | KB_BO_GPU_EX, &F.shd);
   e |= kb_bo_alloc(F.ctx, 4096, KB_BO_CSF_EVENT | KB_BO_GPU_RD | KB_BO_GPU_WR, &F.sync);
   e |= kb_bo_alloc(F.ctx, W * H * 4, KB_BO_GPU_RD | KB_BO_GPU_WR, &F.rt);
   e |= kb_bo_alloc(F.ctx, 4096, KB_BO_GPU_RD | KB_BO_GPU_WR, &F.vb);
   if (e) { printf("{\"error\":\"bo alloc\"}\n"); return -1; }
   memset(F.sync.cpu, 0, 4096);
   memset(F.geo.cpu, 0, 4096 + 65536);
   memset(F.shd.cpu, 0, 4096);
   memcpy(F.shd.cpu, vs_uni_bin, VS_UNI_SIZE);
   memcpy((char *)F.shd.cpu + 512, fs_uni_bin, FS_UNI_SIZE);
   memcpy(F.vb.cpu, k_pos, sizeof k_pos);
   if (kb_queue_create(F.ctx, NULL, &F.q)) { printf("{\"error\":\"queue\"}\n"); return -1; }
   if (kb_tiler_heap_create(F.ctx, 2u << 20, 1, 4, &F.heap)) { printf("{\"error\":\"heap\"}\n"); return -1; }
   return 0;
}

static int hw_submit_multi(void *u, const uint64_t *cs, unsigned n, unsigned nframes) { ((struct fx *)u)->nsubmit += nframes - 1; return hw_submit(u, cs, n); }   /* nsubmit counts frames */
static void hw_drain(void *u) { (void)u; }   /* fake submits are synchronous */

static struct mali_hw make_hw(void)
{
   struct mali_hw hw = {.user = &F, .alloc = hw_alloc, .job_begin = hw_job_begin, .submit = hw_submit, .submit_multi = hw_submit_multi, .drain = hw_drain};
   hw.env = (struct mali_env){.arch = ARCH,
#if ARCH == 13
                              .hmask = 0, .tile_log2 = 12, .tf_or = 0x40000,
#else
                              .hmask = 0xfe,
#endif
                              .slot = 3,
                              .heap_ctx_va = F.heap.ctx_va, .heap_first_chunk_va = F.heap.first_chunk_va,
                              .heap_chunk_size = F.heap.chunk_size, .geo_cpu = F.geo.cpu, .geo_va = F.geo.gpu_va,
                              .sync_va = F.sync.gpu_va};
   return hw;
}

int main(int argc, char **argv)
{
   int scene = 1;
   bool extra = false, m13 = false;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--scene") && i + 1 < argc) scene = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--extra")) extra = true;
      else if (!strcmp(argv[i], "--m13")) m13 = true;
   }
   setenv("MALI_BACKEND", "fake", 1);
   setenv("MALI_FAKE_GPU", ARCH == 13 ? "v13" : "v11", 1);
   fill_compiled();
   if (setup_kb())
      return 1;

   /* ---- ICD bring-up (instance -> device), as t_icd ---- */
   extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char *);
   gipa = vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance ci_fn = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst;
   CHECK(ci_fn(&ici, NULL, &inst) == VK_SUCCESS, "CreateInstance");
   PFN_vkEnumeratePhysicalDevices enumpd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   PFN_vkCreateDevice cdev = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   VkPhysicalDevice pd[2];
   uint32_t npd = 2;
   enumpd(inst, &npd, pd);
   CHECK(npd >= 1, "no physical device");
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
   CHECK(cdev(pd[0], &dci, NULL, &dev) == VK_SUCCESS, "CreateDevice");
   struct mali_device *md = mali_device_from_handle(dev);
   md->vk.command_buffer_ops = &mali_cmd_ops;   /* M8 wiring item #1 */

   struct mali_hw hw = make_hw();
   mali_cmd_set_hw(&hw);
   mali_cmd_set_res(&k_res);

   DFN(CreateCommandPool);
   DFN(EndCommandBuffer);
   DFN(DestroyCommandPool);
   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   VkCommandPool pool;
   CHECK(CreateCommandPool(dev, &pci, NULL, &pool) == VK_SUCCESS, "CreateCommandPool");
   struct vk_buffer *vb = make_buffer(F.vb.gpu_va, sizeof k_pos);
   VkBuffer vbh = vk_buffer_to_handle(vb);

   VkCommandBuffer cb;
   VkResult r = VK_SUCCESS;
   CHECK(scene >= 1 && scene <= 2, "bad scene");
   if (!m13) {   /* --m13 dumps only its own frames: the decoder's REQ_RESOURCE rule is per dump, not per frame */
      struct vk_command_buffer *vcmd = new_cmd(pool, &cb);
      record_scene(cb, &k_scenes[scene - 1], vbh);
      r = EndCommandBuffer(cb);
      CHECK(r == VK_SUCCESS, "EndCommandBuffer %d", r);
      r = mali_cmd_submit(vcmd);
      CHECK(r == VK_SUCCESS, "mali_cmd_submit %d", r);
      CHECK(F.nsubmit == 1, "frames submitted %u", F.nsubmit);
      printf("{\"test\":\"t_cmd%d\",\"scene\":%d,\"frames\":%u,\"pass\":%s}\n", ARCH, scene, F.nsubmit, g_fail ? "false" : "true");
   }

   if (m13) {
      /* M13: an occlusion draw (OQ register + DCD occlusion mode), ops deferred out of the render pass, and an ops frame
       * (timestamp + availability, event wait, flush): 3 frames, the stream must pass the decoder's static rules (STORE_STATE waited). */
      VkCommandBuffer c3;
      struct vk_command_buffer *v5 = new_cmd(pool, &c3);
      g_occ_va = F.vb.gpu_va + 512;
      record_scene_ops(c3, &k_scenes[scene - 1], vbh);
      g_occ_va = 0;
      struct mali_op ops[4] = {{.kind = MALI_OP_TIMESTAMP, .va = F.vb.gpu_va + 256, .va2 = F.vb.gpu_va + 320},
                               {.kind = MALI_OP_AVAIL, .va = F.vb.gpu_va + 384, .val = 1},
                               {.kind = MALI_OP_EVENT_WAIT, .va = F.vb.gpu_va + 384},
                               {.kind = MALI_OP_FLUSH}};
      mali_cmd_push_ops(mali_cmd_buffer_from_vk(v5), ops, 4);
      r = EndCommandBuffer(c3);
      CHECK(r == VK_SUCCESS, "m13 end=%d", r);
      unsigned before = F.nsubmit;
      r = mali_cmd_submit(v5);
      CHECK(r == VK_SUCCESS && F.nsubmit == before + 3, "m13 submit %d frames=%u (want 3)", r, F.nsubmit - before);
      printf("{\"test\":\"t_cmd%d\",\"m13\":true,\"pass\":%s}\n", ARCH, g_fail ? "false" : "true");
   }
   if (extra) {
      /* 1. unsupported features surface as VK_ERROR_FEATURE_NOT_PRESENT at vkEndCommandBuffer, they never crash */
      VkCommandBuffer c2;
      new_cmd(pool, &c2);
      {
         VkRenderingAttachmentInfo ca;
         VkRenderingInfo ri = rendering_info(&ca, false);
         ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
         mali_CmdBeginRendering(c2, &ri);
      }
      r = EndCommandBuffer(c2);
      CHECK(r == VK_ERROR_FEATURE_NOT_PRESENT, "loadOp LOAD end=%d", r);

      /* VK_KHR_load_store_op_none: LOAD_OP_NONE takes the LOAD path (same verdict), STORE_OP_NONE alone is accepted */
      new_cmd(pool, &c2);
      {
         VkRenderingAttachmentInfo ca;
         VkRenderingInfo ri = rendering_info(&ca, false);
         ca.loadOp = VK_ATTACHMENT_LOAD_OP_NONE_KHR;
         mali_CmdBeginRendering(c2, &ri);
      }
      r = EndCommandBuffer(c2);
      CHECK(r == VK_ERROR_FEATURE_NOT_PRESENT, "loadOp NONE must follow LOAD, end=%d", r);
      struct vk_command_buffer *vn = new_cmd(pool, &c2);
      {
         VkRenderingAttachmentInfo ca;
         VkRenderingInfo ri = rendering_info(&ca, true);
         ca.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
         mali_CmdBeginRendering(c2, &ri);
         mali_CmdEndRendering(c2);
      }
      r = EndCommandBuffer(c2);
      CHECK(r == VK_SUCCESS, "storeOp NONE end=%d", r);
      (void)vn;

      /* VK_KHR_maintenance6: PushConstants2 forwards to the 1.0 path (small push ok, oversize rejected the same way);
       * BindDescriptorSets2 with a null layout is rejected */
      {
         uint32_t pv[4] = {1, 2, 3, 4};
         VkPushConstantsInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO_KHR, .layout = (VkPipelineLayout)(uintptr_t)0,
                                      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof pv, .pValues = pv};
         new_cmd(pool, &c2);
         mali_CmdPushConstants2KHR(c2, &pi);
         r = EndCommandBuffer(c2);
         CHECK(r == VK_SUCCESS, "PushConstants2 end=%d", r);
         pi.offset = 1u << 20;
         new_cmd(pool, &c2);
         mali_CmdPushConstants2KHR(c2, &pi);
         r = EndCommandBuffer(c2);
         CHECK(r == VK_ERROR_FEATURE_NOT_PRESENT, "PushConstants2 oversize end=%d", r);
         VkBindDescriptorSetsInfoKHR bi = {.sType = VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO_KHR, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
         new_cmd(pool, &c2);
         mali_CmdBindDescriptorSets2KHR(c2, &bi);
         r = EndCommandBuffer(c2);
         CHECK(r == VK_ERROR_FEATURE_NOT_PRESENT, "BindDescriptorSets2 null layout end=%d", r);
      }

      new_cmd(pool, &c2);
      mali_CmdDrawIndexed(c2, 3, 1, 0, 0, 0);
      r = EndCommandBuffer(c2);
      CHECK(r == VK_ERROR_FEATURE_NOT_PRESENT, "indexed end=%d", r);

      /* 2. clear-only frame: vkCmdClearColorImage -> geometry-less FBD + RUN_FRAGMENT, no IDVS */
      DFN(CmdClearColorImage);
      struct vk_command_buffer *v3 = new_cmd(pool, &c2);
      VkClearColorValue cc = {.float32 = {0.0f, 0.5f, 1.0f, 1.0f}};
      VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      unsigned before = F.nsubmit;
      CmdClearColorImage(c2, (VkImage)(uintptr_t)1, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &rng);
      r = EndCommandBuffer(c2);
      CHECK(r == VK_SUCCESS, "clear image end=%d", r);
      r = mali_cmd_submit(v3);
      CHECK(r == VK_SUCCESS && F.nsubmit == before + 1, "clear-only submit %d n=%u", r, F.nsubmit - before);

      /* 3. full-area vkCmdClearAttachments before the first draw folds into the loadOp CLEAR; after a draw it is rejected */
      DFN(CmdBindPipeline);
      DFN(CmdBindVertexBuffers);
      DFN(CmdDraw);
      struct vk_command_buffer *v4 = new_cmd(pool, &c2);
      VkRenderingAttachmentInfo ca;
      VkRenderingInfo ri = rendering_info(&ca, false);
      mali_CmdBeginRendering(c2, &ri);
      VkClearAttachment cat = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0};
      cat.clearValue.color.float32[0] = 1.0f;
      VkClearRect crect = {.rect = {{0, 0}, {W, H}}, .layerCount = 1};
      mali_CmdClearAttachments(c2, 1, &cat, 1, &crect);
      mali_CmdEndRendering(c2);
      r = EndCommandBuffer(c2);
      CHECK(r == VK_SUCCESS, "ClearAttachments fold end=%d", r);
      before = F.nsubmit;
      r = mali_cmd_submit(v4);
      CHECK(r == VK_SUCCESS && F.nsubmit == before + 1, "fold submit %d", r);
      printf("{\"test\":\"t_cmd%d\",\"extra\":true,\"pass\":%s}\n", ARCH, g_fail ? "false" : "true");
   }
   DestroyCommandPool(dev, pool, NULL);
   kb_queue_destroy(&F.q);
   return g_fail ? 1 : 0;
}
