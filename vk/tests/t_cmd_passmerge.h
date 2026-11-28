/* SPDX-License-Identifier: MIT */
/* t_cmd --passmerge: render-pass merging (mali_cmd.c "pass merge": deferred end, rule A merge, rule B fold, suspend/resume)
 * and the MALI_PASS_STATS counters. Included by t_cmd.c after its helpers (new_cmd, make_buffer, F, ls_preloads).
 * Record-time checks on the command buffer's frame list, then a submit through the fake backend: F.nsubmit counts the
 * frames that reach the CS ring, so "one frame in the dump" is F.nsubmit + 1. */

struct pm_pass {
   uintptr_t col, dep;                 /* view handles of res_view (0 = none): 1 RGBA8 colour, 2 D32F, 3 D32F + S8 */
   VkAttachmentLoadOp lc, ld;          /* 0 = LOAD */
   VkAttachmentStoreOp sc, sd;         /* 0 = STORE */
   VkRenderingFlags fl;
   int32_t x, y;
   uint32_t w, h;                      /* 0 = the whole W x H attachment */
   bool nodraw;
};

static void
pm_rec(VkCommandBuffer cb, const struct pm_pass *p)
{
   DFN(CmdBindPipeline);
   DFN(CmdSetViewport);
   DFN(CmdSetScissor);
   DFN(CmdDraw);
   VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = (VkImageView)p->col,
                                   .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = p->lc, .storeOp = p->sc};
   ca.clearValue.color.float32[0] = 0.25f;
   ca.clearValue.color.float32[3] = 1.0f;
   VkRenderingAttachmentInfo da = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = (VkImageView)p->dep,
                                   .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, .loadOp = p->ld, .storeOp = p->sd};
   da.clearValue.depthStencil.depth = 0.5f;
   uint32_t w = p->w ? p->w : W, h = p->h ? p->h : H;
   VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .flags = p->fl, .renderArea = {{p->x, p->y}, {w, h}},
                         .layerCount = 1, .colorAttachmentCount = p->col ? 1 : 0, .pColorAttachments = p->col ? &ca : NULL,
                         .pDepthAttachment = p->dep ? &da : NULL};
   mali_CmdBeginRendering(cb, &ri);
   if (!p->nodraw) {
      CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)(uintptr_t)1);
      VkViewport vp = {(float)p->x, (float)p->y, (float)w, (float)h, 0.0f, 1.0f};
      VkRect2D scr = {{p->x, p->y}, {w, h}};
      CmdSetViewport(cb, 0, 1, &vp);
      CmdSetScissor(cb, 0, 1, &scr);
      CmdDraw(cb, 3, 1, 0, 0);
   }
   mali_CmdEndRendering(cb);
}

/* Ends and submits the command buffer, checks the frame count at record time and in the fake backend. */
static struct mali_cmd_buffer *
pm_finish(VkCommandBuffer cb, const char *name, unsigned want_frames)
{
   DFN(EndCommandBuffer);
   VkResult r = EndCommandBuffer(cb);
   CHECK(r == VK_SUCCESS, "%s: EndCommandBuffer %d", name, r);
   struct mali_cmd_buffer *m = mali_cmd_buffer_from_vk(vk_command_buffer_from_handle(cb));
   CHECK(!m->pm_open, "%s: a pass is still parked after vkEndCommandBuffer", name);
   CHECK(m->nframes == want_frames, "%s: %u frames (want %u)", name, m->nframes, want_frames);
   unsigned before = F.nsubmit;
   r = mali_cmd_submit(&m->vk);
   CHECK(r == VK_SUCCESS && F.nsubmit - before == want_frames, "%s: submit %d, %u frames on the ring (want %u)", name, r,
         F.nsubmit - before, want_frames);
   return m;
}

/* app draws of a frame: every rec whose fragment shader is the test pipeline's */
static unsigned
pm_app_draws(const struct mali_frame *f)
{
   unsigned n = 0;
   for (unsigned i = 0; i < f->ndraws; i++)
      n += f->draws[i].pipe && f->draws[i].pipe->gfx.fs == &g_fs;
   return n;
}

static void
pm_stats_expect(const char *name, const uint64_t *want, const int *idx, unsigned n)
{
   uint64_t s[MALI_PS_N];
   mali_pass_stats_read(s, true);
   static const char *const nm[MALI_PS_N] = {"passes", "load_c", "load_z", "load_s", "mergeable", "merged", "folded", "empty",
                                             "preloads", "preload_px", "store_px", "partial", "frames", "resumed"};
   for (unsigned i = 0; i < n; i++)
      CHECK(s[idx[i]] == want[i], "%s: stat %s = %llu (want %llu)", name, nm[idx[i]], (unsigned long long)s[idx[i]],
            (unsigned long long)want[i]);
}

static void
test_passmerge(VkCommandPool pool)
{
   const VkAttachmentLoadOp CLR = VK_ATTACHMENT_LOAD_OP_CLEAR, DC = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   const VkAttachmentStoreOp SDC = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   const uint64_t px = (uint64_t)W * H;
   VkCommandBuffer cb;
   struct mali_cmd_buffer *m;
   uint64_t s[MALI_PS_N];
   setenv("MALI_PASS_STATS", "1", 1);
   unsetenv("MALI_PASS_MERGE");
   mali_pass_stats_read(s, true);

   /* ---- must merge ---- */
   /* two passes: CLEAR+draw, then LOAD+draw on the same colour view: one frame, two app draws, no preload */
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR});
   pm_rec(cb, &(struct pm_pass){.col = 1});
   m = pm_finish(cb, "merge_two", 1);
   if (m->nframes == 1) {
      CHECK(m->frames[0].ndraws == 2 && pm_app_draws(&m->frames[0]) == 2 && ls_preloads(&m->frames[0]) == 0,
            "merge_two: %u draws, %u preloads", m->frames[0].ndraws, ls_preloads(&m->frames[0]));
      CHECK(m->frames[0].colour.clear && m->frames[0].colour.store, "merge_two: the first pass' CLEAR and the store survive");
   }
   {
      static const int ix[] = {MALI_PS_PASSES, MALI_PS_LOAD_C, MALI_PS_MERGEABLE, MALI_PS_MERGED, MALI_PS_PRELOADS,
                               MALI_PS_PRELOAD_PX, MALI_PS_STORE_PX, MALI_PS_FRAMES, MALI_PS_EMPTY, MALI_PS_PARTIAL};
      const uint64_t want[] = {2, 1, 1, 1, 0, 0, px, 1, 0, 0};
      pm_stats_expect("merge_two", want, ix, 10);
   }

   /* five passes (the 4-layer UI composite plus its clear): one frame, five draws */
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR});
   for (int i = 0; i < 4; i++)
      pm_rec(cb, &(struct pm_pass){.col = 1});
   m = pm_finish(cb, "merge_five", 1);
   if (m->nframes == 1)
      CHECK(pm_app_draws(&m->frames[0]) == 5 && ls_preloads(&m->frames[0]) == 0, "merge_five: %u app draws %u preloads",
            pm_app_draws(&m->frames[0]), ls_preloads(&m->frames[0]));

   /* colour + depth, LOAD on both, an attachment-only barrier in between (still mergeable) */
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .dep = 2, .lc = CLR, .ld = CLR});
   {
      DFN(CmdPipelineBarrier);   /* the 1.0 entry point ANGLE calls; the runtime turns it into CmdPipelineBarrier2 */
      VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
      CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1, &mb,
                         0, NULL, 0, NULL);
   }
   pm_rec(cb, &(struct pm_pass){.col = 1, .dep = 2});
   m = pm_finish(cb, "merge_zs_att_barrier", 1);
   if (m->nframes == 1)
      CHECK(m->frames[0].zs.clear && m->frames[0].zs.store && ls_preloads(&m->frames[0]) == 0, "merge_zs_att_barrier: zs");

   /* the second pass stores nothing: the first pass' STORE is kept (a later pass may not drop it) */
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .dep = 2, .lc = CLR, .ld = CLR});
   pm_rec(cb, &(struct pm_pass){.col = 1, .dep = 2, .sd = SDC});
   m = pm_finish(cb, "merge_keep_store", 1);
   if (m->nframes == 1)
      CHECK(m->frames[0].zs.store && m->frames[0].colour.store, "merge_keep_store: the first pass' depth STORE was dropped");

   /* ---- suspend / resume ---- */
   /* the pair merges; the suspended pass' DONT_CARE store and the resumed pass' CLEAR are not performed */
   mali_pass_stats_read(s, true);
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR, .sc = SDC, .fl = VK_RENDERING_SUSPENDING_BIT});
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR, .fl = VK_RENDERING_RESUMING_BIT});
   m = pm_finish(cb, "suspend_resume", 1);
   if (m->nframes == 1)
      CHECK(m->frames[0].colour.store && pm_app_draws(&m->frames[0]) == 2 && ls_preloads(&m->frames[0]) == 0,
            "suspend_resume: store %d, %u draws", m->frames[0].colour.store, m->frames[0].ndraws);
   {
      static const int ix[] = {MALI_PS_PASSES, MALI_PS_RESUMED, MALI_PS_MERGED, MALI_PS_FRAMES};
      const uint64_t want[] = {2, 1, 1, 1};
      pm_stats_expect("suspend_resume", want, ix, 4);
   }
   /* three-way chain: suspend, resume+suspend, resume */
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR, .fl = VK_RENDERING_SUSPENDING_BIT});
   pm_rec(cb, &(struct pm_pass){.col = 1, .fl = VK_RENDERING_RESUMING_BIT | VK_RENDERING_SUSPENDING_BIT});
   pm_rec(cb, &(struct pm_pass){.col = 1, .fl = VK_RENDERING_RESUMING_BIT});
   m = pm_finish(cb, "suspend_resume_chain", 1);
   /* suspended in one command buffer, resumed in the next: the suspended pass stores, the resumed one loads (no CLEAR) */
   {
      VkCommandBuffer c2;
      new_cmd(pool, &cb);
      pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR, .sc = SDC, .fl = VK_RENDERING_SUSPENDING_BIT});
      m = pm_finish(cb, "suspend_cross_a", 1);
      if (m->nframes == 1)
         CHECK(m->frames[0].colour.store, "suspend_cross_a: a suspended pass closed at vkEndCommandBuffer must store");
      new_cmd(pool, &c2);
      pm_rec(c2, &(struct pm_pass){.col = 1, .lc = CLR, .fl = VK_RENDERING_RESUMING_BIT});
      m = pm_finish(c2, "suspend_cross_b", 1);
      if (m->nframes == 1)
         CHECK(!m->frames[0].colour.clear && ls_preloads(&m->frames[0]) == 1, "suspend_cross_b: a resumed pass must load, not clear");
   }

   /* ---- must not merge ---- */
   mali_pass_stats_read(s, true);
   struct vk_buffer *b0 = make_buffer(F.geo.gpu_va, 4096), *b1 = make_buffer(F.geo.gpu_va + 4096, 4096);
   /* a transfer between the passes (a copy reading back the attachment orders the same way: every pushed frame closes) */
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR});
   {
      DFN(CmdCopyBuffer);
      VkBufferCopy bc = {0, 0, 256};
      CmdCopyBuffer(cb, vk_buffer_to_handle(b0), vk_buffer_to_handle(b1), 1, &bc);
   }
   pm_rec(cb, &(struct pm_pass){.col = 1});
   m = pm_finish(cb, "no_merge_copy", 3);
   if (m->nframes == 3)
      CHECK(!m->frames[0].compute && m->frames[1].compute && ls_preloads(&m->frames[2]) == 1, "no_merge_copy: order pass, copy, pass");
   /* a layout change to a sampling layout (the next pass samples the attachment) */
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR});
   {
      DFN(CmdPipelineBarrier);
      VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_SHADER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = (VkImage)(uintptr_t)1, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL,
                         1, &ib);
   }
   pm_rec(cb, &(struct pm_pass){.col = 1});
   pm_finish(cb, "no_merge_sampling_barrier", 2);
   /* A pure execution dependency still orders fragments across passes, even
    * though neither access mask requests a memory dependency. */
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR});
   {
      DFN(CmdPipelineBarrier);
      VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb,
                         0, NULL, 0, NULL);
   }
   pm_rec(cb, &(struct pm_pass){.col = 1});
   m = pm_finish(cb, "no_merge_execution_barrier", 2);
   if (m->nframes == 2)
      CHECK(pm_app_draws(&m->frames[0]) == 1 && pm_app_draws(&m->frames[1]) == 1 && ls_preloads(&m->frames[1]) == 1,
            "no_merge_execution_barrier: expected one draw per frame and a preload");
   /* a different render area */
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR});
   pm_rec(cb, &(struct pm_pass){.col = 1, .x = 16, .y = 16, .w = 64, .h = 64});
   m = pm_finish(cb, "no_merge_area", 2);
   if (m->nframes == 2)
      CHECK(ls_preloads(&m->frames[1]) == 1, "no_merge_area: the second pass must preload");
   /* different attachments: colour only, then colour + D32F depth, then colour + D24X8 depth */
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR});
   pm_rec(cb, &(struct pm_pass){.col = 1, .dep = 2});
   pm_rec(cb, &(struct pm_pass){.col = 1, .dep = 4, .ld = DC});   /* D24X8: another depth format at the same address */
   pm_finish(cb, "no_merge_attachments", 3);
   /* the second pass clears what the first stored */
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR});
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR});
   pm_finish(cb, "no_merge_clear", 2);
   /* a query boundary: occlusion begin (mali_cmd_set_occlusion), then an op (EndQuery / timestamp / availability) */
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR});
   mali_cmd_set_occlusion(mali_cmd_buffer_from_vk(vk_command_buffer_from_handle(cb)), F.vb.gpu_va + 512, MALI_OCC_MODE_COUNTER);
   pm_rec(cb, &(struct pm_pass){.col = 1});
   mali_cmd_set_occlusion(mali_cmd_buffer_from_vk(vk_command_buffer_from_handle(cb)), 0, 0);
   {
      struct mali_op e = {.kind = MALI_OP_AVAIL, .va = F.vb.gpu_va + 448, .val = 1};
      mali_cmd_push_ops(mali_cmd_buffer_from_vk(vk_command_buffer_from_handle(cb)), &e, 1);
   }
   pm_rec(cb, &(struct pm_pass){.col = 1});
   m = pm_finish(cb, "no_merge_query", 4);
   if (m->nframes == 4)
      CHECK(m->frames[2].nops == 1 && !m->frames[3].nops, "no_merge_query: order pass, pass, op, pass");
   /* a secondary command buffer executed between the passes */
   {
      DFN(AllocateCommandBuffers);
      DFN(BeginCommandBuffer);
      DFN(EndCommandBuffer);
      DFN(CmdExecuteCommands);
      VkCommandBuffer sec;
      VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY, .commandBufferCount = 1};
      CHECK(AllocateCommandBuffers(dev, &ai, &sec) == VK_SUCCESS, "secondary alloc");
      VkCommandBufferInheritanceInfo inh = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
      VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pInheritanceInfo = &inh};
      CHECK(BeginCommandBuffer(sec, &bi) == VK_SUCCESS && EndCommandBuffer(sec) == VK_SUCCESS, "secondary record");
      new_cmd(pool, &cb);
      pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR});
      CmdExecuteCommands(cb, 1, &sec);
      pm_rec(cb, &(struct pm_pass){.col = 1});
      pm_finish(cb, "no_merge_secondary", 2);
   }
   {
      /* only the area and attachment cases count as "mergeable = 0"; nothing merged anywhere in this block */
      mali_pass_stats_read(s, false);
      CHECK(s[MALI_PS_MERGED] == 0 && s[MALI_PS_MERGEABLE] == 0, "no_merge block: merged %llu mergeable %llu",
            (unsigned long long)s[MALI_PS_MERGED], (unsigned long long)s[MALI_PS_MERGEABLE]);
   }

   /* ---- rule B: an empty clear pass folds into the next pass' LOAD ---- */
   mali_pass_stats_read(s, true);
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR, .nodraw = true});           /* colour-only clear, no draw */
   pm_rec(cb, &(struct pm_pass){.col = 1, .dep = 2, .ld = DC});                  /* other attachment set: no merge, a fold */
   m = pm_finish(cb, "fold_clear_pass", 1);
   if (m->nframes == 1)
      CHECK(m->frames[0].has_zs && m->frames[0].colour.clear && m->frames[0].colour.clear_color[0] == 0.25f &&
            ls_preloads(&m->frames[0]) == 0, "fold_clear_pass: clear %d preloads %u", m->frames[0].colour.clear,
            ls_preloads(&m->frames[0]));
   {
      static const int ix[] = {MALI_PS_PASSES, MALI_PS_EMPTY, MALI_PS_FOLDED, MALI_PS_MERGED, MALI_PS_FRAMES, MALI_PS_PRELOADS};
      const uint64_t want[] = {2, 1, 1, 0, 1, 0};
      pm_stats_expect("fold_clear_pass", want, ix, 6);
   }
   /* vkCmdClearColorImage of the whole image, then a pass that LOADs it */
   new_cmd(pool, &cb);
   {
      VkClearColorValue cv = {.float32 = {0.0f, 1.0f, 0.0f, 1.0f}};
      VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      mali_CmdClearColorImage(cb, (VkImage)(uintptr_t)1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rg);
   }
   pm_rec(cb, &(struct pm_pass){.col = 1});
   m = pm_finish(cb, "fold_clear_image", 1);
   if (m->nframes == 1)
      CHECK(m->frames[0].colour.clear && m->frames[0].colour.clear_color[1] == 1.0f && ls_preloads(&m->frames[0]) == 0 &&
            pm_app_draws(&m->frames[0]) == 1, "fold_clear_image: clear %d preloads %u", m->frames[0].colour.clear,
            ls_preloads(&m->frames[0]));
   /* not folded: the next pass covers less than the cleared area */
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR, .nodraw = true});
   pm_rec(cb, &(struct pm_pass){.col = 1, .w = 128});
   pm_finish(cb, "no_fold_area", 2);
   /* not folded: the next pass does not have the cleared attachment */
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .dep = 2, .lc = CLR, .ld = CLR, .nodraw = true});
   pm_rec(cb, &(struct pm_pass){.col = 1});
   pm_finish(cb, "no_fold_missing_depth", 2);

   /* ---- MALI_PASS_MERGE=0: today's one frame per pass, counters still count ---- */
   setenv("MALI_PASS_MERGE", "0", 1);
   mali_pass_stats_read(s, true);
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR});
   pm_rec(cb, &(struct pm_pass){.col = 1});
   m = pm_finish(cb, "merge_off", 2);
   if (m->nframes == 2)
      CHECK(ls_preloads(&m->frames[1]) == 1 && pm_app_draws(&m->frames[1]) == 1, "merge_off: the second pass preloads");
   {
      static const int ix[] = {MALI_PS_PASSES, MALI_PS_LOAD_C, MALI_PS_MERGEABLE, MALI_PS_MERGED, MALI_PS_PRELOADS,
                               MALI_PS_PRELOAD_PX, MALI_PS_STORE_PX, MALI_PS_FRAMES};
      const uint64_t want[] = {2, 1, 1, 0, 1, px, 2 * px, 2};
      pm_stats_expect("merge_off", want, ix, 8);
   }
   /* a smaller render area counts as partial */
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR, .w = 64, .h = 32});
   pm_finish(cb, "partial", 1);
   mali_pass_stats_read(s, true);
   CHECK(s[MALI_PS_PARTIAL] == 1 && s[MALI_PS_STORE_PX] == 64 * 32, "partial: %llu, store_px %llu",
         (unsigned long long)s[MALI_PS_PARTIAL], (unsigned long long)s[MALI_PS_STORE_PX]);
   /* the suspend/resume pair still renders correctly unmerged: suspended stores, resumed loads */
   new_cmd(pool, &cb);
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR, .sc = SDC, .fl = VK_RENDERING_SUSPENDING_BIT});
   pm_rec(cb, &(struct pm_pass){.col = 1, .lc = CLR, .fl = VK_RENDERING_RESUMING_BIT});
   m = pm_finish(cb, "suspend_resume_off", 2);
   if (m->nframes == 2)
      CHECK(m->frames[0].colour.store && !m->frames[1].colour.clear && ls_preloads(&m->frames[1]) == 1,
            "suspend_resume_off: store %d / clear %d preloads %u", m->frames[0].colour.store, m->frames[1].colour.clear,
            ls_preloads(&m->frames[1]));
   unsetenv("MALI_PASS_MERGE");
   unsetenv("MALI_PASS_STATS");
   free(b0);
   free(b1);
   printf("{\"test\":\"t_cmd%d\",\"passmerge\":true,\"pass\":%s}\n", ARCH, g_fail ? "false" : "true");
}
