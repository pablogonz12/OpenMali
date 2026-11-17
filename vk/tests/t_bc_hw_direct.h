/* generated: direct ICD loader shim for t_bc_hw (bionic, no loader). force-included */
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
static PFN_vkGetInstanceProcAddr g_gipa;
static PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
static PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
static PFN_vkAllocateMemory vkAllocateMemory;
static PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
static PFN_vkBindBufferMemory vkBindBufferMemory;
static PFN_vkBindImageMemory vkBindImageMemory;
static PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets;
static PFN_vkCmdBindPipeline vkCmdBindPipeline;
static PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;
static PFN_vkCmdDispatch vkCmdDispatch;
static PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
static PFN_vkCmdPushConstants vkCmdPushConstants;
static PFN_vkCreateBuffer vkCreateBuffer;
static PFN_vkCreateCommandPool vkCreateCommandPool;
static PFN_vkCreateComputePipelines vkCreateComputePipelines;
static PFN_vkCreateDescriptorPool vkCreateDescriptorPool;
static PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
static PFN_vkCreateDevice vkCreateDevice;
static PFN_vkCreateImage vkCreateImage;
static PFN_vkCreateImageView vkCreateImageView;
static PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
static PFN_vkCreateSampler vkCreateSampler;
static PFN_vkCreateShaderModule vkCreateShaderModule;
static PFN_vkDestroyImage vkDestroyImage;
static PFN_vkDestroyImageView vkDestroyImageView;
static PFN_vkEndCommandBuffer vkEndCommandBuffer;
static PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
static PFN_vkFreeCommandBuffers vkFreeCommandBuffers;
static PFN_vkFreeDescriptorSets vkFreeDescriptorSets;
static PFN_vkFreeMemory vkFreeMemory;
static PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
static PFN_vkGetDeviceQueue vkGetDeviceQueue;
static PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
static PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures;
static PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
static PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkMapMemory vkMapMemory;
static PFN_vkQueueSubmit vkQueueSubmit;
static PFN_vkQueueWaitIdle vkQueueWaitIdle;
static PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets;
static VkResult mali_direct_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *i) {
 const char *p=getenv("BC_ICD"); void *l=dlopen(p?p:"libvulkan_mali.so",RTLD_NOW); if(!l){printf("FAIL dlopen %s\n",dlerror());exit(1);}
 g_gipa=(PFN_vkGetInstanceProcAddr)dlsym(l,"vk_icdGetInstanceProcAddr"); uint32_t v=5; VkResult(*ng)(uint32_t*)=dlsym(l,"vk_icdNegotiateLoaderICDInterfaceVersion"); if(ng) ng(&v);
 PFN_vkCreateInstance ci_=(PFN_vkCreateInstance)g_gipa(NULL,"vkCreateInstance"); VkResult r=ci_(ci,a,i); if(r) return r;
 vkAllocateCommandBuffers=(PFN_vkAllocateCommandBuffers)g_gipa(*i,"vkAllocateCommandBuffers");
 vkAllocateDescriptorSets=(PFN_vkAllocateDescriptorSets)g_gipa(*i,"vkAllocateDescriptorSets");
 vkAllocateMemory=(PFN_vkAllocateMemory)g_gipa(*i,"vkAllocateMemory");
 vkBeginCommandBuffer=(PFN_vkBeginCommandBuffer)g_gipa(*i,"vkBeginCommandBuffer");
 vkBindBufferMemory=(PFN_vkBindBufferMemory)g_gipa(*i,"vkBindBufferMemory");
 vkBindImageMemory=(PFN_vkBindImageMemory)g_gipa(*i,"vkBindImageMemory");
 vkCmdBindDescriptorSets=(PFN_vkCmdBindDescriptorSets)g_gipa(*i,"vkCmdBindDescriptorSets");
 vkCmdBindPipeline=(PFN_vkCmdBindPipeline)g_gipa(*i,"vkCmdBindPipeline");
 vkCmdCopyBufferToImage=(PFN_vkCmdCopyBufferToImage)g_gipa(*i,"vkCmdCopyBufferToImage");
 vkCmdDispatch=(PFN_vkCmdDispatch)g_gipa(*i,"vkCmdDispatch");
 vkCmdPipelineBarrier=(PFN_vkCmdPipelineBarrier)g_gipa(*i,"vkCmdPipelineBarrier");
 vkCmdPushConstants=(PFN_vkCmdPushConstants)g_gipa(*i,"vkCmdPushConstants");
 vkCreateBuffer=(PFN_vkCreateBuffer)g_gipa(*i,"vkCreateBuffer");
 vkCreateCommandPool=(PFN_vkCreateCommandPool)g_gipa(*i,"vkCreateCommandPool");
 vkCreateComputePipelines=(PFN_vkCreateComputePipelines)g_gipa(*i,"vkCreateComputePipelines");
 vkCreateDescriptorPool=(PFN_vkCreateDescriptorPool)g_gipa(*i,"vkCreateDescriptorPool");
 vkCreateDescriptorSetLayout=(PFN_vkCreateDescriptorSetLayout)g_gipa(*i,"vkCreateDescriptorSetLayout");
 vkCreateDevice=(PFN_vkCreateDevice)g_gipa(*i,"vkCreateDevice");
 vkCreateImage=(PFN_vkCreateImage)g_gipa(*i,"vkCreateImage");
 vkCreateImageView=(PFN_vkCreateImageView)g_gipa(*i,"vkCreateImageView");
 vkCreatePipelineLayout=(PFN_vkCreatePipelineLayout)g_gipa(*i,"vkCreatePipelineLayout");
 vkCreateSampler=(PFN_vkCreateSampler)g_gipa(*i,"vkCreateSampler");
 vkCreateShaderModule=(PFN_vkCreateShaderModule)g_gipa(*i,"vkCreateShaderModule");
 vkDestroyImage=(PFN_vkDestroyImage)g_gipa(*i,"vkDestroyImage");
 vkDestroyImageView=(PFN_vkDestroyImageView)g_gipa(*i,"vkDestroyImageView");
 vkEndCommandBuffer=(PFN_vkEndCommandBuffer)g_gipa(*i,"vkEndCommandBuffer");
 vkEnumeratePhysicalDevices=(PFN_vkEnumeratePhysicalDevices)g_gipa(*i,"vkEnumeratePhysicalDevices");
 vkFreeCommandBuffers=(PFN_vkFreeCommandBuffers)g_gipa(*i,"vkFreeCommandBuffers");
 vkFreeDescriptorSets=(PFN_vkFreeDescriptorSets)g_gipa(*i,"vkFreeDescriptorSets");
 vkFreeMemory=(PFN_vkFreeMemory)g_gipa(*i,"vkFreeMemory");
 vkGetBufferMemoryRequirements=(PFN_vkGetBufferMemoryRequirements)g_gipa(*i,"vkGetBufferMemoryRequirements");
 vkGetDeviceQueue=(PFN_vkGetDeviceQueue)g_gipa(*i,"vkGetDeviceQueue");
 vkGetImageMemoryRequirements=(PFN_vkGetImageMemoryRequirements)g_gipa(*i,"vkGetImageMemoryRequirements");
 vkGetPhysicalDeviceFeatures=(PFN_vkGetPhysicalDeviceFeatures)g_gipa(*i,"vkGetPhysicalDeviceFeatures");
 vkGetPhysicalDeviceFormatProperties=(PFN_vkGetPhysicalDeviceFormatProperties)g_gipa(*i,"vkGetPhysicalDeviceFormatProperties");
 vkGetPhysicalDeviceMemoryProperties=(PFN_vkGetPhysicalDeviceMemoryProperties)g_gipa(*i,"vkGetPhysicalDeviceMemoryProperties");
 vkGetPhysicalDeviceProperties=(PFN_vkGetPhysicalDeviceProperties)g_gipa(*i,"vkGetPhysicalDeviceProperties");
 vkGetPhysicalDeviceQueueFamilyProperties=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)g_gipa(*i,"vkGetPhysicalDeviceQueueFamilyProperties");
 vkMapMemory=(PFN_vkMapMemory)g_gipa(*i,"vkMapMemory");
 vkQueueSubmit=(PFN_vkQueueSubmit)g_gipa(*i,"vkQueueSubmit");
 vkQueueWaitIdle=(PFN_vkQueueWaitIdle)g_gipa(*i,"vkQueueWaitIdle");
 vkUpdateDescriptorSets=(PFN_vkUpdateDescriptorSets)g_gipa(*i,"vkUpdateDescriptorSets");
 return VK_SUCCESS; }
#define vkCreateInstance mali_direct_CreateInstance
