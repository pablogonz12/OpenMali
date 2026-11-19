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
#include "kbase_fb.h"
#include "kbase_uapi.h"
#include "kbase_draw.h"
#include "kbase_fb.h"
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
/* per-attachment storeOp in the packed FBD: a discarded colour attachment must clear its own Write Enable (word 1 bit 0) and
 * Clean Tile Write (word 1 bit 31), and must not touch the neighbours' bits. Host-level, no backend: fails if the packer ignores
 * rt_extra[].no_store (the fake backend's render is a no-op, so the pixel test cannot see this). */
static void
test_fbd_rt_store(void)
{
   struct mali_env env;
   struct mali_frame f;
   uint8_t fbd[KB_FB_DESC_SIZE_MAX];
   memset(&env, 0, sizeof env);
   memset(&f, 0, sizeof f);
   memset(fbd, 0, sizeof fbd);
   env.arch = ARCH;
   env.tile_log2 = 10;
   f.width = 64;
   f.height = 32;
   f.has_colour = true;
   f.samples = 1;
   f.ncolour = 3;
   f.colour.va = 0x1000;
   f.colour.row_stride = 64 * 4;
   f.colour.store = true;
   f.colour.clear = true;
   f.colour.clear_color[0] = 1.0f;
   f.colour_x[0].va = 0x2000;          /* RT1: DONT_CARE store, blended + discarded */
   f.colour_x[0].row_stride = 64 * 4;
   f.colour_x[0].store = false;
   f.colour_x[0].clear = true;
   f.colour_x[0].clear_color[2] = 1.0f;
   f.colour_x[1].va = 0x3000;          /* RT2: stored */
   f.colour_x[1].row_stride = 64 * 4;
   f.colour_x[1].store = true;
   f.colour_x[1].clear = true;
   f.colour_x[1].clear_color[1] = 1.0f;
   int rc = mali_render_pack_fbd(&env, &f, fbd, 0x5000, 0x6000);
   CHECK(rc == 0, "pack_fbd rc %d", rc);
   if (rc)
      return;
   const uint32_t *w = (const uint32_t *)(const void *)fbd;
   const uint32_t *r = w + KB_FB_FBD_SIZE / 4;   /* no ZS/CRC extension: descriptors right after the FBD */
   for (unsigned n = 0; n < 3; n++) {
      const uint32_t *q = r + 16u * n;
      const bool want_store = n != 1;
      CHECK(((q[1] >> 0) & 1u) == (want_store ? 1u : 0u), "RT%u Write Enable = %u, want %u", n, (q[1] >> 0) & 1u, want_store ? 1u : 0u);
      CHECK(((q[1] >> 31) & 1u) == (want_store ? 1u : 0u), "RT%u Clean Tile Write = %u, want %u", n, (q[1] >> 31) & 1u, want_store ? 1u : 0u);
      CHECK((q[8] == (uint32_t)(n == 0 ? 0x1000u : n == 1 ? 0x2000u : 0x3000u)), "RT%u base = %08x", n, q[8]);
      CHECK(((q[0] >> 4) & 0xfffu) == 256u * n, "RT%u internal offset = %u, want %u", n, (q[0] >> 4) & 0xfffu, 256u * n);
      /* clear colour is per attachment: RT0 red, RT1 blue, RT2 green (UNORM8 bytes R,G,B,A) */
      CHECK(q[12] == (n == 0 ? 0x000000ffu : n == 1 ? 0x00ff0000u : 0x0000ff00u), "RT%u clear word %08x", n, q[12]);
   }
   CHECK(((w[11] >> 19) & 0xfu) == 2u, "Render Target Count - 1 = %u, want 2", (w[11] >> 19) & 0xfu);
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
   test_clear_words();
   test_poly_assemble();
   test_store_op();
   test_fbd_rt_store();
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
