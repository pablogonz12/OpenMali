/* Dynamic-rendering modifier RT matrix. Included by t_queue.c. */
#include "modrt_spv.h"

#define MODRT_FNS(X) X(CreateImageView) X(DestroyImageView) X(CreateShaderModule) X(DestroyShaderModule) \
   X(CreateGraphicsPipelines) X(DestroyPipeline) X(CmdBeginRendering) X(CmdEndRendering) \
   X(CmdBindPipeline) X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw)
#define MODRT_DECLARE(n) static PFN_vk##n mr_##n;
MODRT_FNS(MODRT_DECLARE)
#undef MODRT_DECLARE

static VkPipeline
modrt_pipeline(VkFormat format, VkPipelineLayout layout)
{
   VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
   VkPipeline pipe = VK_NULL_HANDLE;
   VkShaderModuleCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = sizeof modrt_vert, .pCode = modrt_vert};
   if (mr_CreateShaderModule(dev, &sci, NULL, &vs) != VK_SUCCESS) return VK_NULL_HANDLE;
   sci.codeSize = sizeof modrt_frag; sci.pCode = modrt_frag;
   if (mr_CreateShaderModule(dev, &sci, NULL, &fs) != VK_SUCCESS) goto done;
   VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
   VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState ca = {.colorWriteMask = 15};
   VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ca};
   VkDynamicState dyns[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
   VkPipelineDynamicStateCreateInfo dyn = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyns};
   VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
   VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
      .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
      .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
      .pColorBlendState = &cb, .pDynamicState = &dyn, .layout = layout};
   if (mr_CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gi, NULL, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
done:
   if (fs) mr_DestroyShaderModule(dev, fs, NULL);
   if (vs) mr_DestroyShaderModule(dev, vs, NULL);
   return pipe;
}

static void
modrt_hex(const uint8_t *p, char out[33])
{
   for (unsigned i = 0; i < 16; i++) sprintf(out + 2*i, "%02x", p[i]);
}

static int
modrt_variant(int tiling, int imported, unsigned width, VkFormat fmt, int mode, VkPipeline pipe)
{
   const uint64_t modifier = 0;
   const unsigned height = 720;
   const char *error = NULL;
   VkImage image = VK_NULL_HANDLE;
   VkImageView view = VK_NULL_HANDLE;
   VkDeviceMemory owner = VK_NULL_HANDLE, bound = VK_NULL_HANDLE;
   VkCommandPool pool = VK_NULL_HANDLE;
   VkFence fence = VK_NULL_HANDLE;
   int fd = -1, import_fd = -1, pass = 0;
   uint8_t *mapped = NULL, *dmamap = MAP_FAILED;
   VkMemoryRequirements req = {0};
   VkSubresourceLayout plane = {0};
   char a[33] = "", b[33] = "", c[33] = "", d[33] = "";
#define TRY(call) do { if (!(call)) { error = #call; goto done; } } while (0)
   VkExternalMemoryImageCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
   VkImageDrmFormatModifierListCreateInfoEXT list = {.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
      .pNext = &ext, .drmFormatModifierCount = 1, .pDrmFormatModifiers = &modifier};
   VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = tiling ? (void *)&list : (void *)&ext,
      .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {width,height,1}, .mipLevels = 1,
      .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = tiling ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   TRY(CreateImage(dev, &ii, NULL, &image) == VK_SUCCESS);
   VkImageSubresource sub = {.aspectMask = tiling ? VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT : VK_IMAGE_ASPECT_COLOR_BIT};
   GetImageSubresourceLayout(dev, image, &sub, &plane);
   if (tiling == 2) {
      DestroyImage(dev, image, NULL); image = VK_NULL_HANDLE;
      VkImageDrmFormatModifierExplicitCreateInfoEXT ex = {.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
         .pNext = &ext, .drmFormatModifier = modifier, .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &plane};
      ii.pNext = &ex;
      TRY(CreateImage(dev, &ii, NULL, &image) == VK_SUCCESS);
      GetImageSubresourceLayout(dev, image, &sub, &plane);
   }
   GetImageMemoryRequirements(dev, image, &req);
   uint32_t mt = 0;
   while (mt < memprops.memoryTypeCount &&
          (!(req.memoryTypeBits & (1u << mt)) || !(memprops.memoryTypes[mt].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) mt++;
   TRY(mt < memprops.memoryTypeCount);
   VkExportMemoryAllocateInfo exp = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &exp, .allocationSize = req.size, .memoryTypeIndex = mt};
   TRY(AllocateMemory(dev, &ai, NULL, &owner) == VK_SUCCESS);
   VkMemoryGetFdInfoKHR getfd = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = owner,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
   TRY(GetMemoryFdKHR(dev, &getfd, &fd) == VK_SUCCESS && fd >= 0);
   bound = owner;
   if (imported) {
      import_fd = dup(fd);
      TRY(import_fd >= 0);
      VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
         .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = import_fd};
      ai.pNext = &imp; bound = VK_NULL_HANDLE;
      TRY(AllocateMemory(dev, &ai, NULL, &bound) == VK_SUCCESS);
      import_fd = -1; /* Vulkan owns the descriptor after successful import. */
   }
   TRY(BindImageMemory(dev, image, bound, 0) == VK_SUCCESS);
   VkImageViewCreateInfo iv = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
   TRY(mr_CreateImageView(dev, &iv, NULL, &view) == VK_SUCCESS);
   VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   TRY(CreateCommandPool(dev, &pi, NULL, &pool) == VK_SUCCESS);
   VkCommandBuffer cb;
   VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   TRY(AllocateCommandBuffers(dev, &cbai, &cb) == VK_SUCCESS);
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   TRY(BeginCommandBuffer(cb, &bi) == VK_SUCCESS);
   VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      0, 0, NULL, 0, NULL, 1, &barrier);
   VkRenderingAttachmentInfo att = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue.color.float32 = {0.0f, 1.0f, 0.0f, 1.0f}};
   VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0,0},{width,height}},
      .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &att};
   /* LOAD starts from a preceding clear pass; the dependency makes that content available. */
   if (mode == 2) {
      mr_CmdBeginRendering(cb, &ri); mr_CmdEndRendering(cb);
      barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
      att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
   }
   mr_CmdBeginRendering(cb, &ri);
   if (mode != 0) {
      VkViewport viewport = {0,0,(float)width,(float)height,0,1};
      VkRect2D scissor = {{0,0},{width,height}};
      mr_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      mr_CmdSetViewport(cb, 0, 1, &viewport);
      mr_CmdSetScissor(cb, 0, 1, &scissor);
      mr_CmdDraw(cb, 3, 1, 0, 0);
   }
   mr_CmdEndRendering(cb);
   TRY(EndCommandBuffer(cb) == VK_SUCCESS);
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   TRY(CreateFence(dev, &fi, NULL, &fence) == VK_SUCCESS);
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   TRY(QueueSubmit(queue, 1, &si, fence) == VK_SUCCESS);
   TRY(WaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull) == VK_SUCCESS);
   if (getenv("MALI_BACKEND") && !strcmp(getenv("MALI_BACKEND"), "fake")) { pass = 1; error = "fake-no-pixels"; goto done; }
   TRY(MapMemory(dev, bound, 0, VK_WHOLE_SIZE, 0, (void **)&mapped) == VK_SUCCESS && mapped);
   VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = bound, .size = VK_WHOLE_SIZE};
   TRY(InvalidateMappedMemoryRanges(dev, 1, &range) == VK_SUCCESS);
   dmamap = mmap(NULL, req.size, PROT_READ, MAP_SHARED, fd, 0);
   TRY(dmamap != MAP_FAILED);
   uint64_t middle = plane.offset + (height/2)*plane.rowPitch + (width/2)*4;
   TRY(middle + 16 <= req.size && plane.offset + 16 <= req.size);
   modrt_hex(mapped + plane.offset, a); modrt_hex(mapped + middle, b);
   modrt_hex(dmamap + plane.offset, c); modrt_hex(dmamap + middle, d);
   const uint8_t green[4] = {0,255,0,255};
   const uint8_t red[4] = {fmt == VK_FORMAT_B8G8R8A8_UNORM ? 0 : 255,0,fmt == VK_FORMAT_B8G8R8A8_UNORM ? 255 : 0,255};
   const uint8_t *want = mode ? red : green;
   pass = !memcmp(mapped + plane.offset, want, 4) && !memcmp(mapped + middle, want, 4) &&
          !memcmp(dmamap + plane.offset, want, 4) && !memcmp(dmamap + middle, want, 4);
done:
   printf("{\"test\":\"modrt-matrix\",\"tiling\":%d,\"imported\":%d,\"width\":%u,\"format\":%u,\"mode\":%d,"
          "\"offset\":%llu,\"pitch\":%llu,\"size\":%llu,\"map0\":\"%s\",\"mapmid\":\"%s\","
          "\"dma0\":\"%s\",\"dmamid\":\"%s\",\"result\":\"%s\",\"error\":\"%s\"}\n",
          tiling, imported, width, fmt, mode, (unsigned long long)plane.offset, (unsigned long long)plane.rowPitch,
          (unsigned long long)plane.size, a,b,c,d, error && !strcmp(error, "fake-no-pixels") ? "SKIP" : pass ? "PASS" : "FAIL", error ? error : "");
   fflush(stdout);
   if (dmamap != MAP_FAILED) munmap(dmamap, req.size);
   if (mapped) UnmapMemory(dev, bound);
   if (fence) DestroyFence(dev, fence, NULL);
   if (pool) DestroyCommandPool(dev, pool, NULL);
   if (view) mr_DestroyImageView(dev, view, NULL);
   if (image) DestroyImage(dev, image, NULL);
   if (bound && bound != owner) FreeMemory(dev, bound, NULL);
   if (owner) FreeMemory(dev, owner, NULL);
   if (import_fd >= 0) close(import_fd);
   if (fd >= 0) close(fd);
#undef TRY
   return pass;
}

static int
test_modrt_matrix(PFN_vkGetDeviceProcAddr gdpa)
{
#define MODRT_LOAD(n) mr_##n = (PFN_vk##n)gdpa(dev, "vk" #n); if (!mr_##n) { printf("missing vk" #n "\n"); return 0; }
   MODRT_FNS(MODRT_LOAD)
#undef MODRT_LOAD
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   if (CreatePipelineLayout(dev, &li, NULL, &layout) != VK_SUCCESS) return 0;
   int ok = 1;
   for (unsigned f = 0; f < 2; f++) {
      VkFormat fmt = f ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
      VkPipeline pipe = modrt_pipeline(fmt, layout);
      if (!pipe) { ok = 0; continue; }
      for (int t = 0; t < 3; t++)
         for (int imp = 0; imp < 2; imp++)
            for (unsigned w = 0; w < 2; w++)
               for (int mode = 0; mode < 3; mode++)
                  ok &= modrt_variant(t, imp, w ? 1558 : 1536, fmt, mode, pipe);
      mr_DestroyPipeline(dev, pipe, NULL);
   }
   DestroyPipelineLayout(dev, layout, NULL);
   return ok;
}
