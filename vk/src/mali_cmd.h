/* SPDX-License-Identifier: MIT */
/* M8, Vulkan layer: command buffers over vk_command_buffer. Recording only builds a CPU-side description (struct mali_frame per
 * render pass instance, mali_cs_emit.h); descriptors and the command stream are built at submit time from a rolling arena
 * (mali_cmd_submit), because the arena must not hand out VAs of an in-flight job (libkbase/INTERMITTENT.md).
 *
 * The device is Vulkan 1.0 (MALI_DEVICE_API_VERSION), so the 1.0 entrypoints are implemented. vkCmdBeginRendering is exported as
 * mali_CmdBeginRendering for the day dynamic rendering is exposed; render pass 1.0 goes through the runtime's emulation
 * (vk_common_CmdBeginRenderPass2 -> CmdBeginRendering) once the device advertises it. Viewport/scissor are taken from the runtime's
 * dynamic graphics state (vk_common_CmdSetViewport/Scissor).
 *
 * Seams (installed by wiring code, see R3-PLAN.md "M8"): mali_cmd_set_hw (descriptor memory + submit), mali_cmd_set_res (pipeline,
 * image and shader resolvers; M5/M6 own those objects). */
#ifndef MALI_CMD_H
#define MALI_CMD_H

#include "vk_command_buffer.h"
#include "vk_command_pool.h"
#include "vk_shader.h"
#include "mali_cs_emit.h"
#include "mali_event.h"
#include "mali_private.h"

#define MALI_CMD_WROTE_MAX 64

struct mali_cmd_buffer;
/* The reason is copied when retained for rate limiting. */
void mali_cmd_unsupported(struct mali_cmd_buffer *cmd, const char *what);

/* A render target or depth image as the command buffer needs it. */
struct mali_cmd_target {
   uint64_t va;
   uint32_t row_stride;
   unsigned width, height;
   const struct mali_rtfmt *rt;   /* colour: NULL = R8G8B8A8_UNORM */
   int zs_format;   /* 0 = colour (RGBA8), else enum kb_zs_format */
   bool is_zs;      /* a depth and/or stencil view */
   bool has_s;      /* the view has a stencil aspect: s_va / s_row_stride name the S8 plane */
   uint64_t s_va;
   uint32_t s_row_stride;
   uint32_t s_surface_stride;
   unsigned samples;     /* image sample count (1 or 4) */
   uint32_t surface_stride;   /* bytes per sample plane (MSAA images: samples consecutive) */
   bool multi;      /* image has more than one level/layer/sample or is not 2D: vkCmdClearColorImage takes the M12 compute path */
};

struct mali_cmd_res_ops {
   /* VkPipeline (graphics) -> state; arch of the compiled shaders must match the device. 0 = ok. */
   int (*pipeline)(VkPipeline p, struct mali_gfx_state *out);
   int (*image_view)(VkImageView v, struct mali_cmd_target *out);
   int (*image)(VkImage i, struct mali_cmd_target *out);   /* for vkCmdClear*Image */
   /* vk_shader (shader-object path, vk_device_shader_ops::cmd_bind_shaders) -> compiled shader + binary VA. 0 = ok. */
   int (*shader)(const struct vk_shader *s, const struct mali_compiled **c, uint64_t *va);
   /* VkPipeline (compute) -> compiled shader + binary VA. 0 = ok. */
   int (*compute)(VkPipeline p, const struct mali_compiled **c, uint64_t *va);
   /* R4: the pipeline's static graphics state (applied to the command buffer at bind). */
   int (*pipeline_dyn)(VkPipeline p, const struct vk_dynamic_graphics_state **out);
   /* M15: VkDeviceMemory behind a handle, for per-BO GPU-write tracking. NULL in tests (no fake casts). */
   struct mali_memory *(*buffer_mem)(VkBuffer b);
   struct mali_memory *(*image_mem)(VkImage i);
   struct mali_memory *(*image_view_mem)(VkImageView v);
};
void mali_cmd_set_res(const struct mali_cmd_res_ops *ops);
void mali_cmd_set_hw(const struct mali_hw *hw);   /* copied; NULL clears. Without it submit returns VK_ERROR_FEATURE_NOT_PRESENT */

struct mali_cmd_buffer {
   struct vk_command_buffer vk;

   struct mali_frame *frames;   /* finished render pass instances (and clear-only frames) */
   unsigned nframes, capframes;
   struct mali_frame cur;       /* the open one */
   bool in_render;
   /* VK_EXT_transform_feedback (stage 2): what the app bound and whether a streamout range is open. The streamout
    * job itself does not exist yet (libkbase kbase_cs.h has no vertex-only/streamout run), so these are recorded,
    * validated and marked written, and the draw is what still refuses (mali_compile.c names the missing lowering). */
   struct {
      VkBuffer buf[4];
      VkDeviceSize off[4], size[4];
      unsigned nbufs;
      bool active;
   } tf;

   struct mali_gfx_state gfx;
   bool have_gfx;
   uint64_t vb_va[MALI_MAX_VB];
   uint32_t vb_size[MALI_MAX_VB];
   uint32_t vb_stride[MALI_MAX_VB];   /* stride from vkCmdBindVertexBuffers2/3KHR setStride (used when the pipeline's VERTEX_INPUT_BINDING_STRIDE is dynamic) */
   uint8_t push[2][MALI_PUSH_MAX];
   unsigned nsets;
   uint64_t set_va[MALI_CMD_SETS];
   uint32_t set_bytes[MALI_CMD_SETS];
   unsigned ndyn;                       /* dynamic buffer descriptors (offset applied), table-0 entry MALI_DYN_BUF_START + i */
   uint32_t dyn_desc[MALI_MAX_DYN][8];
   uint64_t ib_va;       /* bound index buffer, offset applied (0 = none) */
   uint32_t ib_bytes;
   const uint8_t *ib_cpu;   /* CPU view of ib_va (NULL = unmapped) */
   unsigned ib_size;     /* bytes per index */
   const struct mali_compiled *cs;   /* bound compute pipeline */
   uint64_t cs_va;
   uint8_t push_cs[MALI_PUSH_MAX];
   uint64_t occ_va;             /* M13: active occlusion query counter address (0 = none), copied into every draw */
   uint32_t dirty;                  /* MALI_D_* accumulated by the state setters since the last draw snapshot */
   unsigned occ_mode;
   struct mali_op *def_ops;     /* M13: ops recorded inside a render pass, pushed after the pass' frame */
   unsigned ndef, capdef;
   struct mali_frame *def_frames;   /* meta frames recorded inside a render pass: pushed after the pass' frame, in order */
   unsigned ndef_frames, capdef_frames;
   /* M15: memory the GPU writes in this command buffer (render targets, copy destinations, clears). Applied as
    * per-BO write marks at submit, so a fence wait only CSYNCs those BOs (vk/MEMORY.md). Full = one write was
    * not recorded: the next device->CPU sync then sweeps every BO. */
   struct mali_memory *wrote[MALI_CMD_WROTE_MAX];
   unsigned nwrote;
   bool wrote_full;
   /* Deferred render-pass end (mali_cmd.c "pass merge", MALI_PASS_MERGE): EndRendering parks the finished pass in `pm`
    * instead of pushing it; the next BeginRendering on the same attachments continues it (one tiler + fragment pass, no
    * store/preload pair), anything else pushes it first (pass_close). pm_keep: per colour/depth/stencil, a storeOp STORE
    * of the chain that a later pass may not drop (suspend-forced stores excluded). pass_*: the same facts of the open pass. */
   struct mali_frame pm;
   bool pm_open, pm_suspended, pm_keep[3];
   bool pass_suspending, pass_nomerge, pass_keep[3];
   unsigned pass_draws0, pass_clear_quads;   /* cur.ndraws once the pass' own preload draws are in; ClearAttachments quads */
   /* MALI_PASS_STATS: why the next BeginRendering cannot continue a parked pass (enum mali_pass_why, the boundary that
    * closed it or MALI_PW_FIRST), and the BeginRendering count of this recording (passes per submit). */
   unsigned pass_why, npasses;
};

/* Reason a pass is not mergeable with the previous pass of its command buffer (MALI_PASS_STATS nm_* counters). */
enum mali_pass_why {
   MALI_PW_FIRST,      /* no earlier pass in this command buffer */
   MALI_PW_UNPARKED,   /* the previous pass could not be parked: resolve, ops/meta recorded inside it, transform feedback */
   MALI_PW_FRAME,      /* a frame between them: copy, blit, clear, dispatch, query/event/timestamp op */
   MALI_PW_BARRIER,    /* a barrier that orders more than attachment access */
   MALI_PW_QUERY,      /* vkCmdBeginQuery / vkCmdEndQuery between them */
   MALI_PW_SECONDARY,  /* vkCmdExecuteCommands between them */
   MALI_PW_FLAGS,      /* the new pass resolves, is layered or multiview */
   MALI_PW_SIZE,       /* framebuffer size or sample count differ */
   MALI_PW_AREA,       /* render area differs */
   MALI_PW_ATTACH,     /* attachment set or views differ */
   MALI_PW_CLEAR,      /* the new pass clears an attachment the previous one drew */
   MALI_PW_N
};

/* MALI_PASS_STATS counters (cumulative per process). MALI_PASS_STATS=1 prints one "mali_pass:" line of the deltas every
 * MALI_PASS_STATS_EVERY submits (default 300) and "mali_pass_total:" at vkDestroyDevice, both on stderr. */
enum mali_pass_stat {
   MALI_PS_PASSES, MALI_PS_LOAD_C, MALI_PS_LOAD_Z, MALI_PS_LOAD_S, MALI_PS_MERGEABLE, MALI_PS_MERGED, MALI_PS_FOLDED,
   MALI_PS_EMPTY, MALI_PS_PRELOADS, MALI_PS_PRELOAD_PX, MALI_PS_STORE_PX, MALI_PS_PARTIAL, MALI_PS_FRAMES, MALI_PS_RESUMED,
   /* loadOp CLEAR and DONT_CARE/NONE per colour/depth/stencil (LOAD is MALI_PS_LOAD_*) */
   MALI_PS_CLEAR_C, MALI_PS_CLEAR_Z, MALI_PS_CLEAR_S, MALI_PS_DC_C, MALI_PS_DC_Z, MALI_PS_DC_S,
   /* nominal 16x16 tiles of the render area, and of the union of the pass' draw boxes (viewport & scissor & area) */
   MALI_PS_AREA_TILES, MALI_PS_TOUCHED_TILES,
   /* predicted pixel traffic a merge removes over the mergeable joins: the new pass' preloads + the previous pass' stores */
   MALI_PS_SAVE_PX,
   /* submits (mali_pass_stats_submit), passes and render frames in submitted command buffers, most passes in one submit */
   MALI_PS_SUBMITS, MALI_PS_SUB_PASSES, MALI_PS_SUB_FRAMES, MALI_PS_SUB_MAXPASSES,
   MALI_PS_NM0,   /* MALI_PS_NM0 + enum mali_pass_why: passes not mergeable with the previous one, by reason */
   MALI_PS_N = MALI_PS_NM0 + MALI_PW_N
};
void mali_pass_stats_read(uint64_t out[MALI_PS_N], bool reset);
/* One vkQueueSubmit has run its command buffers (mali_queue.c): counts it and prints the summary line every
 * MALI_PASS_STATS_EVERY submits (default 300). mali_pass_stats_print_total: the process totals (vkDestroyDevice). */
void mali_pass_stats_submit(void);
void mali_pass_stats_print_total(void);
const char *mali_pass_stat_name(unsigned i);   /* the key printed for counter i */
/* vkCmdPipelineBarrier2 hook (mali_pipeline.c): a barrier that makes attachment writes visible to anything but attachment
 * access closes a parked pass (it cannot merge with the next one). */
void mali_cmd_barrier(VkCommandBuffer h, const VkDependencyInfo *info);

static inline struct mali_cmd_buffer *
mali_cmd_buffer_from_vk(struct vk_command_buffer *cmd)
{
   return container_of(cmd, struct mali_cmd_buffer, vk);
}

/* M12/M13 hooks (mali_cmd.c): append a finished frame / an ops frame; ops recorded inside a render pass are deferred until it ends. */
void mali_cmd_push_frame(struct mali_cmd_buffer *cmd, const struct mali_frame *f);
/* Same, but a frame recorded while a render pass is open is queued and pushed when the pass ends: transfer work
 * recorded inside a pass executes after it, in submission order, which is what Vulkan requires. */
void mali_cmd_push_frame_deferred(struct mali_cmd_buffer *cmd, const struct mali_frame *f);
void mali_cmd_push_ops(struct mali_cmd_buffer *cmd, const struct mali_op *ops, unsigned n);
void mali_cmd_set_occlusion(struct mali_cmd_buffer *cmd, uint64_t va, unsigned mode);
/* M15: record that the GPU writes `mem` through this command buffer. Idempotent; overflow falls back to a full
 * sweep at the next device->CPU sync. NULL/zero-size memory is ignored. */
void mali_cmd_mark_written(struct mali_cmd_buffer *cmd, struct mali_memory *mem);
/* Same, by handle, through the resolvers (NULL-safe; a no-op where the resolver is not installed, e.g. unit tests). */
void mali_cmd_mark_buffer_written(struct mali_cmd_buffer *cmd, VkBuffer b);

/* VK_EXT_transform_feedback (stage 2): recorded and validated here; the streamout job itself is not implemented yet. */
void mali_CmdBindTransformFeedbackBuffersEXT(VkCommandBuffer h, uint32_t first, uint32_t count, const VkBuffer *buffers,
                                             const VkDeviceSize *offsets, const VkDeviceSize *sizes);
void mali_CmdBeginTransformFeedbackEXT(VkCommandBuffer h, uint32_t first_counter_buffer, uint32_t counter_buffer_count,
                                       const VkBuffer *counter_buffers, const VkDeviceSize *counter_buffer_offsets);
void mali_CmdEndTransformFeedbackEXT(VkCommandBuffer h, uint32_t first_counter_buffer, uint32_t counter_buffer_count,
                                     const VkBuffer *counter_buffers, const VkDeviceSize *counter_buffer_offsets);
void mali_cmd_mark_image_written(struct mali_cmd_buffer *cmd, VkImage i);
bool mali_resolve_1x_clamp(VkImageResolve2 *region, const int32_t area[4],
                           uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh);
void mali_resolve_1x_slices(VkImageResolve2 *region, bool src3d, bool dst3d);

/* Install as dev->vk.command_buffer_ops (vk_common_Create/Allocate/Free/ResetCommandPool and Begin/End then work as is). */
extern const struct vk_command_buffer_ops mali_cmd_ops;

/* Builds + submits every recorded frame of the given command buffers, in order. VkResult (device lost on a build/submit failure).
 * To be called from mali_queue_submit (M4 currently sends an empty CS, wiring item). */
VkResult mali_cmd_submit(struct vk_command_buffer *cmd);

/* Hooks with the vk_device_shader_ops signatures (M6 wiring). */
void mali_cmd_bind_shaders(struct vk_command_buffer *cmd, uint32_t stage_count, const mesa_shader_stage *stages,
                           struct vk_shader *const *shaders);

/* Entrypoints implemented here (picked up by the generated weak mali_entrypoints). */
VKAPI_ATTR VkResult VKAPI_CALL mali_BeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo *);
VKAPI_ATTR VkResult VKAPI_CALL mali_EndCommandBuffer(VkCommandBuffer);
VKAPI_ATTR void VKAPI_CALL mali_CmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
VKAPI_ATTR void VKAPI_CALL mali_CmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t,
                                                      const VkDescriptorSet *, uint32_t, const uint32_t *);
VKAPI_ATTR void VKAPI_CALL mali_CmdBindVertexBuffers(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer *, const VkDeviceSize *);
VKAPI_ATTR void VKAPI_CALL mali_CmdBindIndexBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType);
VKAPI_ATTR void VKAPI_CALL mali_CmdBindDescriptorSets2KHR(VkCommandBuffer, const VkBindDescriptorSetsInfoKHR *);
VKAPI_ATTR void VKAPI_CALL mali_CmdPushConstants2KHR(VkCommandBuffer, const VkPushConstantsInfoKHR *);
VKAPI_ATTR void VKAPI_CALL mali_CmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t,
                                                 const void *);
VKAPI_ATTR void VKAPI_CALL mali_CmdDispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL mali_CmdDispatchIndirect(VkCommandBuffer, VkBuffer, VkDeviceSize);
VKAPI_ATTR void VKAPI_CALL mali_CmdDrawIndirect(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL mali_CmdDrawIndexedIndirect(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL mali_CmdDrawIndirectCount(VkCommandBuffer, VkBuffer, VkDeviceSize, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL mali_CmdDrawIndexedIndirectCount(VkCommandBuffer, VkBuffer, VkDeviceSize, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL mali_CmdDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL mali_CmdDrawIndexed(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL mali_CmdBeginRendering(VkCommandBuffer, const VkRenderingInfo *);
VKAPI_ATTR void VKAPI_CALL mali_CmdExecuteCommands(VkCommandBuffer, uint32_t, const VkCommandBuffer *);
VKAPI_ATTR void VKAPI_CALL mali_CmdEndRendering(VkCommandBuffer);
VKAPI_ATTR void VKAPI_CALL mali_CmdClearColorImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue *, uint32_t,
                                                   const VkImageSubresourceRange *);
VKAPI_ATTR void VKAPI_CALL mali_CmdClearDepthStencilImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearDepthStencilValue *,
                                                          uint32_t, const VkImageSubresourceRange *);
VKAPI_ATTR void VKAPI_CALL mali_CmdClearAttachments(VkCommandBuffer, uint32_t, const VkClearAttachment *, uint32_t,
                                                    const VkClearRect *);

/* Region of the deferred MSAA resolve (colour attachment storeOp STORE with a resolve): source = attachment view
 * subresource, destination = resolve view subresource, rect = the pass's render area. */
void mali_resolve_region(const struct mali_frame *f, VkImageResolve2 *r);

#endif
