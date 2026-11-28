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
#include <unistd.h>

#include "mali_cmd.h"
#include "mali_image.h"
#include "mali_meta.h"
#include "mali_meta_vk.h"
#include "mali_query_core.h"
#include "mali_descriptor.h"
#include "vk_buffer.h"
#include "vk_command_buffer.h"
#include "vk_pipeline_layout.h"

#include "kbase.h"
#include "kbase_uapi.h"
#include "kbase_draw.h"
#include "kbase_fb.h"
#include "kbase_samples.h"
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

/* E (#308): the region of the deferred MSAA resolve. The attachment view and the resolve view are independent views with
 * their own mip level and array layer, and Vulkan resolves only the pass's render area — not the whole attachment. A
 * shared level/layer or a whole-attachment rect writes the wrong mip of the resolve image and overwrites pixels outside
 * the render area, which the app expects to keep. */
static void
test_resolve_region(void)
{
   struct mali_frame f;
   memset(&f, 0, sizeof f);
   f.width = 64; f.height = 48;
   f.area[0] = 8; f.area[1] = 4; f.area[2] = 16; f.area[3] = 16;
   f.resolve_level = 2; f.resolve_layer = 1;
   f.resolve_dst_level = 1; f.resolve_dst_layer = 3;
   VkImageResolve2 r;
   mali_resolve_region(&f, &r);
   CHECK(r.srcSubresource.mipLevel == 2 && r.srcSubresource.baseArrayLayer == 1,
         "src subresource %u/%u, expected the attachment view 2/1", r.srcSubresource.mipLevel, r.srcSubresource.baseArrayLayer);
   CHECK(r.dstSubresource.mipLevel == 1 && r.dstSubresource.baseArrayLayer == 3,
         "dst subresource %u/%u: the resolve view's mip/layer (1/3) is ignored", r.dstSubresource.mipLevel, r.dstSubresource.baseArrayLayer);
   CHECK(r.dstSubresource.layerCount == 1 && r.dstSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT, "dst subresource shape");
   CHECK(r.srcOffset.x == 8 && r.srcOffset.y == 4 && r.dstOffset.x == 8 && r.dstOffset.y == 4,
         "resolve offsets %d,%d -> %d,%d, expected the render area origin 8,4", r.srcOffset.x, r.srcOffset.y, r.dstOffset.x, r.dstOffset.y);
   CHECK(r.extent.width == 16 && r.extent.height == 16 && r.extent.depth == 1,
         "resolve extent %ux%u: covers the whole attachment instead of the 16x16 render area", r.extent.width, r.extent.height);
   /* no render area: BeginRendering leaves area[] zero, which means the whole attachment */
   memset(f.area, 0, sizeof f.area);
   mali_resolve_region(&f, &r);
   CHECK(r.extent.width == 64 && r.extent.height == 48 && r.srcOffset.x == 0 && r.srcOffset.y == 0 && r.dstOffset.x == 0,
         "no render area: %ux%u at %d,%d, expected the whole 64x48 attachment", r.extent.width, r.extent.height, r.srcOffset.x, r.srcOffset.y);
   int32_t area[] = {8, 4, 80, 40};
   CHECK(mali_resolve_1x_clamp(&r, area, 64, 48, 32, 20), "clipped resolve disappeared");
   CHECK(r.srcOffset.x == 8 && r.dstOffset.y == 4 && r.extent.width == 24 && r.extent.height == 16,
         "clipped resolve %d,%d %ux%u", r.srcOffset.x, r.srcOffset.y, r.extent.width, r.extent.height);
   area[0] = 40;
   CHECK(!mali_resolve_1x_clamp(&r, area, 64, 48, 32, 20), "out-of-bounds resolve was not skipped");
   r.srcSubresource.baseArrayLayer = 2;
   r.dstSubresource.baseArrayLayer = 3;
   mali_resolve_1x_slices(&r, true, true);
   CHECK(r.srcOffset.z == 2 && r.dstOffset.z == 3 &&
         r.srcSubresource.baseArrayLayer == 0 && r.dstSubresource.baseArrayLayer == 0,
         "3D slices not mapped to z offsets");
   r.srcOffset.z = r.dstOffset.z = 0;
   r.srcSubresource.baseArrayLayer = 2;
   r.dstSubresource.baseArrayLayer = 3;
   mali_resolve_1x_slices(&r, false, true);
   CHECK(r.srcOffset.z == 0 && r.srcSubresource.baseArrayLayer == 2 &&
         r.dstOffset.z == 3 && r.dstSubresource.baseArrayLayer == 0,
         "2D array layer was not preserved");
}

static void
test_resolve_view_formats(void)
{
   struct mali_image src = {0}, dst = {0};
   struct mali_image_desc d = {.format = VK_FORMAT_R8G8B8A8_UNORM, .type = VK_IMAGE_TYPE_2D,
      .extent = {16, 16, 1}, .levels = 1, .layers = 1, .samples = 1};
   CHECK(!mali_image_layout_init(ARCH, &d, &src.lay), "source layout");
   CHECK(!mali_image_layout_init(ARCH, &d, &dst.lay), "destination layout");
   src.va = 0x100000;
   dst.va = 0x200000;
   VkImageBlit2 r = {.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
      .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .srcOffsets = {{0, 0, 0}, {16, 16, 1}}, .dstOffsets = {{0, 0, 0}, {16, 16, 1}}};
   struct mali_meta_list l = {0};
   int err = mali_meta_plan_blit_views(&l, &src, &dst, &r,
                                       VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM);
   CHECK(!err && l.n == 1 && (l.j[0].push[20] & 8), "mutable BGRA view must swap channels");
   mali_meta_list_free(&l);
   d.format = VK_FORMAT_B8G8R8A8_UNORM;
   CHECK(!mali_image_layout_init(ARCH, &d, &dst.lay), "BGRA destination layout");
   err = mali_meta_plan_blit_views(&l, &src, &dst, &r,
                                   VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM);
   CHECK(!err && l.n == 1 && !(l.j[0].push[20] & 8), "matching views must not swap channels");
   mali_meta_list_free(&l);
   d.type = VK_IMAGE_TYPE_3D;
   d.extent.depth = 4;
   CHECK(!mali_image_layout_init(ARCH, &d, &src.lay), "3D source layout");
   CHECK(!mali_image_layout_init(ARCH, &d, &dst.lay), "3D destination layout");
   r.srcOffsets[0].z = 2; r.srcOffsets[1].z = 3;
   r.dstOffsets[0].z = 3; r.dstOffsets[1].z = 4;
   err = mali_meta_plan_blit_views(&l, &src, &dst, &r,
                                   VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM);
   CHECK(!err && l.n == 1 &&
         l.j[0].bind[0].va + l.j[0].push[0] == src.va + 2 * src.lay.plane[0].slice[0].surface_stride &&
         l.j[0].bind[1].va + l.j[0].push[3] == dst.va + 3 * dst.lay.plane[0].slice[0].surface_stride,
         "3D blit must select each view slice");
   mali_meta_list_free(&l);
}

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
   struct kb_bo geo, shd, sync, rt, vb, prel;   /* prel: --loadstore compiled preload shaders */
   struct kb_tiler_heap heap;
   struct kb_queue q;
   unsigned nsubmit;
};
static struct fx F;

/* provoking-vertex test hook: the frame build allocates the sample table, then the tiler context (KB_TILER_CTX_SIZE), ..., the FBD (KB_FB_DESC_SIZE_ZS) */
static uint32_t *g_tctx;
static uint8_t *g_fbd;
static int g_after_smp;
static void *hw_alloc(void *u, size_t n, size_t al, uint64_t *va)
{
   void *p = kb_arena_alloc(&((struct fx *)u)->ar, n, al, va);
   if (n == sizeof kb_sample_positions) { g_after_smp = 1; return p; }
   if (g_after_smp && n == KB_TILER_CTX_SIZE) g_tctx = p;
   if (n == KB_FB_DESC_SIZE_ZS) g_fbd = p;
   g_after_smp = 0;
   return p;
}
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
static VkImageView g_resolve_test_view;
static int res_view(VkImageView v, struct mali_cmd_target *o)
{
   /* --loadstore attachment handles: 1 = RGBA8 colour, 2 = D32F depth, 3 = D32F + S8 (depth and stencil, separate plane VAs),
    * 4 = D24X8 (LOAD is unsupported but NONE is not). Handle 2 is also the D32 depth-only view of --extra logicOp-retained. */
   if ((uintptr_t)v == 5) { /* 4x colour: 64 x 32 */
      *o = (struct mali_cmd_target){.va = 0x100000, .row_stride = 256, .width = 64, .height = 32,
                                    .samples = 4, .surface_stride = 8192};
      return 0;
   }
   if ((uintptr_t)v == 6) { /* 4x S8: 64 x 48, with a padded sample plane */
      *o = (struct mali_cmd_target){.va = 0x200000, .row_stride = 64, .width = 64, .height = 48,
                                    .is_zs = true, .has_s = true, .s_va = 0x200000, .s_row_stride = 64,
                                    .samples = 4, .surface_stride = 3328, .s_surface_stride = 3328};
      return 0;
   }
   if ((uintptr_t)v == 7) { /* 4x depth, separate from the S8 target */
      *o = (struct mali_cmd_target){.va = 0x300000, .row_stride = 256, .width = 64, .height = 48,
                                    .zs_format = 1, .is_zs = true, .samples = 4, .surface_stride = 12288};
      return 0;
   }
   if ((uintptr_t)v == 8 || (uintptr_t)v == 9) {
      *o = (struct mali_cmd_target){.va = 0x300000, .row_stride = 256, .width = 63, .height = 47,
         .zs_format = (uintptr_t)v == 8 ? 2 : 1, .is_zs = true, .has_s = true,
         .s_va = 0x200000, .s_row_stride = 64, .samples = 4,
         .surface_stride = 12032, .s_surface_stride = 3072};
      return 0;
   }
   if ((uintptr_t)v == 1 || (g_resolve_test_view && v == g_resolve_test_view)) {
      *o = (struct mali_cmd_target){.va = F.rt.gpu_va, .row_stride = W * 4, .width = W, .height = H, .rt = NULL};
      return 0;
   }
   if ((uintptr_t)v == 2 || (uintptr_t)v == 3 || (uintptr_t)v == 4) {
      bool st = (uintptr_t)v == 3;
      *o = (struct mali_cmd_target){.va = F.rt.gpu_va, .row_stride = W * 4, .width = W, .height = H, .rt = NULL,
                                    .zs_format = (uintptr_t)v == 4 ? 2 : 1,   /* 4: D24X8, where LOAD is unsupported but NONE is not */
                                    .is_zs = true, .has_s = st, .s_va = st ? F.rt.gpu_va + 192 * 1024 : 0,
                                    .s_row_stride = st ? W : 0, .samples = 1};
      return 0;
   }
   return -1;
}
static int res_image(VkImage i, struct mali_cmd_target *o) { return res_view((VkImageView)i, o); }
static int res_shader(const struct vk_shader *s, const struct mali_compiled **c, uint64_t *va) { return -1; }
/* M15 write tracking: the fill/update case below needs a stand-in for the VkDeviceMemory behind a handle (the
 * mark path only compares pointers); NULL everywhere else, which is what the other cases rely on. */
static struct mali_memory *g_fake_buf_mem;
static struct mali_memory *res_buffer_mem(VkBuffer b) { (void)b; return g_fake_buf_mem; }
static const struct mali_cmd_res_ops k_res = {res_pipeline, res_view, res_image, res_shader,
                                              NULL, NULL, res_buffer_mem, NULL, NULL};

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

static void
test_resolve_mismatched_view(VkCommandPool pool)
{
   struct mali_image src = {0}, dst = {0};
   src.vk.format = VK_FORMAT_R8G8B8A8_UNORM;
   src.vk.mip_levels = 1;
   dst.vk.format = VK_FORMAT_R8_UNORM;
   dst.vk.mip_levels = 1;
   struct mali_image_view sv = {0}, dv = {0};
   sv.vk.base.type = dv.vk.base.type = VK_OBJECT_TYPE_IMAGE_VIEW;
   sv.vk.image = &src.vk;
   sv.vk.view_format = VK_FORMAT_R8G8B8A8_UNORM;
   dv.vk.image = &dst.vk;
   dv.vk.view_format = VK_FORMAT_R8G8B8A8_UNORM;
   g_resolve_test_view = mali_image_view_to_handle(&sv);
   VkCommandBuffer cb;
   struct vk_command_buffer *vc = new_cmd(pool, &cb);
   struct mali_cmd_buffer *m = mali_cmd_buffer_from_vk(vc);
   VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = g_resolve_test_view, .resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT,
      .resolveImageView = mali_image_view_to_handle(&dv),
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
   VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {{0, 0}, {W, H}}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
   mali_CmdBeginRendering(cb, &ri);
   CHECK(!m->cur.resolve_1x && !m->cur.resolve_dst, "mismatched resolve was recorded");
   mali_CmdEndRendering(cb);
   DFN(EndCommandBuffer);
   CHECK(EndCommandBuffer(cb) == VK_SUCCESS && m->nframes == 1,
         "mismatched resolve must skip without recording a copy");
   g_resolve_test_view = VK_NULL_HANDLE;
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
   e |= kb_bo_alloc(F.ctx, 64 * 1024, KB_BO_GPU_RD | KB_BO_GPU_EX, &F.prel);   /* --loadstore: mali_meta preload binaries */
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

/* Clear words of the RAW (non-blendable) render targets: the packed pixel replicated like pan_pack_raw (mali_render.c raw_special_words) */
static void
check_clear_words(int fmt, const float c[4], uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3, const char *what)
{
   bool ok = false;
   const struct mali_rtfmt *rt = mali_rtfmt_lookup(fmt, &ok);
   uint32_t w[4] = { 0 };
   CHECK(ok && rt, "clear words %s: no row", what);
   if (!ok || !rt)
      return;
   mali_rtfmt_clear_words(rt, c, w);
   CHECK(w[0] == w0 && w[1] == w1 && w[2] == w2 && w[3] == w3, "clear words %s: %08x %08x %08x %08x want %08x %08x %08x %08x", what, w[0], w[1], w[2], w[3], w0, w1, w2, w3);
}

static void
test_clear_words(void)
{
   const float one[4] = { 1.0f, 0.0f, 0.0f, 1.0f }, neg[4] = { -1.0f, 1.0f, 0.0f, 1.0f };
   float f[4];
   uint32_t u[4];
   check_clear_words(VK_FORMAT_R8_SNORM, one, 0x7f7f7f7f, 0x7f7f7f7f, 0x7f7f7f7f, 0x7f7f7f7f, "R8_SNORM 1");
   check_clear_words(VK_FORMAT_R8_SNORM, neg, 0x81818181, 0x81818181, 0x81818181, 0x81818181, "R8_SNORM -1");
   check_clear_words(VK_FORMAT_R8G8_SNORM, neg, 0x7f817f81, 0x7f817f81, 0x7f817f81, 0x7f817f81, "R8G8_SNORM");
   check_clear_words(VK_FORMAT_R8G8B8A8_SNORM, neg, 0x7f007f81, 0x7f007f81, 0x7f007f81, 0x7f007f81, "R8G8B8A8_SNORM");
   check_clear_words(VK_FORMAT_A8B8G8R8_SNORM_PACK32, neg, 0x7f007f81, 0x7f007f81, 0x7f007f81, 0x7f007f81, "A8B8G8R8_SNORM");
   check_clear_words(VK_FORMAT_R16_UNORM, one, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, "R16_UNORM 1");
   check_clear_words(VK_FORMAT_R16G16_UNORM, one, 0x0000ffff, 0x0000ffff, 0x0000ffff, 0x0000ffff, "R16G16_UNORM");
   check_clear_words(VK_FORMAT_R16G16B16A16_UNORM, one, 0x0000ffff, 0xffff0000, 0x0000ffff, 0xffff0000, "R16G16B16A16_UNORM");
   check_clear_words(VK_FORMAT_R16_SNORM, one, 0x7fff7fff, 0x7fff7fff, 0x7fff7fff, 0x7fff7fff, "R16_SNORM 1");
   check_clear_words(VK_FORMAT_R16G16B16A16_SNORM, neg, 0x7fff8001, 0x7fff0000, 0x7fff8001, 0x7fff0000, "R16G16B16A16_SNORM");
   f[0] = 1.0f; f[1] = 2.0f; f[2] = -0.5f; f[3] = 4.0f;
   memcpy(u, f, sizeof u);
   check_clear_words(VK_FORMAT_R32_SFLOAT, f, u[0], u[0], u[0], u[0], "R32_SFLOAT");
   check_clear_words(VK_FORMAT_R32G32_SFLOAT, f, u[0], u[1], u[0], u[1], "R32G32_SFLOAT");
   check_clear_words(VK_FORMAT_R32G32B32A32_SFLOAT, f, u[0], u[1], u[2], u[3], "R32G32B32A32_SFLOAT");
   u[0] = 1; u[1] = 2; u[2] = 3; u[3] = 1;
   memcpy(f, u, sizeof f);
   check_clear_words(VK_FORMAT_A2B10G10R10_UINT_PACK32, f, 0x40300801, 0x40300801, 0x40300801, 0x40300801, "A2B10G10R10_UINT");
   u[0] = 1; u[1] = 2; u[2] = 3; u[3] = 4;   /* A2B10G10R10_SINT: alpha is 2 bits, the value is masked */
   memcpy(f, u, sizeof f);
   check_clear_words(VK_FORMAT_A2B10G10R10_SINT_PACK32, f, 0x00300801, 0x00300801, 0x00300801, 0x00300801, "A2B10G10R10_SINT");
   u[0] = 7; u[1] = 0; u[2] = 0; u[3] = 0;
   memcpy(f, u, sizeof f);
   check_clear_words(VK_FORMAT_A8B8G8R8_UINT_PACK32, f, 0x00000007, 0x00000007, 0x00000007, 0x00000007, "A8B8G8R8_UINT");
}

/* fillModeNonSolid triangle assembly (mali_poly_assemble): per assembled triangle, no incomplete primitives, restart markers removed. */
static void
test_poly_assemble(void)
{
   uint32_t out[6 * 16];
   const uint32_t v4[4] = { 0, 1, 2, 3 };
   /* strip of 4 vertices = 2 triangles: 6 edge instances (shared edge 1-2 twice), 6 points */
   size_t n = mali_poly_assemble(1, 10, NULL, v4, 4, out);
   CHECK(n == 12, "poly strip4 lines: %zu entries want 12", n);
   unsigned shared = 0;
   for (size_t i = 0; i + 1 < n; i += 2)
      shared += (out[i] == 1 && out[i + 1] == 2) || (out[i] == 2 && out[i + 1] == 1);
   CHECK(shared == 2, "poly strip4 lines: shared edge x%u want 2", shared);
   CHECK(mali_poly_assemble(2, 10, NULL, v4, 4, out) == 6, "poly strip4 points");
   /* fan of 4 vertices */
   CHECK(mali_poly_assemble(1, 12, NULL, v4, 4, out) == 12, "poly fan4 lines");
   /* a single triangle keeps 3 edges / 3 points */
   CHECK(mali_poly_assemble(1, 8, NULL, v4, 3, out) == 6, "poly tri lines");
   CHECK(mali_poly_assemble(2, 8, NULL, v4, 3, out) == 3, "poly tri points");
   /* incomplete triangles produce nothing: list of 2, list of 4 (1 triangle), strip of 2 */
   CHECK(mali_poly_assemble(2, 8, NULL, v4, 2, out) == 0, "poly list2 points");
   CHECK(mali_poly_assemble(1, 8, NULL, v4, 2, out) == 0, "poly list2 lines");
   CHECK(mali_poly_assemble(2, 8, NULL, v4, 4, out) == 3, "poly list4 points");
   CHECK(mali_poly_assemble(2, 10, NULL, v4, 2, out) == 0, "poly strip2 points");
   /* restart: 0 1 2 R 3 4 5 -> two triangles, the marker is never a vertex */
   const uint32_t R = 0xffffu, vr[7] = { 0, 1, 2, R, 3, 4, 5 };
   n = mali_poly_assemble(2, 10, &R, vr, 7, out);
   bool bad = false;
   for (size_t i = 0; i < n; i++)
      bad |= out[i] == R;
   CHECK(n == 6 && !bad, "poly strip restart points: %zu entries, marker %d", n, bad);
   /* restart with short segments: 0 1 R 2 3 4 -> only the second segment (1 triangle) */
   const uint32_t vs[6] = { 0, 1, R, 2, 3, 4 };
   CHECK(mali_poly_assemble(2, 10, &R, vs, 6, out) == 3, "poly strip restart short segment");
   /* list topology ignores restart (restart pointer NULL for lists) */
   CHECK(mali_poly_assemble(2, 8, NULL, vr, 6, out) == 6, "poly list points");
}

/* storeOp DONT_CARE/NONE: the FBD render target's Write Enable (word 1 bit 0) is off, so the tile is not written back; STORE and an MSAA
 * resolve (which writes through the same RT) keep it on. */
static unsigned
fbd_rt_write_enable(bool store, bool resolve)
{
   struct mali_env env;
   struct mali_frame f;
   uint8_t fbd[KB_FB_DESC_SIZE_ZS];
   memset(&env, 0, sizeof env);
   memset(&f, 0, sizeof f);
   memset(fbd, 0, sizeof fbd);
   env.arch = 11;
   f.width = f.height = 64;
   f.has_colour = true;
   f.samples = resolve ? 4 : 1;
   f.colour.va = 0x100000;
   f.colour.row_stride = 256;
   f.colour.store = store;
   if (resolve) {
      f.colour.resolve_va = 0x200000;
      f.colour.resolve_row_stride = 256;
   }
   int rc = mali_render_pack_fbd(&env, &f, fbd, 0, 0x300000);
   CHECK(rc == 0, "pack_fbd rc %d", rc);
   uint32_t w1;
   memcpy(&w1, fbd + KB_FB_FBD_SIZE + 4, 4);
   return w1 & 1;
}

static void
test_store_op(void)
{
   CHECK(fbd_rt_write_enable(true, false) == 1, "storeOp STORE must write the tile back");
   CHECK(fbd_rt_write_enable(false, false) == 0, "storeOp DONT_CARE/NONE must not write the tile back (RT Write Enable)");
   CHECK(fbd_rt_write_enable(false, true) == 1, "MSAA resolve needs the RT write");
}

/* The stencil image can be taller than the colour framebuffer and its sample
 * plane can have padding.  The legacy descriptor uses framebuffer height. */
static void
test_stencil_store_stride_baseline(void)
{
   struct mali_env env = { .arch = ARCH, .tile_log2 = 10 };
   struct mali_frame f = {0};
   uint8_t fbd[KB_FB_DESC_SIZE_ZS], one[KB_FB_DESC_SIZE_ZS];
   uint32_t mode, stride;
   f.width = 64;
   f.height = 32;                 /* colour height; stencil height is 48 */
   f.has_colour = f.has_st = true;
   f.samples = 4;
   f.colour.va = 0x100000;
   f.colour.row_stride = 256;
   f.colour.store = true;
   f.st.va = 0x200000;
   f.st.row_stride = 64;
   f.st.surface_stride = 64 * 48 + 256; /* padded actual stencil plane */
   f.st.store = true;
   CHECK(mali_render_pack_fbd(&env, &f, fbd, 0, 0x300000) == 0, "stencil FBD pack");
   memcpy(&mode, fbd + KB_FB_FBD_SIZE, 4);
   memcpy(&stride, fbd + KB_FB_FBD_SIZE + 15 * 4, 4);
   CHECK(((mode >> 24) & 3) == 3 && stride == 64 * 48 + 256, "layered mode/stride %u/%u", (mode >> 24) & 3, stride);
   f.samples = 1;
   CHECK(mali_render_pack_fbd(&env, &f, fbd, 0, 0x300000) == 0, "1x pack");
   memcpy(&mode, fbd + KB_FB_FBD_SIZE, 4);
   memcpy(&stride, fbd + KB_FB_FBD_SIZE + 15 * 4, 4);
   CHECK(((mode >> 24) & 3) == 0 && stride == 64 * 32, "1x unchanged");
   memcpy(one, fbd, sizeof one);
   f.st.surface_stride = 0;
   CHECK(mali_render_pack_fbd(&env, &f, fbd, 0, 0x300000) == 0 && !memcmp(one, fbd, sizeof fbd), "1x byte equal with or without sample stride");
   f.st.surface_stride = 64 * 48 + 256;
   f.samples = 4;
   f.has_zs = true;
   f.zs.zs_format = KB_ZS_D32F;
   f.zs.va = 0x400000;
   f.zs.row_stride = 256;
   f.zs.surface_stride = 12544;
   CHECK(mali_render_pack_fbd(&env, &f, fbd, 0, 0x300000) == 0, "depth pack");
   memcpy(&mode, fbd + KB_FB_FBD_SIZE, 4);
   memcpy(&stride, fbd + KB_FB_FBD_SIZE + 11 * 4, 4);
   CHECK(((mode >> 8) & 3) == 3 && ((mode >> 24) & 3) == 3 && stride == 12544, "depth layered mode/stride");
}

/* Exercise the producer, not just a hand-constructed mali_frame. */
static void
test_stencil_store_recording(VkCommandPool pool)
{
   DFN(EndCommandBuffer);
   for (unsigned i = 0; i < 3; i++) {
      VkCommandBuffer cb;
      struct vk_command_buffer *vc = new_cmd(pool, &cb);
      VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
         .imageView = (VkImageView)(uintptr_t)5, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
      VkRenderingAttachmentInfo sa = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
         .imageView = (VkImageView)(uintptr_t)6, .imageLayout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = i == 1 ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE};
      VkRenderingAttachmentInfo da = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
         .imageView = (VkImageView)(uintptr_t)7, .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
      VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {64, 32}},
         .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca,
         .pStencilAttachment = &sa, .pDepthAttachment = i == 2 ? &da : NULL};
      mali_CmdBeginRendering(cb, &ri);
      struct mali_cmd_buffer *cmd = mali_cmd_buffer_from_vk(vc);
      const struct mali_frame *f = &cmd->cur;
      CHECK(cmd->in_render && f->has_colour && f->has_st && f->samples == 4 && f->height == 32 &&
            f->st.va == 0x200000 && f->st.row_stride == 64, "record stencil case %u shape", i);
      CHECK(f->has_zs == (i == 2), "record stencil case %u depth", i);
      CHECK(f->st.surface_stride == 3328u,
            "record stencil case %u plane stride %u", i, f->st.surface_stride);
      mali_CmdEndRendering(cb);
      CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "record stencil case %u end", i);
   }
}

/* Ring wrap + chunking (host, fake kbase backend). A 4 KiB ring, a 3000-instruction stream and pieces from
 * kb_queue_piece_instrs: every piece must be readable at its masked ring address as it crosses the end of the ring,
 * a stream longer than the ring must still be accepted in pieces, and one oversized block must be refused. */
static int
test_ringwrap(void)
{
   enum { N = 3000 };
   uint64_t *ins = malloc(N * sizeof *ins);
   if (!ins)
      return 1;
   for (unsigned i = 0; i < N; i++)
      ins[i] = 0x1000000000ull + i;
   CHECK(F.q.ring_size == 4096, "ring size %u", F.q.ring_size);
   unsigned piece = kb_queue_piece_instrs(F.q.ring_size);
   CHECK(piece >= 1 && ((size_t)piece * 8 + 63) / 64 * 64 + 64 <= F.q.ring_size,
         "piece %u instrs (%zu B) leaves room in the %u-byte ring", piece, (size_t)piece * 8, F.q.ring_size);
   CHECK(kb_queue_piece_instrs(1u << 20) == (1u << 20) / 4 / 8, "piece scales with the ring (%u)", kb_queue_piece_instrs(1u << 20));
   CHECK(kb_queue_piece_instrs(1) == piece, "a too-small ring clamps to the 4 KiB piece (%u)", kb_queue_piece_instrs(1));

   const size_t mask = F.q.ring_size - 1;
   unsigned pieces = 0, wraps = 0, submitted = 0;
   for (unsigned i = 0; i < N && g_fail == 0;) {
      unsigned m = N - i < piece ? N - i : piece;
      /* vary only full pieces, so offsets are not ring/4 aligned and copies straddle the end; a partial tail is
       * submitted whole and must not shrink to zero */
      if (m == piece) m -= pieces % 13;
      uint64_t off = F.q.insert;
      if (kb_queue_submit(&F.q, ins + i, m, KB_KICK_IOCTL) != 0) {
         CHECK(false, "submit piece %u (%u instrs)", pieces, m);
         break;
      }
      submitted += m;
      size_t total = ((size_t)m * 8 + 63) & ~(size_t)63;
      for (unsigned k = 0; k < m; k++)
         if (*(const uint64_t *)((const char *)F.q.ring.cpu + ((off + k * 8) & mask)) != ins[i + k]) {
            CHECK(false, "piece %u word %u at ring offset %llu: got %llx want %llx (wrap)", pieces, k,
                  (unsigned long long)((off + k * 8) & mask),
                  (unsigned long long)*(const uint64_t *)((const char *)F.q.ring.cpu + ((off + k * 8) & mask)),
                  (unsigned long long)ins[i + k]);
            break;
         }
      if (((off + total - 1) & ~(uint64_t)mask) != (off & ~(uint64_t)mask))
         wraps++;
      pieces++;
      i += m;
   }
   CHECK(pieces >= 20, "stream longer than the ring was submitted in %u pieces", pieces);
   CHECK(submitted == N, "submitted %u of %u instructions (no gaps in the stream)", submitted, N);
   CHECK(wraps >= 3, "%u pieces straddled the end of the ring", wraps);
   /* total >= ring_size is refused by the queue, so callers must chunk (the old 2 KiB cap silently relied on this) */
   CHECK(kb_queue_submit(&F.q, ins, F.q.ring_size / 8, KB_KICK_IOCTL) == -EINVAL, "oversized block refused");
   printf("{\"test\":\"ringwrap\",\"piece\":%u,\"pieces\":%u,\"wraps\":%u,\"pass\":%s}\n", piece, pieces, wraps,
          g_fail ? "false" : "true");
   free(ins);
   return g_fail ? 1 : 0;
}

/* VK_EXT_provoking_vertex: the mode reaches both hardware copies of "First provoking vertex" (FBD word 11 bit 14, tiler ctx word 2 bit 18) */
static void
test_provoking(VkCommandPool pool, VkBuffer vbh)
{
   DFN(EndCommandBuffer);
   for (int last = 0; last < 2; last++) {
      VkCommandBuffer c;
      struct vk_command_buffer *vc = new_cmd(pool, &c);
      /* snapshot_draw reads the runtime state a bound pipeline (VkPipelineRasterizationProvokingVertexStateCreateInfoEXT) would have set */
      vc->dynamic_graphics_state.rs.provoking_vertex = last ? VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT : VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
      record_scene(c, &k_scenes[0], vbh);
      CHECK(EndCommandBuffer(c) == VK_SUCCESS, "provoking end");
      g_tctx = NULL; g_fbd = NULL;
      CHECK(mali_cmd_submit(vc) == VK_SUCCESS, "provoking submit");
      CHECK(g_tctx && g_fbd, "provoking: tiler ctx / FBD allocations not seen");
      if (!g_tctx || !g_fbd) return;
      uint32_t f11;
      memcpy(&f11, g_fbd + 44, 4);
      CHECK(((f11 >> 14) & 1) == (last ? 0u : 1u), "provoking %s: FBD word 11 = %08x bit 14 wrong", last ? "LAST" : "FIRST", f11);
      CHECK(((g_tctx[2] >> 18) & 1) == (last ? 0u : 1u), "provoking %s: tiler ctx word 2 = %08x bit 18 wrong", last ? "LAST" : "FIRST", g_tctx[2]);
   }
}


/* ---- --loadstore: VK_KHR_load_store_op_none and per-attachment load ops (colour, depth, stencil) ---- */
struct ls_case {
   const char *name;
   bool col, dep, st;      /* attachments present */
   VkAttachmentLoadOp lcol, ldep, lst;
   unsigned pre;           /* expected preload draws before the application draw */
   int dv;                 /* 0: depth view 2 (D32F), 4: D24X8 */
};

static unsigned g_prel_used;

static int
ls_upload(void *u, const uint8_t *bin, uint32_t n, uint64_t *va)
{
   struct fx *f = u;
   if (g_prel_used + n > (64u << 10))
      return -1;
   memcpy((uint8_t *)f->prel.cpu + g_prel_used, bin, n);
   *va = f->prel.gpu_va + g_prel_used;
   g_prel_used += (n + 15) & ~15u;
   return 0;
}

static void ls_meta_free(void *u, void *cpu, uint64_t va, size_t size) { (void)u; (void)cpu; (void)va; (void)size; }   /* arena memory */

/* Preload draws are the driver's meta draws: every rec before the application draw, whose fragment shader is not the bound test pipeline
 * (mali_meta_preload returns its own compiled FS; the app pipeline resolver returns g_fs). */
static unsigned
ls_preloads(const struct mali_frame *f)
{
   unsigned n = 0;
   while (n < f->ndraws && f->draws[n].pipe && f->draws[n].pipe->gfx.fs != &g_fs)
      n++;
   return n;
}

static void
test_msaa_zs_only(VkCommandPool pool)
{
   DFN(EndCommandBuffer);
   for (unsigned i = 0; i < 4; i++) {
      VkCommandBuffer cb;
      struct vk_command_buffer *vc = new_cmd(pool, &cb);
      bool depth = i != 1, stencil = i != 0;
      uintptr_t view = i == 0 ? 7 : i == 1 ? 6 : i == 2 ? 8 : 9;
      VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
         .imageView = (VkImageView)view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
         .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
      VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
         .renderArea = {{0, 0}, {32, 32}}, .layerCount = 1,
         .pDepthAttachment = depth ? &a : NULL, .pStencilAttachment = stencil ? &a : NULL};
      mali_CmdBeginRendering(cb, &ri);
      struct mali_cmd_buffer *cmd = mali_cmd_buffer_from_vk(vc);
      struct mali_frame *f = &cmd->cur;
      CHECK(cmd->in_render && f->samples == 4 && f->has_zs == depth && f->has_st == stencil,
            "4x depth/stencil-only case %u records", i);
      CHECK(f->ndraws == 1 && f->draws[0].push, "4x case %u preload", i);
      if (f->ndraws && f->draws[0].push) {
         uint32_t pc[5];
         memcpy(pc, f->draws[0].push->push[1], sizeof pc);
         CHECK(pc[3] == (depth ? (i == 0 ? 12288u : 12032u) : 0u), "4x case %u depth stride %u", i, pc[3]);
         CHECK(pc[4] == (stencil ? (i == 1 ? 3328u : 3072u) : 0u), "4x case %u stencil stride %u", i, pc[4]);
      }
      uint8_t fbd[KB_FB_DESC_SIZE_ZS];
      struct mali_env env = {.arch = ARCH, .tile_log2 = 10};
      CHECK(mali_render_pack_fbd(&env, f, fbd, 0, 0x300000) == 0, "4x case %u pack", i);
      uint32_t w[16];
      memcpy(w, fbd + KB_FB_FBD_SIZE, sizeof w);
      CHECK(!depth || (((w[0] >> 8) & 3) == 3 && w[11] == f->zs.surface_stride), "4x case %u depth writeback", i);
      CHECK(!stencil || (((w[0] >> 24) & 3) == 3 && w[15] == f->st.surface_stride), "4x case %u stencil writeback", i);
      mali_CmdEndRendering(cb);
      CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "4x case %u end", i);
   }
}

/* Records one pass with one triangle and checks the load-op outcome at record time (the frame draw list is the command stream's draws). */
static struct mali_frame *
ls_pass(VkCommandPool pool, const struct ls_case *c)
{
   DFN(CmdBindPipeline);
   DFN(CmdSetViewport);
   DFN(CmdSetScissor);
   DFN(CmdDraw);
   DFN(EndCommandBuffer);
   VkCommandBuffer cb;
   struct vk_command_buffer *vc = new_cmd(pool, &cb);
   VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = (VkImageView)(uintptr_t)1,
                                   .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = c->lcol,
                                   .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
   VkRenderingAttachmentInfo da = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                   .imageView = (VkImageView)(uintptr_t)(c->dv ? c->dv : c->st ? 3 : 2),
                                   .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, .loadOp = c->ldep,
                                   .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
   VkRenderingAttachmentInfo sa = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = (VkImageView)(uintptr_t)3,
                                   .imageLayout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL, .loadOp = c->lst,
                                   .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
   VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                         .renderArea = {{0, 0}, {W, H}},
                         .layerCount = 1,
                         .colorAttachmentCount = c->col ? 1 : 0,
                         .pColorAttachments = c->col ? &ca : NULL,
                         .pDepthAttachment = c->dep ? &da : NULL,
                         .pStencilAttachment = c->st ? &sa : NULL};
   mali_CmdBeginRendering(cb, &ri);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)(uintptr_t)1);
   VkViewport vp = {0, 0, W, H, 0.0f, 1.0f};
   VkRect2D scr = {{0, 0}, {W, H}};
   CmdSetViewport(cb, 0, 1, &vp);
   CmdSetScissor(cb, 0, 1, &scr);
   CmdDraw(cb, 3, 1, 0, 0);
   mali_CmdEndRendering(cb);
   VkResult r = EndCommandBuffer(cb);
   CHECK(r == VK_SUCCESS, "%s: EndCommandBuffer %d", c->name, r);
   struct mali_cmd_buffer *m = mali_cmd_buffer_from_vk(vc);
   CHECK(m->nframes == 1 && m->frames[0].ndraws == c->pre + 1, "%s: %u frames, %u draws (want %u)", c->name, m->nframes,
         m->nframes ? m->frames[0].ndraws : 0u, c->pre + 1);
   if (m->nframes != 1)
      return NULL;
   struct mali_frame *f = &m->frames[0];
   CHECK(ls_preloads(f) == c->pre, "%s: %u preload draws in the stream (want %u)", c->name, ls_preloads(f), c->pre);
   CHECK(f->has_colour == c->col && f->has_zs == c->dep && f->has_st == c->st, "%s: attached colour %u zs %u st %u", c->name,
         f->has_colour, f->has_zs, f->has_st);
   /* LOAD preserves: each preload draw reads the old plane (its push carries the attachment's row bytes: colour/depth W*4, S8 W) */
   for (unsigned i = 0; i < c->pre; i++) {
      const uint32_t *pc = (const uint32_t *)f->draws[i].push->push[1];
      CHECK(pc[0] == W * 4 || pc[0] == W, "%s: preload %u reads row bytes %u (want %u colour/depth or %u stencil)", c->name, i, pc[0], W * 4, W);
   }
   /* the frame still completes: build and submit through the fake backend */
   unsigned before = F.nsubmit;
   r = mali_cmd_submit(vc);
   CHECK(r == VK_SUCCESS && F.nsubmit == before + 1, "%s: submit %d frames +%u", c->name, r, F.nsubmit - before);
   return f;
}

/* One UI-composite frame: four full-screen layers, each its own pass, one draw each (the 4-layer shape of the perf model). Builds every
 * frame's command stream and reports its size, so the preload draws' cost is visible in the fake-backend stream. */
static void
ls_ui_frame(VkCommandPool pool, VkAttachmentLoadOp op, const char *tag)
{
   DFN(CmdBindPipeline);
   DFN(CmdSetViewport);
   DFN(CmdSetScissor);
   DFN(CmdDraw);
   DFN(EndCommandBuffer);
   VkCommandBuffer cb;
   struct vk_command_buffer *vc = new_cmd(pool, &cb);
   for (int p = 0; p < 4; p++) {
      VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = (VkImageView)(uintptr_t)1,
                                      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = op,
                                      .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
      VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {W, H}}, .layerCount = 1,
                            .colorAttachmentCount = 1, .pColorAttachments = &ca};
      mali_CmdBeginRendering(cb, &ri);
      CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)(uintptr_t)1);
      VkViewport vp = {0, 0, W, H, 0.0f, 1.0f};
      VkRect2D scr = {{0, 0}, {W, H}};
      CmdSetViewport(cb, 0, 1, &vp);
      CmdSetScissor(cb, 0, 1, &scr);
      CmdDraw(cb, 3, 1, 0, 0);
      mali_CmdEndRendering(cb);
   }
   VkResult r = EndCommandBuffer(cb);
   CHECK(r == VK_SUCCESS, "ui_like_%s: EndCommandBuffer %d", tag, r);
   struct mali_cmd_buffer *m = mali_cmd_buffer_from_vk(vc);
   CHECK(m->nframes == 4, "ui_like_%s: %u frames", tag, m->nframes);
   struct mali_hw hw = make_hw();
   if (hw.job_begin)
      hw.job_begin(hw.user);
   unsigned draws = 0, words = 0;
   for (unsigned i = 0; i < m->nframes; i++) {
      uint64_t *cs = NULL;
      unsigned n = 0;
      int rc = mali_frame_build(&hw, &m->frames[i], &cs, &n);
      CHECK(rc == 0, "ui_like_%s frame %u: build %d", tag, i, rc);
      draws += m->frames[i].ndraws;
      words += n;
      free(cs);
   }
   printf("{\"test\":\"t_cmd%d\",\"case\":\"ui_like_%s\",\"passes\":%u,\"draws\":%u,\"cs_words\":%u,\"cs_bytes\":%u}\n", ARCH, tag,
          m->nframes, draws, words, words * 8u);
   r = mali_cmd_submit(vc);
   CHECK(r == VK_SUCCESS, "ui_like_%s: submit %d", tag, r);
}

static void
test_descriptor_bind_points(VkCommandPool pool)
{
   VkCommandBuffer cb;
   struct mali_cmd_buffer *m = mali_cmd_buffer_from_vk(new_cmd(pool, &cb));
   struct mali_dsl layout = {.dyn_count = 1};
   struct mali_dyn_buf a_dyn = {.va = 0x100000, .size = 1024}, b_dyn = {.va = 0x200000, .size = 1024};
   struct mali_dset a = {.layout = &layout, .va = 0x300000, .bytes = 64, .dyn = &a_dyn, .arch = ARCH};
   struct mali_dset b = {.layout = &layout, .va = 0x400000, .bytes = 96, .dyn = &b_dyn, .arch = ARCH};
   a.base.type = b.base.type = VK_OBJECT_TYPE_DESCRIPTOR_SET;
   struct vk_pipeline_layout pl = {.base = {.type = VK_OBJECT_TYPE_PIPELINE_LAYOUT}, .set_count = 1};
   pl.set_layouts[0] = &layout.vk;
   VkPipelineLayout pipeline_layout = vk_pipeline_layout_to_handle(&pl);
   VkDescriptorSet as = mali_dset_to_handle(&a), bs = mali_dset_to_handle(&b);
   uint32_t ao = 64, bo = 128;
   struct mali_hw_desc ad, bd;
   mali_dyn_buf_pack(ARCH, &ad, &a_dyn, ao);
   mali_dyn_buf_pack(ARCH, &bd, &b_dyn, bo);
   mali_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &as, 1, &ao);
   mali_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &bs, 1, &bo);
   struct mali_compiled cs = {.bin_size = 4};
   m->cs = &cs;
   mali_CmdDispatch(cb, 1, 1, 1);
   VkRenderingAttachmentInfo ca;
   VkRenderingInfo ri = rendering_info(&ca, true);
   mali_CmdBeginRendering(cb, &ri);
   DFN(CmdBindPipeline);
   DFN(CmdDraw);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)(uintptr_t)1);
   CmdDraw(cb, 3, 1, 0, 0);
   mali_CmdEndRendering(cb);
   DFN(EndCommandBuffer);
   CHECK(EndCommandBuffer(cb) == VK_SUCCESS && m->nframes == 2 && m->frames[1].ndraws == 1,
         "bind points: expected dispatch and draw");
   if (m->nframes == 2 && m->frames[1].ndraws) {
      const struct mali_rec *draw = &m->frames[1].draws[0];
      CHECK(m->frames[0].disp.nsets == 1 && m->frames[0].disp.set_va[0] == b.va &&
            m->frames[0].disp.ndyn == 1 && !memcmp(m->frames[0].disp.dyn_desc[0], bd.w, sizeof bd.w),
            "compute did not retain set B and its dynamic offset");
      CHECK(draw->bind && draw->bind->nsets == 1 && draw->bind->set_va[0] == a.va &&
            draw->dyn && draw->dyn->n == 1 && !memcmp(draw->dyn->d[0], ad.w, sizeof ad.w),
            "graphics did not retain set A and its dynamic offset");
   }
}

/* ---- VK_KHR_push_descriptor (vkd3d-proton requires it at device creation) ----
 * Every push is a fresh transient set (draws recorded earlier keep theirs), seeded from the previous push of the same bind point,
 * set index and set layout; graphics and compute keep separate push state. The entry points are taken from the device dispatch
 * table, which is what an application reaches (the runtime forwards the 1.0 forms to the maintenance6 2KHR forms). */
static VkDescriptorSetLayout
push_dsl(VkDescriptorSetLayoutCreateFlags flags, uint32_t n, const VkDescriptorSetLayoutBinding *b)
{
   DFN(CreateDescriptorSetLayout);
   VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = flags,
                                         .bindingCount = n, .pBindings = b};
   VkDescriptorSetLayout l = VK_NULL_HANDLE;
   CHECK(CreateDescriptorSetLayout(dev, &ci, NULL, &l) == VK_SUCCESS, "push: create set layout");
   return l;
}

static bool
push_holds_buffer(const struct mali_dset *s, unsigned slot, uint64_t va, uint32_t size)
{
   struct mali_hw_desc hd;
   mali_pack_buffer(s->arch, &hd, va, size);
   return s->cpu && (slot + 1) * MALI_DESC_SLOT_SIZE <= s->bytes && !memcmp(s->cpu + slot * MALI_DESC_SLOT_SIZE, hd.w, sizeof hd.w);
}

static void *fail_alloc(struct vk_device *d, size_t size, uint64_t *va) { (void)d; (void)size; (void)va; return NULL; }
static void fail_free(struct vk_device *d, void *p, size_t size) { (void)d; (void)p; (void)size; }
static const struct mali_desc_mem_ops k_fail_mem = {fail_alloc, fail_free};

static void
test_push_descriptors(VkCommandPool pool)
{
   struct mali_device *md = mali_device_from_handle(dev);
   const struct vk_device_dispatch_table *dt = &md->vk.dispatch_table;
   /* dispatch_table holds the secondary-recording wrappers (always set); cmd_dispatch is what a primary reaches */
   const struct vk_device_dispatch_table *pt = &md->cmd_dispatch;
   CHECK(pt->CmdPushDescriptorSet2KHR && pt->CmdPushDescriptorSetWithTemplate2KHR && pt->CmdPushDescriptorSetKHR &&
         pt->CmdPushDescriptorSetWithTemplateKHR, "push: vkCmdPushDescriptorSet* not implemented");
   if (!pt->CmdPushDescriptorSet2KHR || !pt->CmdPushDescriptorSetWithTemplate2KHR)
      return;
   DFN(CmdBindPipeline);
   DFN(CmdDraw);
   DFN(EndCommandBuffer);
   DFN(ResetCommandBuffer);
   PFN_vkCreateDescriptorUpdateTemplate CreateDescriptorUpdateTemplate = dt->CreateDescriptorUpdateTemplate;   /* 1.1 core: the test device is 1.0 */
   const VkDescriptorSetLayoutCreateFlags PUSH = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
   const VkDescriptorSetLayoutBinding ubo2[2] = {
      {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL},
      {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL}};
   VkDescriptorSetLayout l2 = push_dsl(PUSH, 2, ubo2), l1 = push_dsl(PUSH, 1, ubo2), l0 = push_dsl(PUSH, 0, NULL);
   VkDescriptorSetLayout lplain = push_dsl(0, 1, ubo2);
   struct vk_pipeline_layout pl2 = {.base = {.type = VK_OBJECT_TYPE_PIPELINE_LAYOUT}, .set_count = 1},
                             pl1 = pl2, pl0 = pl2, plp = pl2;
   pl2.set_layouts[0] = vk_descriptor_set_layout_from_handle(l2);
   pl1.set_layouts[0] = vk_descriptor_set_layout_from_handle(l1);
   pl0.set_layouts[0] = vk_descriptor_set_layout_from_handle(l0);
   plp.set_layouts[0] = vk_descriptor_set_layout_from_handle(lplain);
   VkPipelineLayout hpl2 = vk_pipeline_layout_to_handle(&pl2), hpl1 = vk_pipeline_layout_to_handle(&pl1);
   VkPipelineLayout hpl0 = vk_pipeline_layout_to_handle(&pl0), hplp = vk_pipeline_layout_to_handle(&plp);
   VkBuffer ba = vk_buffer_to_handle(make_buffer(0x1000000, 4096)), bb = vk_buffer_to_handle(make_buffer(0x2000000, 4096));
   VkBuffer bc = vk_buffer_to_handle(make_buffer(0x3000000, 4096));
   VkDescriptorBufferInfo ia = {ba, 0, 256}, ib = {bb, 256, 512}, ic = {bc, 0, 1024};
   VkWriteDescriptorSet w0 = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &ia};
   VkWriteDescriptorSet w1 = w0, wab[2];
   w1.dstBinding = 1;
   w1.pBufferInfo = &ic;
   struct mali_compiled cs = {.bin_size = 4};
   VkRenderingAttachmentInfo ca;
   VkRenderingInfo ri = rendering_info(&ca, true);
   VkPushDescriptorSetInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_INFO_KHR, .layout = hpl2, .set = 0};

   /* 1. graphics A and compute B at the same set index: the dispatch sees B, the draw sees A */
   {
      VkCommandBuffer cb;
      struct mali_cmd_buffer *m = mali_cmd_buffer_from_vk(new_cmd(pool, &cb));
      dt->CmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hpl2, 0, 1, &w0);
      const struct mali_dset *ga = m->push_sets[0][0];
      w0.pBufferInfo = &ib;
      dt->CmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hpl2, 0, 1, &w0);
      w0.pBufferInfo = &ia;
      const struct mali_dset *cbs = m->push_sets[1][0];
      m->cs = &cs;
      mali_CmdDispatch(cb, 1, 1, 1);
      mali_CmdBeginRendering(cb, &ri);
      CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)(uintptr_t)1);
      CmdDraw(cb, 3, 1, 0, 0);
      mali_CmdEndRendering(cb);
      CHECK(EndCommandBuffer(cb) == VK_SUCCESS && m->nframes == 2 && m->frames[1].ndraws == 1, "push bind points: dispatch and draw");
      CHECK(ga && cbs && ga != cbs && ga->va != cbs->va, "push bind points: one transient set per bind point");
      if (ga && cbs && m->nframes == 2 && m->frames[1].ndraws) {
         const struct mali_rec *d = &m->frames[1].draws[0];
         CHECK(m->frames[0].disp.nsets == 1 && m->frames[0].disp.set_va[0] == cbs->va && push_holds_buffer(cbs, 0, 0x2000000 + 256, 512),
               "push bind points: compute did not see its own set B");
         CHECK(d->bind && d->bind->nsets == 1 && d->bind->set_va[0] == ga->va && push_holds_buffer(ga, 0, 0x1000000, 256),
               "push bind points: graphics did not see its own set A");
      }
   }
   /* 2. push, draw, re-push (binding 0 only), draw: draw 1 keeps its snapshot, draw 2 inherits binding 1 from the first push */
   {
      VkCommandBuffer cb;
      struct mali_cmd_buffer *m = mali_cmd_buffer_from_vk(new_cmd(pool, &cb));
      wab[0] = w0;
      wab[1] = w1;
      mali_CmdBeginRendering(cb, &ri);
      CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)(uintptr_t)1);
      dt->CmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hpl2, 0, 2, wab);
      const struct mali_dset *s1 = m->push_sets[0][0];
      CmdDraw(cb, 3, 1, 0, 0);
      w0.pBufferInfo = &ib;
      dt->CmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hpl2, 0, 1, &w0);
      w0.pBufferInfo = &ia;
      const struct mali_dset *s2 = m->push_sets[0][0];
      CmdDraw(cb, 3, 1, 0, 0);
      mali_CmdEndRendering(cb);
      CHECK(EndCommandBuffer(cb) == VK_SUCCESS && m->nframes == 1 && m->frames[0].ndraws == 2, "push re-push: two draws");
      CHECK(s1 && s2 && s1 != s2 && s1->va != s2->va, "push re-push: the second push must be a new set");
      if (s1 && s2 && m->nframes == 1 && m->frames[0].ndraws == 2) {
         const struct mali_rec *d = m->frames[0].draws;
         CHECK(d[0].bind && d[0].bind->set_va[0] == s1->va && push_holds_buffer(s1, 0, 0x1000000, 256) &&
               push_holds_buffer(s1, 1, 0x3000000, 1024), "push re-push: draw 1 lost its snapshot");
         const struct mali_bbind *b2 = d[1].bind ? d[1].bind : d[0].bind;
         CHECK(b2 && b2->set_va[0] == s2->va && push_holds_buffer(s2, 0, 0x2000000 + 256, 512) && push_holds_buffer(s2, 1, 0x3000000, 1024),
               "push re-push: draw 2 must see B and the inherited binding 1");
      }
   }
   /* 3. templates: the bind point comes from the template (1.0 and 2KHR forms) */
   {
      VkDescriptorUpdateTemplateEntry te = {.dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                            .offset = 0, .stride = sizeof(VkDescriptorBufferInfo)};
      VkDescriptorUpdateTemplateCreateInfo tci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO, .descriptorUpdateEntryCount = 1,
                                                  .pDescriptorUpdateEntries = &te, .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR,
                                                  .pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE, .pipelineLayout = hpl2, .set = 0};
      VkDescriptorUpdateTemplate t = VK_NULL_HANDLE;
      CHECK(CreateDescriptorUpdateTemplate(dev, &tci, NULL, &t) == VK_SUCCESS, "push template: create");
      VkCommandBuffer cb;
      struct mali_cmd_buffer *m = mali_cmd_buffer_from_vk(new_cmd(pool, &cb));
      dt->CmdPushDescriptorSetWithTemplateKHR(cb, t, hpl2, 0, &ib);
      const struct mali_dset *t1 = m->push_sets[1][0];
      CHECK(t1 && !m->push_sets[0][0] && push_holds_buffer(t1, 0, 0x2000000 + 256, 512), "push template: compute set from the template's bind point");
      VkPushDescriptorSetWithTemplateInfoKHR ti = {.sType = VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_WITH_TEMPLATE_INFO_KHR,
                                                   .descriptorUpdateTemplate = t, .layout = hpl2, .set = 0, .pData = &ic};
      dt->CmdPushDescriptorSetWithTemplate2KHR(cb, &ti);
      const struct mali_dset *t2 = m->push_sets[1][0];
      CHECK(t2 && t2 != t1 && !m->push_sets[0][0] && push_holds_buffer(t2, 0, 0x3000000, 1024), "push template 2KHR: compute set");
      CHECK(t1 && push_holds_buffer(t1, 0, 0x2000000 + 256, 512), "push template: the first set was overwritten");
      CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "push template: end");
   }
   /* 4. maintenance6 2KHR: stageFlags route to the bind points; a null layout takes VkPipelineLayoutCreateInfo from pNext */
   {
      VkCommandBuffer cb;
      struct mali_cmd_buffer *m = mali_cmd_buffer_from_vk(new_cmd(pool, &cb));
      pi.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
      pi.descriptorWriteCount = 1;
      pi.pDescriptorWrites = &w0;
      dt->CmdPushDescriptorSet2KHR(cb, &pi);
      CHECK(m->push_sets[0][0] && !m->push_sets[1][0], "push 2KHR: VERTEX must reach graphics only");
      const struct mali_dset *g = m->push_sets[0][0];
      pi.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      pi.pDescriptorWrites = &w1;
      dt->CmdPushDescriptorSet2KHR(cb, &pi);
      CHECK(m->push_sets[0][0] && m->push_sets[1][0] && m->push_sets[0][0] != g && m->push_sets[0][0] != m->push_sets[1][0],
            "push 2KHR: COMPUTE|FRAGMENT must reach both bind points");
      if (m->push_sets[0][0] && m->push_sets[1][0])
         CHECK(push_holds_buffer(m->push_sets[0][0], 0, 0x1000000, 256) && push_holds_buffer(m->push_sets[0][0], 1, 0x3000000, 1024) &&
               !push_holds_buffer(m->push_sets[1][0], 0, 0x1000000, 256) && push_holds_buffer(m->push_sets[1][0], 1, 0x3000000, 1024),
               "push 2KHR: graphics inherits its own binding 0, compute starts fresh");
      VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &l1};
      VkPushDescriptorSetInfoKHR pn = {.sType = VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_INFO_KHR, .pNext = &plci,
                                       .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .layout = VK_NULL_HANDLE, .set = 0,
                                       .descriptorWriteCount = 1, .pDescriptorWrites = &w0};
      dt->CmdPushDescriptorSet2KHR(cb, &pn);
      CHECK(m->push_sets[1][0] && m->push_sets[1][0]->bytes == MALI_DESC_SLOT_SIZE && push_holds_buffer(m->push_sets[1][0], 0, 0x1000000, 256),
            "push 2KHR: null layout with a pNext pipeline layout");
      CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "push 2KHR: end");
      new_cmd(pool, &cb);
      pn.pNext = NULL;
      dt->CmdPushDescriptorSet2KHR(cb, &pn);
      CHECK(EndCommandBuffer(cb) != VK_SUCCESS, "push 2KHR: a null layout without pNext must be refused");
   }
   /* 5. empty layout: nothing to allocate, the set index is still bound */
   {
      VkCommandBuffer cb;
      struct mali_cmd_buffer *m = mali_cmd_buffer_from_vk(new_cmd(pool, &cb));
      mali_CmdBeginRendering(cb, &ri);
      CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)(uintptr_t)1);
      dt->CmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hpl0, 0, 0, NULL);
      CmdDraw(cb, 3, 1, 0, 0);
      mali_CmdEndRendering(cb);
      CHECK(EndCommandBuffer(cb) == VK_SUCCESS && m->nframes == 1 && m->frames[0].ndraws == 1 && m->push_sets[0][0] &&
            m->push_sets[0][0]->bytes == 0, "push empty layout");
      if (m->nframes == 1 && m->frames[0].ndraws == 1)
         CHECK(m->frames[0].draws[0].bind && m->frames[0].draws[0].bind->nsets == 1 && m->frames[0].draws[0].bind->set_bytes[0] == 0,
               "push empty layout: set 0 bound with no bytes");
   }
   /* 6. layout changes and ordinary binds: a push with another set layout starts fresh (sized for it), an ordinary bind replaces it */
   {
      VkCommandBuffer cb;
      struct mali_cmd_buffer *m = mali_cmd_buffer_from_vk(new_cmd(pool, &cb));
      wab[0] = w0;
      wab[1] = w1;
      dt->CmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hpl2, 0, 2, wab);
      w0.pBufferInfo = &ib;
      dt->CmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hpl1, 0, 1, &w0);
      w0.pBufferInfo = &ia;
      const struct mali_dset *s = m->push_sets[0][0];
      CHECK(s && s->bytes == MALI_DESC_SLOT_SIZE && push_holds_buffer(s, 0, 0x2000000 + 256, 512), "push layout change: fresh set of the new layout");
      struct mali_dsl dl = {0};
      struct mali_dset plain = {.layout = &dl, .va = 0x5000000, .bytes = 64, .arch = ARCH};
      plain.base.type = VK_OBJECT_TYPE_DESCRIPTOR_SET;
      VkDescriptorSet ph = mali_dset_to_handle(&plain);
      mali_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hpl1, 0, 1, &ph, 0, NULL);
      CHECK(!m->push_sets[0][0] && m->descriptors[0].set_va[0] == 0x5000000, "push then ordinary bind: the bound set replaces the push");
      dt->CmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hpl2, 0, 1, &w1);
      CHECK(m->push_sets[0][0] && !push_holds_buffer(m->push_sets[0][0], 0, 0x1000000, 256), "push after an ordinary bind starts fresh");
      CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "push layout change: end");
   }
   /* 7. incompatible layouts are refused loudly: a set layout without the push flag, a set index outside the pipeline layout */
   {
      VkCommandBuffer cb;
      struct mali_cmd_buffer *m = mali_cmd_buffer_from_vk(new_cmd(pool, &cb));
      dt->CmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hplp, 0, 1, &w0);
      CHECK(!m->push_sets[0][0] && m->descriptors[0].nsets == 0 && EndCommandBuffer(cb) != VK_SUCCESS,
            "push: a set layout without PUSH_DESCRIPTOR must be refused");
      m = mali_cmd_buffer_from_vk(new_cmd(pool, &cb));
      dt->CmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hpl2, 1, 1, &w0);
      CHECK(m->descriptors[0].nsets == 0 && EndCommandBuffer(cb) != VK_SUCCESS, "push: a set index outside the layout must be refused");
   }
   /* 8. lifetime: reset frees the transient sets and reuses the arena; a failed allocation is reported and binds nothing */
   {
      VkCommandBuffer cb;
      struct mali_cmd_buffer *m = mali_cmd_buffer_from_vk(new_cmd(pool, &cb));
      for (int i = 0; i < 300; i++)
         dt->CmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hpl2, 0, 1, &w0);
      CHECK(EndCommandBuffer(cb) == VK_SUCCESS && m->push_arena.nsets == 300, "push lifetime: 300 sets recorded");
      unsigned chunks = m->push_arena.nchunks;
      CHECK(ResetCommandBuffer(cb, 0) == VK_SUCCESS && m->push_arena.nsets == 0 && !m->push_sets[1][0], "push lifetime: reset frees the sets");
      DFN(BeginCommandBuffer);
      VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      BeginCommandBuffer(cb, &bi);
      for (int i = 0; i < 300; i++)
         dt->CmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hpl2, 0, 1, &w0);
      CHECK(EndCommandBuffer(cb) == VK_SUCCESS && m->push_arena.nchunks == chunks && chunks >= 1, "push lifetime: the arena is reused (%u chunks)", chunks);
      m = mali_cmd_buffer_from_vk(new_cmd(pool, &cb));
      mali_desc_set_mem_ops(&k_fail_mem);
      dt->CmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hpl2, 0, 1, &w0);
      mali_resources_init();
      CHECK(!m->push_sets[0][0] && m->descriptors[0].nsets == 0, "push alloc failure: nothing bound");
      CHECK(EndCommandBuffer(cb) == VK_ERROR_OUT_OF_DEVICE_MEMORY, "push alloc failure: VK_ERROR_OUT_OF_DEVICE_MEMORY");
   }
   /* 9. maxPushDescriptors = 32: a full layout of combined image samplers with immutable samplers, and 32 uniform buffers */
   {
      static struct mali_sampler smp;
      smp.vk.base.type = VK_OBJECT_TYPE_SAMPLER;
      for (unsigned i = 0; i < 8; i++)
         smp.hw.w[i] = 0x5a000000u | i;
      VkSampler sh = mali_sampler_to_handle(&smp), imm[32];
      for (unsigned i = 0; i < 32; i++)
         imm[i] = sh;
      VkDescriptorSetLayoutBinding cis = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 32,
                                          .stageFlags = VK_SHADER_STAGE_ALL, .pImmutableSamplers = imm};
      VkDescriptorSetLayoutBinding ubos = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 32,
                                           .stageFlags = VK_SHADER_STAGE_ALL};
      VkDescriptorSetLayout lc = push_dsl(PUSH, 1, &cis), lu = push_dsl(PUSH, 1, &ubos);
      struct vk_pipeline_layout plc = pl2, plu = pl2;
      plc.set_layouts[0] = vk_descriptor_set_layout_from_handle(lc);
      plu.set_layouts[0] = vk_descriptor_set_layout_from_handle(lu);
      VkDescriptorImageInfo ii[32] = {{0}};
      VkDescriptorBufferInfo bi32[32];
      for (unsigned i = 0; i < 32; i++)
         bi32[i] = (VkDescriptorBufferInfo){ba, 64 * i, 64};
      VkWriteDescriptorSet wc = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 32,
                                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = ii};
      VkWriteDescriptorSet wu = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 32,
                                 .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = bi32};
      VkCommandBuffer cb;
      struct mali_cmd_buffer *m = mali_cmd_buffer_from_vk(new_cmd(pool, &cb));
      dt->CmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_layout_to_handle(&plc), 0, 1, &wc);
      dt->CmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, vk_pipeline_layout_to_handle(&plu), 0, 1, &wu);
      const struct mali_dset *c = m->push_sets[0][0], *u = m->push_sets[1][0];
      unsigned bad_c = 0, bad_u = 0;
      const struct mali_dsl *dl = c ? c->layout : NULL;
      for (unsigned i = 0; c && dl && i < 32; i++) {
         const uint8_t *e = c->cpu + (size_t)(dl->b[0].slot + i * dl->b[0].stride) * MALI_DESC_SLOT_SIZE;
         static const uint8_t zero[MALI_DESC_SLOT_SIZE];
         if (memcmp(e, zero, sizeof zero) || memcmp(e + MALI_DESC_SLOT_SIZE, smp.hw.w, sizeof smp.hw.w))
            bad_c++;
      }
      for (unsigned i = 0; u && i < 32; i++)
         if (!push_holds_buffer(u, i, 0x1000000 + 64 * i, 64))
            bad_u++;
      CHECK(c && u && !bad_c && !bad_u, "push 32 descriptors: %u combined/immutable and %u uniform elements wrong", bad_c, bad_u);
      CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "push 32 descriptors: end");
   }
}

int main(int argc, char **argv)
{
   int scene = 1;
   bool extra = false, m13 = false, loadstore = false, ringwrap = false;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--scene") && i + 1 < argc) scene = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--extra")) extra = true;
      else if (!strcmp(argv[i], "--m13")) m13 = true;
      else if (!strcmp(argv[i], "--loadstore")) loadstore = true;
      else if (!strcmp(argv[i], "--ringwrap")) ringwrap = true;
   }
   setenv("MALI_BACKEND", "fake", 1);
   setenv("MALI_FAKE_GPU", ARCH == 13 ? "v13" : ARCH == 10 ? "v10" : "v11", 1);
   test_clear_words();
   test_poly_assemble();
   test_store_op();
   test_stencil_store_stride_baseline();
   test_resolve_region();
   test_resolve_view_formats();
   fill_compiled();
   if (setup_kb())
      return 1;
   if (ringwrap) {
      test_ringwrap();
      return g_fail ? 1 : 0;
   }

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
   if (extra) {
      test_descriptor_bind_points(pool);
      test_push_descriptors(pool);
      test_resolve_mismatched_view(pool);
      test_stencil_store_recording(pool);
   }
   if (extra) {
      /* Exercise real refusal sites with debugging disabled, including a
       * different reason and an identical refusal in a second buffer. */
      DFN(CmdDraw);
      DFN(CmdDrawIndexed);
      VkCommandBuffer a, b;
      new_cmd(pool, &a);
      new_cmd(pool, &b);
      const char *dbg = getenv("MALI_PIPE_DBG");
      char *saved_dbg = dbg ? strdup(dbg) : NULL;
      unsetenv("MALI_PIPE_DBG");
      FILE *log = tmpfile();
      int saved_stderr = dup(STDERR_FILENO);
      if (!log || saved_stderr < 0 || dup2(fileno(log), STDERR_FILENO) < 0) {
         fprintf(stdout, "{\"error\":\"cannot capture stderr\"}\n");
         exit(1);
      }
      CmdDraw(a, 3, 1, 0, 0);
      CmdDraw(b, 3, 1, 0, 0);
      CmdDrawIndexed(b, 3, 1, 0, 0, 0);
      fflush(stderr);
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stderr);
      rewind(log);
      char got[1024] = {0};
      size_t n = fread(got, 1, sizeof(got) - 1, log);
      fclose(log);
      if (saved_dbg) {
         setenv("MALI_PIPE_DBG", saved_dbg, 1);
         free(saved_dbg);
      }
      const char *want =
         "mali_cmd: unsupported draw outside a rendering with a pipeline\n"
         "mali_cmd: unsupported indexed draw outside a rendering, without a pipeline or index buffer\n";
      CHECK(n == strlen(want) && !strcmp(got, want), "refusal stderr mismatch");
      CHECK(EndCommandBuffer(a) == VK_ERROR_FEATURE_NOT_PRESENT, "first refusal not sticky");
      CHECK(EndCommandBuffer(b) == VK_ERROR_FEATURE_NOT_PRESENT, "second refusal not sticky");
   }
   struct vk_buffer *vb = make_buffer(F.vb.gpu_va, sizeof k_pos);
   VkBuffer vbh = vk_buffer_to_handle(vb);

   VkCommandBuffer cb;
   VkResult r = VK_SUCCESS;
   CHECK(scene >= 1 && scene <= 2, "bad scene");
   if (!m13 && !loadstore) {   /* --m13/--loadstore dump only their own frames: the decoder's REQ_RESOURCE rule is per dump, not per frame */
      struct vk_command_buffer *vcmd = new_cmd(pool, &cb);
      record_scene(cb, &k_scenes[scene - 1], vbh);
      r = EndCommandBuffer(cb);
      CHECK(r == VK_SUCCESS, "EndCommandBuffer %d", r);
      r = mali_cmd_submit(vcmd);
      CHECK(r == VK_SUCCESS, "mali_cmd_submit %d", r);
      CHECK(F.nsubmit == 1, "frames submitted %u", F.nsubmit);
      printf("{\"test\":\"t_cmd%d\",\"scene\":%d,\"frames\":%u,\"pass\":%s}\n", ARCH, scene, F.nsubmit, g_fail ? "false" : "true");
   }
   if (extra) {
      uint32_t n0 = F.nsubmit;
      test_provoking(pool, vbh);
      (void)n0;
   }

   {   /* frame pool recycling: finished 64 KiB chunks are parked and reused, and a recycled (dirty) chunk still hands out zeroed blocks */
      struct mali_frame pf;
      memset(&pf, 0, sizeof pf);
      unsigned c0 = mali_frame_pool_cached();
      uint8_t *a = mali_frame_blk(&pf, 4096);
      CHECK(a != NULL, "pool blk");
      memset(a, 0xAB, 4096);
      mali_frame_finish(&pf);
      CHECK(mali_frame_pool_cached() == c0 + 1, "pool chunk not recycled: %u -> %u", c0, mali_frame_pool_cached());
      uint8_t *b = mali_frame_blk(&pf, 4096);
      CHECK(b == a && mali_frame_pool_cached() == c0, "pool chunk not reused (%p vs %p)", (void *)b, (void *)a);
      int nz = 0;
      for (int i = 0; b && i < 4096; i++)
         nz |= b[i];
      CHECK(!nz, "recycled block not zeroed");
      mali_frame_finish(&pf);
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
      /* M15: vkCmdFillBuffer / vkCmdUpdateBuffer write their destination on the GPU, so the submission must mark
       * the BO written: the device->CPU fence wait CSYNCs only marked BOs and the app maps stale bytes otherwise. */
      {
         VkCommandBuffer cf;
         struct vk_command_buffer *vcf = new_cmd(pool, &cf);
         struct vk_buffer *fb = make_buffer(F.vb.gpu_va, 4096);
         VkBuffer fbh = vk_buffer_to_handle(fb);
         uint32_t word = 0x11223344u;
         g_fake_buf_mem = (struct mali_memory *)(uintptr_t)0x2b2b;
         mali_CmdFillBuffer(cf, fbh, 0, 4096, 0);
         mali_CmdUpdateBuffer(cf, fbh, 0, sizeof word, &word);
         struct mali_cmd_buffer *cfm = mali_cmd_buffer_from_vk(vcf);
         CHECK(cfm->nwrote == 1 && cfm->wrote[0] == g_fake_buf_mem,
               "fill/update not marked GPU-written (nwrote %u, want 1 at the buffer's memory)", cfm->nwrote);
         g_fake_buf_mem = NULL;
      }
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

      /* VK_KHR_load_store_op_none: LOAD_OP_NONE discards the old contents, so it must not take the LOAD path (no preload draw) and is
       * accepted where DONT_CARE is; the full per-attachment matrix is --loadstore */
      new_cmd(pool, &c2);
      {
         VkRenderingAttachmentInfo ca;
         VkRenderingInfo ri = rendering_info(&ca, false);
         ca.loadOp = VK_ATTACHMENT_LOAD_OP_NONE_KHR;
         mali_CmdBeginRendering(c2, &ri);
         mali_CmdEndRendering(c2);
      }
      r = EndCommandBuffer(c2);
      CHECK(r == VK_SUCCESS, "loadOp NONE must not preload, end=%d", r);
      {
         struct mali_cmd_buffer *vnn = mali_cmd_buffer_from_vk(vk_command_buffer_from_handle(c2));
         CHECK(vnn->nframes == 1 && vnn->frames[0].ndraws == 0, "loadOp NONE recorded %u frames / %u preload draws", vnn->nframes,
               vnn->nframes ? vnn->frames[0].ndraws : 0u);
      }
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

      /* logicOpEnable is command-buffer state, not a draw property: a depth-only pass can retain it from an
       * earlier colour pipeline. It cannot change any output there, so the draw must stay valid. */
      {
         struct vk_command_buffer *vz = new_cmd(pool, &c2);
         vz->dynamic_graphics_state.cb.logic_op_enable = true;
         VkRenderingAttachmentInfo da = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                         .imageView = (VkImageView)(uintptr_t)2,
                                         .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
         da.clearValue.depthStencil.depth = 1.0f;
         VkRenderingInfo zr = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {W, H}}, .layerCount = 1,
                               .colorAttachmentCount = 0, .pDepthAttachment = &da};
         mali_CmdBeginRendering(c2, &zr);
         CmdBindPipeline(c2, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)(uintptr_t)1);
         VkDeviceSize zoff = 0;
         CmdBindVertexBuffers(c2, 0, 1, &vbh, &zoff);
         CmdDraw(c2, 3, 1, 0, 0);
         mali_CmdEndRendering(c2);
         r = EndCommandBuffer(c2);
         CHECK(r == VK_SUCCESS, "depth-only retained logicOp end=%d", r);
      }
      printf("{\"test\":\"t_cmd%d\",\"extra\":true,\"pass\":%s}\n", ARCH, g_fail ? "false" : "true");
   }
   if (loadstore) {
      struct mali_meta_dev seam = {.user = &F, .arch = ARCH, .alloc = hw_alloc, .free = ls_meta_free, .upload_shader = ls_upload};
      mali_meta_set_dev(&seam);
      test_msaa_zs_only(pool);
      static const struct ls_case cases[] = {
         {.name = "colour_none", .col = true, .lcol = VK_ATTACHMENT_LOAD_OP_NONE_KHR, .pre = 0},
         {.name = "colour_dont_care", .col = true, .lcol = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .pre = 0},
         {.name = "colour_load", .col = true, .lcol = VK_ATTACHMENT_LOAD_OP_LOAD, .pre = 1},
         {.name = "colour_clear", .col = true, .lcol = VK_ATTACHMENT_LOAD_OP_CLEAR, .pre = 0},
         {.name = "mixed_colour_load_depth_none", .col = true, .dep = true, .lcol = VK_ATTACHMENT_LOAD_OP_LOAD,
          .ldep = VK_ATTACHMENT_LOAD_OP_NONE_KHR, .pre = 1},
         {.name = "mixed_colour_none_depth_load", .col = true, .dep = true, .lcol = VK_ATTACHMENT_LOAD_OP_NONE_KHR,
          .ldep = VK_ATTACHMENT_LOAD_OP_LOAD, .pre = 1},
         {.name = "mixed_colour_none_depth_load_stencil_none", .col = true, .dep = true, .st = true,
          .lcol = VK_ATTACHMENT_LOAD_OP_NONE_KHR, .ldep = VK_ATTACHMENT_LOAD_OP_LOAD, .lst = VK_ATTACHMENT_LOAD_OP_NONE_KHR, .pre = 1},
         {.name = "mixed_colour_none_depth_none_stencil_load", .col = true, .dep = true, .st = true,
          .lcol = VK_ATTACHMENT_LOAD_OP_NONE_KHR, .ldep = VK_ATTACHMENT_LOAD_OP_NONE_KHR, .lst = VK_ATTACHMENT_LOAD_OP_LOAD, .pre = 1},
         {.name = "mixed_colour_none_depth_stencil_load", .col = true, .dep = true, .st = true,
          .lcol = VK_ATTACHMENT_LOAD_OP_NONE_KHR, .ldep = VK_ATTACHMENT_LOAD_OP_LOAD, .lst = VK_ATTACHMENT_LOAD_OP_LOAD, .pre = 1},
         {.name = "depth_none", .dep = true, .ldep = VK_ATTACHMENT_LOAD_OP_NONE_KHR, .pre = 0},
         {.name = "depth_d24_none", .col = true, .dep = true, .lcol = VK_ATTACHMENT_LOAD_OP_NONE_KHR,
          .ldep = VK_ATTACHMENT_LOAD_OP_NONE_KHR, .pre = 0, .dv = 4},
      };
      for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++)
         ls_pass(pool, &cases[i]);
      ls_ui_frame(pool, VK_ATTACHMENT_LOAD_OP_NONE_KHR, "none");
      ls_ui_frame(pool, VK_ATTACHMENT_LOAD_OP_LOAD, "load");
      mali_meta_set_dev(NULL);
      printf("{\"test\":\"t_cmd%d\",\"loadstore\":true,\"pass\":%s}\n", ARCH, g_fail ? "false" : "true");
   }
   DestroyCommandPool(dev, pool, NULL);
   kb_queue_destroy(&F.q);
   return g_fail ? 1 : 0;
}
