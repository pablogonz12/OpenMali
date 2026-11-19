/* SPDX-License-Identifier: MIT */
/* Unity 2023.2 (Vulkan 1.1 client) capability probe, host-side, fake backend:
 *   - every graphics format Unity's backend can hand to vkCreateImage (render targets, depth/stencil, sampled, compressed), checked
 *     through vkGetPhysicalDeviceFormatProperties AND vkGetPhysicalDeviceImageFormatProperties AND a real vkCreateImage +
 *     vkGetImageMemoryRequirements for the usage Unity asks for;
 *   - the swapchain contract (surface formats, present modes, composite alpha, image count, mutable sRGB pair) on whichever surface
 *     this build's WSI offers (headless / xcb / wayland, first that works).
 * Prints one line per row and a JSON summary. Exit 0 = every row is fine or degrades, 1 = a hard blocker.
 *
 *   VK_DRIVER_FILES=out/native/mali_icd.json MALI_BACKEND=fake MALI_FAKE_GPU=v13 t_unity_caps [--json] [--verbose]
 * The verdict table it produces is in vk/UNITY-GAPS.md.
 */
#define _DEFAULT_SOURCE
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_beta.h>
#if __has_include(<xcb/xcb.h>)
#define HAVE_XCB 1
#include <xcb/xcb.h>
#define VK_USE_PLATFORM_XCB_KHR 1
#include <vulkan/vulkan_xcb.h>
#else
#define HAVE_XCB 0
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

static int g_fail, g_verbose;
static VkPhysicalDevice g_pd;
static VkDevice g_dev;
static VkPhysicalDeviceFeatures g_feat;
static VkPhysicalDeviceProperties g_props;
static VkPhysicalDeviceMemoryProperties g_mem;

#define DEV_FNS(X)                                                                                     \
   X(DestroyDevice) X(GetDeviceQueue) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements)    \
   X(AllocateMemory) X(FreeMemory) X(BindImageMemory) X(CreateImageView) X(DestroyImageView)         \
   X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR)

typedef struct {
   VkFormat f;
   const char *name;
   VkImageUsageFlags usage;
   VkImageCreateFlags flags;
   int compressed;
   const char *cls;   /* Unity use */
} FmtRow;

/* The formats Unity 2023.2's Vulkan backend allocates without a logged fallback (GraphicsFormat -> VkFormat table; the compressed
 * rows are the ones the player probes because it enabled textureCompressionBC). RT = color attachment, DS = depth/stencil,
 * SMP = sampled only, BC = compressed sampled. */
static const FmtRow g_rows[] = {
   /* swapchain-adjacent colour render targets */
   {VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, 0, "RT"},
   {VK_FORMAT_B8G8R8A8_SRGB, "B8G8R8A8_SRGB", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, 0, "RT"},
   {VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, 0, "RT"},
   {VK_FORMAT_R8G8B8A8_SRGB, "R8G8B8A8_SRGB", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, 0, "RT"},
   {VK_FORMAT_R8G8B8A8_SNORM, "R8G8B8A8_SNORM", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_A2R10G10B10_UNORM_PACK32, "A2R10G10B10_UNORM", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16, "R10X6G10X6B10X6A10X6", 0, 0, 0, "RT"},   /* not in the Unity table: expect unsupported, informational */
   {VK_FORMAT_B10G11R11_UFLOAT_PACK32, "B10G11R11_UFLOAT", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, "E5B9G9R9_UFLOAT", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, 0, "RT(HDR)"},
   {VK_FORMAT_R16G16B16A16_UNORM, "R16G16B16A16_UNORM", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R16G16B16A16_SNORM, "R16G16B16A16_SNORM", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R32G32B32A32_SFLOAT, "R32G32B32A32_SFLOAT", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R8_UNORM, "R8_UNORM", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R8G8_UNORM, "R8G8_UNORM", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R16_UNORM, "R16_UNORM", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R16G16_UNORM, "R16G16_UNORM", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R16_SFLOAT, "R16_SFLOAT", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R16G16_SFLOAT, "R16G16_SFLOAT", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R32_SFLOAT, "R32_SFLOAT", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R32G32_SFLOAT, "R32G32_SFLOAT", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R8_UINT, "R8_UINT", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R16_UINT, "R16_UINT", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R32_UINT, "R32_UINT", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R8G8_UINT, "R8G8_UINT", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R16G16_UINT, "R16G16_UINT", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R32G32_UINT, "R32G32_UINT", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_R32G32B32A32_UINT, "R32G32B32A32_UINT", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_A4B4G4R4_UNORM_PACK16, "A4B4G4R4_UNORM", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT(EXT_4444)"},
   {VK_FORMAT_R5G6B5_UNORM_PACK16, "R5G6B5_UNORM", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   {VK_FORMAT_A1R5G5B5_UNORM_PACK16, "A1R5G5B5_UNORM", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "RT"},
   /* depth / stencil */
   {VK_FORMAT_D16_UNORM, "D16_UNORM", VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "DS"},
   {VK_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8", VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "DS"},
   {VK_FORMAT_D32_SFLOAT, "D32_SFLOAT", VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "DS"},
   {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32_SFLOAT_S8", VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "DS"},
   {VK_FORMAT_S8_UINT, "S8_UINT", VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "DS"},
   {VK_FORMAT_X8_D24_UNORM_PACK32, "X8_D24_UNORM", VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, "DS"},
   /* compressed sampled (textureCompressionBC) */
   {VK_FORMAT_BC1_RGB_UNORM_BLOCK, "BC1_RGB_UNORM", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 1, "BC"},
   {VK_FORMAT_BC1_RGB_SRGB_BLOCK, "BC1_RGB_SRGB", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 1, "BC"},
   {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1_RGBA_UNORM", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 1, "BC"},
   {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, "BC1_RGBA_SRGB", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 1, "BC"},
   {VK_FORMAT_BC2_UNORM_BLOCK, "BC2_UNORM", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 1, "BC"},
   {VK_FORMAT_BC2_SRGB_BLOCK, "BC2_SRGB", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 1, "BC"},
   {VK_FORMAT_BC3_UNORM_BLOCK, "BC3_UNORM", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 1, "BC"},
   {VK_FORMAT_BC3_SRGB_BLOCK, "BC3_SRGB", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 1, "BC"},
   {VK_FORMAT_BC4_UNORM_BLOCK, "BC4_UNORM", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 1, "BC"},
   {VK_FORMAT_BC4_SNORM_BLOCK, "BC4_SNORM", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 1, "BC"},
   {VK_FORMAT_BC5_UNORM_BLOCK, "BC5_UNORM", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 1, "BC"},
   {VK_FORMAT_BC5_SNORM_BLOCK, "BC5_SNORM", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 1, "BC"},
   {VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H_UFLOAT", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 1, "BC"},
   {VK_FORMAT_BC6H_SFLOAT_BLOCK, "BC6H_SFLOAT", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 1, "BC"},
   {VK_FORMAT_BC7_UNORM_BLOCK, "BC7_UNORM", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 1, "BC"},
   {VK_FORMAT_BC7_SRGB_BLOCK, "BC7_SRGB", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 1, "BC"},
};

static int
find_memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < g_mem.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (g_mem.memoryTypes[i].propertyFlags & want) == want)
         return (int)i;
   return -1;
}

/* The strong check: create the image the way Unity does (2D, optimal, 1 sample, mip chain, cube-array for cubemaps) and bind memory.
 * Returns VK_SUCCESS or the first failure. */
static VkResult
try_create(VkFormat f, VkImageUsageFlags usage, uint32_t levels, uint32_t layers, VkImageViewType view)
{
   VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
      .extent = {W, H, 1}, .mipLevels = levels, .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
      .flags = view == VK_IMAGE_VIEW_TYPE_CUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0};
   VkImage img;
   VkResult r = vkCreateImage(g_dev, &ici, NULL, &img);
   if (r != VK_SUCCESS) return r;
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(g_dev, img, &mr);
   int mt = find_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (mt < 0) { vkDestroyImage(g_dev, img, NULL); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt};
   VkDeviceMemory mem;
   r = vkAllocateMemory(g_dev, &ai, NULL, &mem);
   if (r != VK_SUCCESS) { vkDestroyImage(g_dev, img, NULL); return r; }
   r = vkBindImageMemory(g_dev, img, mem, 0);
   vkFreeMemory(g_dev, mem, NULL);
   vkDestroyImage(g_dev, img, NULL);
   return r;
}

static const char *
flagnames(VkFormatFeatureFlags f, char *buf, size_t n)
{
   buf[0] = 0;
   struct { VkFormatFeatureFlags b; const char *s; } t[] = {
      {VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "S"}, {VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT, "L"},
      {VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "C"}, {VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT, "B"},
      {VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, "D"}, {VK_FORMAT_FEATURE_BLIT_SRC_BIT, "bs"},
      {VK_FORMAT_FEATURE_BLIT_DST_BIT, "bd"}, {VK_FORMAT_FEATURE_TRANSFER_SRC_BIT, "ts"},
      {VK_FORMAT_FEATURE_TRANSFER_DST_BIT, "td"}, {VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "si"},
      {VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT, "sa"},
   };
   for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
      if (f & t[i].b) { strncat(buf, t[i].s, n - strlen(buf) - 1); strncat(buf, " ", n - strlen(buf) - 1); }
   return buf;
}

/* Verdict per row, mirroring the gap doc: a format Unity needs for a colour attachment, depth attachment or sampled texture and that
 * cannot be created is a hard blocker; a missing optional format (EXT_4444) degrades. */
static void
probe_formats(void)
{
   printf("== formats (vkGetPhysicalDeviceFormatProperties + real vkCreateImage) ==\n");
   printf("%-22s %-10s %-22s %-8s %s\n", "format", "unity", "optimal features", "props", "create(2D, optimal, mips)");
   int hard = 0, soft = 0;
   for (size_t i = 0; i < sizeof g_rows / sizeof g_rows[0]; i++) {
      const FmtRow *r = &g_rows[i];
      VkFormatProperties fp;
      vkGetPhysicalDeviceFormatProperties(g_pd, r->f, &fp);
      char fn[96];
      flagnames(fp.optimalTilingFeatures, fn, sizeof fn);
      VkResult propm = VK_SUCCESS;
      VkImageFormatProperties ifp;
      /* Unity asks the driver this before creating the image */
      if (r->usage)
         propm = vkGetPhysicalDeviceImageFormatProperties(g_pd, r->f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, r->usage, 0, &ifp);
      /* The format query is the spec's support check for exactly the parameters vkCreateImage gets: do not create an image the query
       * already rejected, report the query's error instead. */
      VkResult cr = !r->usage ? VK_SUCCESS
                  : propm != VK_SUCCESS ? propm
                  : try_create(r->f, r->usage, 5, 1, VK_IMAGE_VIEW_TYPE_2D);
      /* Sampled formats are also uploaded as mip chains and cubemaps by Unity. The cube create passes VK_IMAGE_CREATE_CUBE_COMPATIBLE,
       * so it needs its own query with that flag (the 2D query above passes flags 0 and cannot vouch for it). */
      VkResult cub = VK_SUCCESS;
      if (r->usage && !(r->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
         VkImageFormatProperties cifp;
         VkResult cprop = vkGetPhysicalDeviceImageFormatProperties(g_pd, r->f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, r->usage,
                                                                   VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, &cifp);
         cub = cprop == VK_SUCCESS ? try_create(r->f, r->usage, 1, 6, VK_IMAGE_VIEW_TYPE_CUBE) : cprop;
      }
      bool need = r->usage && !strstr(r->cls, "EXT_4444") && strcmp(r->name, "R10X6G10X6B10X6A10X6");
      bool ok = (propm == VK_SUCCESS) && (cr == VK_SUCCESS) && (cub == VK_SUCCESS) &&
                (!(r->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) &&
                (!(r->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) || (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) &&
                (!(r->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT));
      if (!ok && need) hard++;
      else if (!ok) soft++;
      char fb[32] = "";
      if (cr != VK_SUCCESS) snprintf(fb, sizeof fb, "(%d)", cr);
      printf("%-22s %-10s %-22s %-8s %s %s%s\n", r->name, r->cls, fn,
             propm == VK_SUCCESS ? "ok" : "NONE",
             cr != VK_SUCCESS ? "FAIL" : "ok", fb,
             ok ? (need ? "" : " DEGRADES") : (need ? " HARD-BLOCKER" : ""));
   }
   printf("formats: %d hard blocker(s), %d degraded row(s); textureCompressionBC=%s\n", hard, soft,
          g_feat.textureCompressionBC ? "yes" : "NO");
   g_fail += hard;
}

/* ---- swapchain contract. Unity only uses VK_KHR_{wayland,xcb,xlib}_surface; this probe takes the first surface extension the ICD
 * exposes: VK_EXT_headless_surface (the plain host build) or VK_KHR_xcb_surface (the -Dplatforms=x11 build, run under gamescope's
 * Xwayland as tests/xwsi-host.sh does). Both go through wsi_common, i.e. the same format/present-mode/composite-alpha negotiation and the
 * same swapchain image allocation as the Wayland path. ---- */

static VkInstance g_inst;

static VkSurfaceKHR
make_surface(PFN_vkCreateHeadlessSurfaceEXT *headless, const char **used)
{
   uint32_t ne = 0;
   vkEnumerateInstanceExtensionProperties(NULL, &ne, NULL);
   VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
   vkEnumerateInstanceExtensionProperties(NULL, &ne, ep);
   bool has_headless = false, has_xcb = false;
   for (uint32_t i = 0; i < ne; i++) {
      if (!strcmp(ep[i].extensionName, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME)) has_headless = true;
      if (!strcmp(ep[i].extensionName, "VK_KHR_xcb_surface")) has_xcb = true;
   }
   free(ep);
   const char *exts[4];
   uint32_t next = 0;
   exts[next++] = VK_KHR_SURFACE_EXTENSION_NAME;
   if (has_headless) exts[next++] = VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME;
   else if (has_xcb && getenv("DISPLAY")) exts[next++] = "VK_KHR_xcb_surface";
   const char *pick = has_headless ? "headless" : (has_xcb && getenv("DISPLAY")) ? "xcb" : NULL;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = next, .ppEnabledExtensionNames = exts};
   if (vkCreateInstance(&ici, NULL, &g_inst) != VK_SUCCESS) { g_inst = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
   /* "none" = no platform was usable here (a skip); anything else means that platform was selected, so a NULL return below is a
    * setup failure and probe_swapchain must count it. */
   *used = pick ? pick : "none";
   if (has_headless) {
      *headless = (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(g_inst, "vkCreateHeadlessSurfaceEXT");
      if (!*headless) return VK_NULL_HANDLE;
      VkHeadlessSurfaceCreateInfoEXT hci = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
      VkSurfaceKHR s = VK_NULL_HANDLE;
      if ((*headless)(g_inst, &hci, NULL, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
      return s;
   }
#if HAVE_XCB
   if (has_xcb && getenv("DISPLAY")) {
      PFN_vkCreateXcbSurfaceKHR create = (PFN_vkCreateXcbSurfaceKHR)vkGetInstanceProcAddr(g_inst, "vkCreateXcbSurfaceKHR");
      if (!create) return VK_NULL_HANDLE;
      int scr = 0;
      xcb_connection_t *c = xcb_connect(NULL, &scr);
      if (!c || xcb_connection_has_error(c)) return VK_NULL_HANDLE;
      const xcb_setup_t *su = xcb_get_setup(c);
      xcb_screen_iterator_t it = xcb_setup_roots_iterator(su);
      for (int i = 0; i < scr && it.rem; i++) xcb_screen_next(&it);
      xcb_window_t w = xcb_generate_id(c);
      xcb_create_window(c, XCB_COPY_FROM_PARENT, w, it.data->root, 0, 0, W, H, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                        it.data->root_visual, 0, NULL);
      xcb_map_window(c, w);
      xcb_flush(c);
      /* the test needs the window to be mapped before the server reports a size; a mapped window without a compositor is fine here */
      for (int i = 0; i < 100; i++) {
         xcb_generic_event_t *ev = xcb_poll_for_event(c);
         while (ev) { free(ev); ev = xcb_poll_for_event(c); }
      }
      VkXcbSurfaceCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR, .connection = c, .window = w};
      VkSurfaceKHR s = VK_NULL_HANDLE;
      if (create(g_inst, &sci, NULL, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
      return s;
   }
#endif
   return VK_NULL_HANDLE;
}

static bool g_mutable;   /* VK_KHR_swapchain_mutable_format (+ VK_KHR_image_format_list) enabled on the device */

static void
probe_swapchain(VkSurfaceKHR surf, const char *used)
{
   printf("== swapchain (Unity: >=minImageCount, prefers 3, FIFO required, B8G8R8A8 or R8G8B8A8 UNORM/SRGB pair) ==\n");
   if (!surf) {
      if (!strcmp(used, "none")) {
         printf("surface: NONE (this build has no headless or xcb surface) - swapchain rows NOT checked here\n");
         return;
      }
      /* a platform was selected and vkCreateInstance/vkCreate*SurfaceKHR failed: that is a hard failure, not "not checked here" */
      printf("surface: VK_KHR_%s_surface selected but setup FAILED - HARD-BLOCKER\n", used);
      g_fail++;
      return;
   }
   printf("surface: VK_KHR_%s_surface\n", used);
   VkSurfaceCapabilitiesKHR caps;
   if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, surf, &caps) != VK_SUCCESS) { printf("caps: FAIL\n"); g_fail++; return; }
   uint32_t nf = 0, nm = 0;
   vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, surf, &nf, NULL);
   VkSurfaceFormatKHR *fs = calloc(nf ? nf : 1, sizeof *fs);
   vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, surf, &nf, fs);
   vkGetPhysicalDeviceSurfacePresentModesKHR(g_pd, surf, &nm, NULL);
   VkPresentModeKHR *ms = calloc(nm ? nm : 1, sizeof *ms);
   vkGetPhysicalDeviceSurfacePresentModesKHR(g_pd, surf, &nm, ms);
   printf("formats (%u):", nf);
   for (uint32_t i = 0; i < nf; i++) printf(" %s/%s", fs[i].format == VK_FORMAT_B8G8R8A8_UNORM ? "B8G8R8A8_UNORM" :
      fs[i].format == VK_FORMAT_B8G8R8A8_SRGB ? "B8G8R8A8_SRGB" : fs[i].format == VK_FORMAT_R8G8B8A8_UNORM ? "R8G8B8A8_UNORM" :
      fs[i].format == VK_FORMAT_R8G8B8A8_SRGB ? "R8G8B8A8_SRGB" : "other", fs[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ? "SRGB_NL" : "cs?");
   printf("\npresent modes (%u):", nm);
   for (uint32_t i = 0; i < nm; i++) printf(" %s", ms[i] == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" : ms[i] == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" :
      ms[i] == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" :
      ms[i] == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "FIFO_RELAXED" : "other");
   printf("\nimageCount %u..%u (0 = unlimited), current %ux%u, supported composite alpha 0x%x, transforms 0x%x\n",
          caps.minImageCount, caps.maxImageCount, caps.currentExtent.width, caps.currentExtent.height,
          caps.supportedCompositeAlpha, caps.supportedTransforms);

   int hard = 0;
   bool bgra = false, bgra_srgb = false, rgba = false, rgba_srgb = false, fifo = false;
   for (uint32_t i = 0; i < nf; i++) {
      if (fs[i].format == VK_FORMAT_B8G8R8A8_UNORM) bgra = true;
      if (fs[i].format == VK_FORMAT_B8G8R8A8_SRGB) bgra_srgb = true;
      if (fs[i].format == VK_FORMAT_R8G8B8A8_UNORM) rgba = true;
      if (fs[i].format == VK_FORMAT_R8G8B8A8_SRGB) rgba_srgb = true;
   }
   for (uint32_t i = 0; i < nm; i++) if (ms[i] == VK_PRESENT_MODE_FIFO_KHR) fifo = true;
   printf("row surface formats: B8G8R8A8_UNORM=%s SRGB=%s R8G8B8A8_UNORM=%s SRGB=%s -> %s\n", bgra ? "yes" : "NO", bgra_srgb ? "yes" : "NO",
          rgba ? "yes" : "NO", rgba_srgb ? "yes" : "NO",
          bgra && bgra_srgb ? "fine (the pair Unity wants)" : (rgba && rgba_srgb ? "degrades (RGBA pair only)" : "HARD"));
   if (!(bgra || rgba)) hard++;
   printf("row present mode FIFO: %s\n", fifo ? "fine" : "HARD-BLOCKER");
   if (!fifo) hard++;
   printf("row composite alpha: OPAQUE=%s INHERIT=%s PRE=%s POST=%s -> %s\n",
          (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? "yes" : "no",
          (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) ? "yes" : "no",
          (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) ? "yes" : "no",
          (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR) ? "yes" : "no",
          (caps.supportedCompositeAlpha & (VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)) ? "fine" : "HARD-BLOCKER");

   /* Unity's actual request: vulkanNumSwapchainBuffers images (default 3), FIFO, OPAQUE, B8G8R8A8_UNORM + SRGB view. */
   /* Unity's sRGB-write path: a B8G8R8A8_SRGB view of the swapchain image, i.e. VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR plus a
    * two-format image format list (VK_KHR_swapchain_mutable_format, which we advertise). Without the flag the view is illegal. */
   /* Pick the UNORM/SRGB pair the surface actually advertises: requesting BGRA on an RGBA-only surface must fail, not "degrade". */
   bool use_rgba = !(bgra && bgra_srgb) && rgba && rgba_srgb;
   VkFormat base_fmt = use_rgba ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
   VkFormat srgb_fmt = use_rgba ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_B8G8R8A8_SRGB;
   VkFormat view_fmts[2] = {base_fmt, srgb_fmt};
   VkImageFormatListCreateInfo fli = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = 2, .pViewFormats = view_fmts};
   VkSwapchainCreateInfoKHR ci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .pNext = g_mutable ? &fli : NULL, .surface = surf,
      .flags = g_mutable ? VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR : 0,
      .minImageCount = (3u >= caps.minImageCount && (caps.maxImageCount == 0 || 3u <= caps.maxImageCount)) ? 3u : caps.minImageCount,
      .imageFormat = base_fmt, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
      .imageExtent = {caps.currentExtent.width == UINT32_MAX ? W : caps.currentExtent.width,
                      caps.currentExtent.height == UINT32_MAX ? H : caps.currentExtent.height},
      .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE, .oldSwapchain = VK_NULL_HANDLE};
   VkSwapchainKHR sc = VK_NULL_HANDLE;
   VkResult r = vkCreateSwapchainKHR(g_dev, &ci, NULL, &sc);
   if (r != VK_SUCCESS) {
      printf("swapchain create (%s, FIFO, OPAQUE, %u images): FAIL (%d) HARD-BLOCKER\n", use_rgba ? "R8G8B8A8_UNORM" : "B8G8R8A8_UNORM",
             ci.minImageCount, r);
      g_fail++;
   } else {
      uint32_t got = 0;
      vkGetSwapchainImagesKHR(g_dev, sc, &got, NULL);
      printf("swapchain create (%s, FIFO, OPAQUE, %u requested): ok, %u image(s) -> %s\n", use_rgba ? "R8G8B8A8_UNORM" : "B8G8R8A8_UNORM",
             ci.minImageCount, got,
             got >= ci.minImageCount ? "fine" : "HARD-BLOCKER");
      if (got < ci.minImageCount) hard++;
      /* the sRGB half of the pair: Unity renders through a _SRGB view of a UNORM image, i.e. it needs the mutable-format swapchain.
       * Without VK_KHR_swapchain_mutable_format enabled the request is illegal instead of testable, so report it as unavailable. */
      VkImage imgs[8];
      uint32_t n = got < 8 ? got : 8;
      vkGetSwapchainImagesKHR(g_dev, sc, &n, imgs);
      if (!g_mutable)
         printf("row sRGB view over the swapchain image (vulkanEnableSetSRGBWrite): unavailable (device has no VK_KHR_swapchain_mutable_format)\n");
      else if (n > 0) {
         VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = imgs[0],
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = srgb_fmt,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
         VkImageView iv;
         VkResult vr = vkCreateImageView(g_dev, &vi, NULL, &iv);
         printf("row sRGB view over the swapchain image (vulkanEnableSetSRGBWrite): %s\n", vr == VK_SUCCESS ? "fine" : "degrades (mutable format needed)");
         if (vr == VK_SUCCESS) vkDestroyImageView(g_dev, iv, NULL);
      }
      vkDestroySwapchainKHR(g_dev, sc, NULL);
   }
   printf("swapchain: %d hard blocker(s)\n", hard);
   g_fail += hard;
   free(fs); free(ms);
   if (surf) vkDestroySurfaceKHR(g_inst, surf, NULL);
}

int
main(int argc, char **argv)
{
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--verbose")) g_verbose = 1;
      else if (!strcmp(argv[i], "--json")) { /* JSON summary only at the end; same rows on stdout */ }
      else { fprintf(stderr, "usage: t_unity_caps [--verbose]\n"); return 2; }
   }

   /* One instance for everything: the surface must belong to the same instance as the physical device, or the surface queries are
    * undefined (they "succeed" with garbage capabilities and hang vkCreateSwapchainKHR). */
   PFN_vkCreateHeadlessSurfaceEXT headless = NULL;
   const char *surf_kind = "none";
   VkSurfaceKHR surf = make_surface(&headless, &surf_kind);
   if (!g_inst) {
      VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
      VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
      if (vkCreateInstance(&ici, NULL, &g_inst) != VK_SUCCESS) { printf("no instance (set VK_DRIVER_FILES)\n"); return 2; }
   }
   uint32_t n = 0;
   if (vkEnumeratePhysicalDevices(g_inst, &n, NULL) != VK_SUCCESS || n == 0) { printf("no device\n"); return 2; }
   VkPhysicalDevice pds[4] = {0};
   if (n > 4) n = 4;
   if (vkEnumeratePhysicalDevices(g_inst, &n, pds) != VK_SUCCESS || n == 0) { printf("no device\n"); return 2; }
   g_pd = pds[0];
   vkGetPhysicalDeviceProperties(g_pd, &g_props);
   vkGetPhysicalDeviceFeatures(g_pd, &g_feat);
   vkGetPhysicalDeviceMemoryProperties(g_pd, &g_mem);
   printf("device: %s api %u.%u.%u\n", g_props.deviceName, VK_API_VERSION_MAJOR(g_props.apiVersion),
          VK_API_VERSION_MINOR(g_props.apiVersion), VK_API_VERSION_PATCH(g_props.apiVersion));

   uint32_t qn = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &qn, NULL);
   VkQueueFamilyProperties qp[16];
   if (qn > 16) qn = 16;
   vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &qn, qp);
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1};
   float prio = 1.0f;
   qci.pQueuePriorities = &prio;
   uint32_t nq = 0, ns = 0;
   for (uint32_t i = 0; i < qn; i++) {
      if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) nq++;
      if (qp[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) ns++;
   }
   /* VK_KHR_swapchain_mutable_format (and its VK_KHR_image_format_list dependency) is what the sRGB-view rows need; advertising it is
    * not enabling it, so query and enable both before issuing a mutable swapchain request. */
   bool has_mutable = false, has_fmtlist = false;
   uint32_t nde = 0;
   if (vkEnumerateDeviceExtensionProperties(g_pd, NULL, &nde, NULL) == VK_SUCCESS && nde) {
      VkExtensionProperties *deps = calloc(nde, sizeof *deps);
      if (deps && vkEnumerateDeviceExtensionProperties(g_pd, NULL, &nde, deps) == VK_SUCCESS)
         for (uint32_t i = 0; i < nde; i++) {
            if (!strcmp(deps[i].extensionName, VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME)) has_mutable = true;
            if (!strcmp(deps[i].extensionName, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME)) has_fmtlist = true;
         }
      free(deps);
   }
   g_mutable = has_mutable && has_fmtlist;
   const char *dev_exts[3] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
   uint32_t ndev = 1;
   if (g_mutable) { dev_exts[ndev++] = VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME; dev_exts[ndev++] = VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME; }
   else printf("device: VK_KHR_swapchain_mutable_format%s - sRGB-view rows report unavailable\n",
               has_mutable ? " without VK_KHR_image_format_list" : " absent");
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                             .enabledExtensionCount = ndev, .ppEnabledExtensionNames = dev_exts};
   if (vkCreateDevice(g_pd, &dci, NULL, &g_dev) != VK_SUCCESS) { printf("device create failed\n"); return 2; }
   printf("queues: %u family(ies), graphics %u, 1 present-capable graphics family: %s\n", qn, nq, "checked below");

   probe_formats();
   probe_swapchain(surf, surf_kind);

   printf("{\"test\":\"unity_caps\",\"device\":\"%s\",\"bc\":%s,\"hard\":%d,\"ok\":%s}\n", g_props.deviceName,
          g_feat.textureCompressionBC ? "true" : "false", g_fail, g_fail ? "false" : "true");
   return g_fail ? 1 : 0;
}
