/* SPDX-License-Identifier: MIT. Offscreen vertex+fragment triangle with a vertex-colour draw; used to make PanVK create a tiler heap. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "spv.h"
#define W 256
#define H 256
#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s failed %d line %d\n", #x, r_, __LINE__); return 1; } } while (0)
int main(int argc, char **argv)
{
	void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (!lib) { fprintf(stderr, "no libvulkan\n"); return 1; }
	PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
	VkInstance inst = 0;
	PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(0, "vkCreateInstance");
	VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
	CK(ci(&ici, 0, &inst));
	G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceProperties); G(vkGetPhysicalDeviceMemoryProperties); G(vkCreateDevice);
	G(vkGetDeviceProcAddr); G(vkGetPhysicalDeviceQueueFamilyProperties);
	uint32_t n = 1; VkPhysicalDevice pd;
	VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd);
	if (r != VK_SUCCESS && r != VK_INCOMPLETE) { fprintf(stderr, "enum %d\n", r); return 1; }
	if (!n) { fprintf(stderr, "no devices\n"); return 1; }
	VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
	fprintf(stderr, "device: %s\n", pp.deviceName);
	VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	float prio = 1;
	VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi };
	VkDevice dev; CK(vkCreateDevice(pd, &dci, 0, &dev));
#define D(n) PFN_##n n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
	D(vkGetDeviceQueue); D(vkCmdBindVertexBuffers); D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkAllocateMemory); D(vkBindImageMemory); D(vkCreateImageView);
	D(vkCreateRenderPass); D(vkCreateFramebuffer); D(vkCreateShaderModule); D(vkCreatePipelineLayout); D(vkCreateGraphicsPipelines);
	D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkEndCommandBuffer); D(vkCmdBeginRenderPass);
	D(vkCmdEndRenderPass); D(vkCmdBindPipeline); D(vkCmdDraw); D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkCreateBuffer);
	D(vkGetBufferMemoryRequirements); D(vkBindBufferMemory); D(vkCmdCopyImageToBuffer); D(vkMapMemory); D(vkDeviceWaitIdle);
	VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
	VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
	VkImage img; CK(vkCreateImage(dev, &ii, 0, &img));
	VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
	uint32_t mt = 0; for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt = i; break; }
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt };
	VkDeviceMemory mem; CK(vkAllocateMemory(dev, &mai, 0, &mem)); CK(vkBindImageMemory(dev, img, mem, 0));
	VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = ii.format,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	VkImageView view; CK(vkCreateImageView(dev, &vi, 0, &view));
	VkAttachmentDescription ad = { .format = ii.format, .samples = 1, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
	VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar };
	VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sp };
	VkRenderPass rp; CK(vkCreateRenderPass(dev, &rpi, 0, &rp));
	VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1 };
	VkFramebuffer fb; CK(vkCreateFramebuffer(dev, &fi, 0, &fb));
	VkShaderModuleCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	VkShaderModule vs, fs;
	int var = argc > 1 && argv[1][0] == 'v';
	si.codeSize = pos_vert_spv_len; si.pCode = (const uint32_t *)pos_vert_spv; CK(vkCreateShaderModule(dev, &si, 0, &vs));
	if (var) { si.codeSize = var_frag_spv_len; si.pCode = (const uint32_t *)var_frag_spv; } else { si.codeSize = tri_frag_spv_len; si.pCode = (const uint32_t *)tri_frag_spv; } CK(vkCreateShaderModule(dev, &si, 0, &fs));
	VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pli, 0, &pl));
	VkPipelineShaderStageCreateInfo st[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
	VkVertexInputBindingDescription vbd[2] = { { 0, 16, VK_VERTEX_INPUT_RATE_VERTEX }, { 1, 16, VK_VERTEX_INPUT_RATE_VERTEX } };
	VkVertexInputAttributeDescription vad[2] = { { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 }, { 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0 } };
	VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 2,
		.pVertexBindingDescriptions = vbd, .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = vad };
	VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkViewport vp = { 0, 0, W, H, 0, 1 }; VkRect2D sc = { { 0, 0 }, { W, H } };
	VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
	VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1 };
	VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = 1 };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 15 };
	VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
	VkGraphicsPipelineCreateInfo gp = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vin,
		.pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl, .renderPass = rp };
	VkPipeline pipe; CK(vkCreateGraphicsPipelines(dev, 0, 1, &gp, 0, &pipe));
	VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer buf; CK(vkCreateBuffer(dev, &bi, 0, &buf));
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { mt = i; break; }
	mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
	VkDeviceMemory bm; CK(vkAllocateMemory(dev, &mai, 0, &bm)); CK(vkBindBufferMemory(dev, buf, bm, 0));
	VkBufferCreateInfo vbi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 96, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT };
	VkBuffer vbuf; CK(vkCreateBuffer(dev, &vbi, 0, &vbuf));
	vkGetBufferMemoryRequirements(dev, vbuf, &mr);
	mai.allocationSize = mr.size;
	VkDeviceMemory vm; CK(vkAllocateMemory(dev, &mai, 0, &vm)); CK(vkBindBufferMemory(dev, vbuf, vm, 0));
	float *vp_; CK(vkMapMemory(dev, vm, 0, 96, 0, (void **)&vp_));
	static const float pos[12] = { -0.5f, -0.5f, 0, 1, 0.5f, -0.5f, 0, 1, 0, 0.5f, 0, 1 };
	static const float col[12] = { 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1 };
	memcpy(vp_, pos, 48); memcpy(vp_ + 12, col, 48);
	VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpi, 0, &cp));
	VkCommandBufferAllocateInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cmd; CK(vkAllocateCommandBuffers(dev, &cbi, &cmd));
	VkCommandBufferBeginInfo bgi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CK(vkBeginCommandBuffer(cmd, &bgi));
	VkClearValue cv = { .color = { { 16 / 255.f, 32 / 255.f, 64 / 255.f, 128 / 255.f } } };
	VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1, .pClearValues = &cv };
	vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	VkBuffer vbs[2] = { vbuf, vbuf }; VkDeviceSize vo[2] = { 0, 48 };
	vkCmdBindVertexBuffers(cmd, 0, 2, vbs, vo);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
	VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
	vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);
	CK(vkEndCommandBuffer(cmd));
	VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
	CK(vkQueueSubmit(q, 1, &sub, 0)); CK(vkQueueWaitIdle(q));
	unsigned char *p; CK(vkMapMemory(dev, bm, 0, W * H * 4, 0, (void **)&p));
	unsigned cnt = 0; for (int i = 0; i < W * H; i++) if (p[i * 4] != 16 || p[i * 4 + 1] != 32 || p[i * 4 + 2] != 64 || p[i * 4 + 3] != 128) cnt++;
	printf("{\"device\":\"%s\",\"covered\":%u,\"expected\":%d,\"center\":[%u,%u,%u]}\n", pp.deviceName, cnt, 8192,
	       p[(128 * W + 128) * 4], p[(128 * W + 128) * 4 + 1], p[(128 * W + 128) * 4 + 2]);
	return 0;
}
