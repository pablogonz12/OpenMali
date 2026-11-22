/* SPDX-License-Identifier: MIT */
/* RUNG 2 state sweep: every advertised SAMPLER state combination and every blending / depth / stencil state this
 * driver advertises, drawn in 8x8 tiles of one 256x128 target, each case compared against a CPU reference that uses
 * integer arithmetic only:
 *   - texel coordinates are carried in 1/8 texel units and every texture channel is a multiple of 8, so a weighted
 *     sum with eighth-weights divides exactly and no rounding rule has to be guessed;
 *   - blend arithmetic is done in units of 1/255: 255 is odd, so no sum/product of UNORM8 values is ever a .5 tie.
 *
 *   phone : VK_DRIVER_FILES=... t_state_sweep --direct libvulkan_mali.so --spv-dir vk/shader
 *   fake  : MALI_BACKEND=fake t_state_sweep --direct out/native/libvulkan_mali.so   (records nothing: every case SKIP)
 *
 * One line per case:  case=<id> PASS|FAIL|SKIP expected=... got=...
 * Exit 0 = every case PASS or SKIP, 1 = at least one FAIL, 2 = usage/init error.
 *
 * Why: a skinned character renders as a flat saturated colour while the world is textured. What a character draw has
 * that a wall does not is sampler state (wrap / filter / mip selection / anisotropy / border) and per-draw state (blend
 * equation and factors, colour write mask, depth test/write/compare/clamp, stencil). One wrongly packed word in the
 * sampler descriptor (mali_sampler.c) or the blend descriptor (mali_cs_emit.c apply_blend) gives a wrong colour with no
 * shader bug at all.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define TW 8        /* tile edge, px */
#define GRID 32     /* tiles per row */
#define NROWS 16    /* 512 slots */
#define NCASE (GRID * NROWS)
#define FW (GRID * TW)   /* 256 */
#define FH (NROWS * TW)  /* 128 */

#define TEXN 8
#define NMIP 3
#define MAXLVL (NMIP - 1)
#define D24MAX 16777215u

static int g_fail, g_fake, g_ncase, g_alt_formula, g_blend_1lsb;
static int g_has_clip, g_has_cb, g_has_mc;   /* device extensions this run really enabled */
static const char *g_only, *g_spvdir = "shader", *g_ppm;

/* ---------------------------------------------------------------- device */
static PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties_;
#define DEV_FNS(X)                                                                                                          \
   X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) X(CreateImageView)                       \
   X(DestroyImageView) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory)                 \
   X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) X(CreateShaderModule) X(DestroyShaderModule)                \
   X(CreateSampler) X(DestroySampler) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout)                           \
   X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets)                      \
   X(CreatePipelineLayout) X(CreateGraphicsPipelines) X(CreateRenderPass) X(CreateFramebuffer) X(CreateCommandPool)        \
   X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBeginRenderPass) X(CmdEndRenderPass)           \
   X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDraw) X(CmdSetViewport) X(CmdSetScissor)           \
   X(CmdPipelineBarrier) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(QueueSubmit) X(CreateFence) X(WaitForFences)    \
   X(DeviceWaitIdle) X(GetDeviceQueue)
#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)
#undef DECL

static VkDevice dev;
static VkQueue queue;
static VkPhysicalDevice pd;
static VkPhysicalDeviceFeatures g_feat;
static VkPhysicalDeviceProperties g_props;

static void
die(const char *m)
{
   fprintf(stderr, "t_state_sweep: %s\n", m);
   exit(2);
}

static uint32_t
find_mem(uint32_t bits, VkMemoryPropertyFlags want)
{
   VkPhysicalDeviceMemoryProperties mp;
   GetPhysicalDeviceMemoryProperties_(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   return UINT32_MAX;
}

static VkImage
make_image(VkFormat fmt, uint32_t w, uint32_t h, uint32_t mips, VkImageUsageFlags usage, VkDeviceMemory *mem)
{
   VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                           .format = fmt, .extent = {w, h, 1}, .mipLevels = mips, .arrayLayers = 1,
                           .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VkImage img;
   if (CreateImage(dev, &ci, NULL, &img) != VK_SUCCESS)
      die("vkCreateImage");
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, img, &mr);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
   if (ai.memoryTypeIndex == UINT32_MAX || AllocateMemory(dev, &ai, NULL, mem) != VK_SUCCESS)
      die("image memory");
   if (BindImageMemory(dev, img, *mem, 0) != VK_SUCCESS)
      die("vkBindImageMemory");
   return img;
}

static VkBuffer
make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory *mem, void **map)
{
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer b;
   if (CreateBuffer(dev, &bi, NULL, &b) != VK_SUCCESS)
      die("vkCreateBuffer");
   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(dev, b, &mr);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
   if (ai.memoryTypeIndex == UINT32_MAX || AllocateMemory(dev, &ai, NULL, mem) != VK_SUCCESS)
      die("buffer memory");
   if (BindBufferMemory(dev, b, *mem, 0) != VK_SUCCESS)
      die("vkBindBufferMemory");
   if (map && MapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS)
      die("vkMapMemory");
   return b;
}

static VkShaderModule
load_spv(const char *name)
{
   char path[512];
   snprintf(path, sizeof path, "%s/%s", g_spvdir, name);
   FILE *f = fopen(path, "rb");
   if (!f) {
      fprintf(stderr, "t_state_sweep: cannot open %s (--spv-dir)\n", path);
      exit(2);
   }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   void *code = malloc((size_t)n);
   if (!code || fread(code, 1, (size_t)n, f) != (size_t)n)
      die("shader read");
   fclose(f);
   VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                  .codeSize = (size_t)n, .pCode = code};
   VkShaderModule m;
   if (CreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS)
      die("vkCreateShaderModule");
   free(code);
   return m;
}

/* ---------------------------------------------------------------- textures */
struct texdata {
   const char *name;
   uint8_t mip[NMIP][TEXN * TEXN * 4];   /* row stride TEXN*4 in every level (only w columns used) */
};

static struct texdata T_PAT = {.name = "pat"}, T_BLK = {.name = "blk"}, T_FLAT = {.name = "flat"};

static void
fill_tex(struct texdata *t)
{
   memset(t->mip, 0, sizeof t->mip);
   for (int n = 0; n < NMIP; n++) {
      int w = TEXN >> n;
      for (int y = 0; y < w; y++)
         for (int x = 0; x < w; x++) {
            uint8_t *p = &t->mip[n][(y * TEXN + x) * 4];
            if (t == &T_FLAT) {
               p[0] = n == 0 ? 255 : 0;
               p[1] = n == 1 ? 255 : 0;
               p[2] = n == 2 ? 255 : 0;
            } else if (t == &T_PAT) {
               /* (R,G) unique per texel of level 0; every channel is a multiple of 8 */
               p[0] = (uint8_t)(8 * ((n == 0 ? x : (n == 1 ? 2 * x + 1 : 2 * x + 1)) & 7));
               p[1] = (uint8_t)(8 * ((n == 0 ? y : (n == 1 ? 2 * y + 3 : 2 * y + 1)) & 7));
               p[2] = (uint8_t)(8 * ((x * 3 + y * 5) & 7));
            } else {   /* T_BLK: constant inside every 2x2 block of level 0; the matching mip texel holds the same */
               int bi = n == 0 ? x / 2 : x, bj = n == 0 ? y / 2 : y;
               p[0] = (uint8_t)(8 * ((bi * 3) & 7));
               p[1] = (uint8_t)(8 * ((bj * 5) & 7));
               p[2] = (uint8_t)(8 * ((bi + bj) & 7));
            }
            p[3] = 255;
         }
   }
}

/* 1: treat MIRROR_CLAMP_TO_EDGE as CLAMP_TO_EDGE (what Intel LNL does; the spec says mirror once then clamp) */
static int g_mclamp_clamp;   /* sweeps accept either formula; the PASS line names which one matched */

static int
floordiv(int a, int b)
{
   int q = a / b;
   if ((a % b) != 0 && ((a < 0) != (b < 0)))
      q--;
   return q;
}

/* one tap through the per-axis address modes; *border is set when the coordinate is outside and the mode says border */
static int
tap1(const struct texdata *t, int level, int x, int y, VkSamplerAddressMode mu, VkSamplerAddressMode mv,
     const int bc[4], int c, int *border)
{
   int n = TEXN >> level, bx = 0, by = 0;
   switch (mu) {
   case VK_SAMPLER_ADDRESS_MODE_REPEAT: x = ((x % n) + n) % n; break;
   case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: {
      int p = 2 * n, m = ((x % p) + p) % p;
      x = m < n ? m : p - 1 - m;
      break;
   }
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: x = x < 0 ? 0 : (x > n - 1 ? n - 1 : x); break;
   case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE:
      if (!g_mclamp_clamp) {
         if (x < 0) x = -x - 1;
      }
      x = x < 0 ? 0 : (x > n - 1 ? n - 1 : x);
      break;
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
      if (x < 0 || x >= n) bx = 1;
      break;
   default: break;
   }
   switch (mv) {
   case VK_SAMPLER_ADDRESS_MODE_REPEAT: y = ((y % n) + n) % n; break;
   case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: {
      int p = 2 * n, m = ((y % p) + p) % p;
      y = m < n ? m : p - 1 - m;
      break;
   }
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: y = y < 0 ? 0 : (y > n - 1 ? n - 1 : y); break;
   case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE:
      if (!g_mclamp_clamp) {
         if (y < 0) y = -y - 1;
      }
      y = y < 0 ? 0 : (y > n - 1 ? n - 1 : y);
      break;
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
      if (y < 0 || y >= n) by = 1;
      break;
   default: break;
   }
   *border = 0;
   if (bx || by) {
      *border = 1;
      return bc[c];
   }
   return t->mip[level][(y * TEXN + x) * 4 + c];
}

/* Filtered sample of one level; tu/tv are texel coordinates in 1/8 texel units (tu = 8 -> texel coordinate 1.0). */
static void
sample_level(const struct texdata *t, int level, int tu, int tv, int linear, VkSamplerAddressMode mu,
             VkSamplerAddressMode mv, const int bc[4], int out[4])
{
   if (!linear) {
      int i = floordiv(tu, 8), j = floordiv(tv, 8), b;
      for (int c = 0; c < 4; c++)
         out[c] = tap1(t, level, i, j, mu, mv, bc, c, &b);
      return;
   }
   int i0 = floordiv(tu - 4, 8), fu = (tu - 4) - 8 * i0;
   int j0 = floordiv(tv - 4, 8), fv = (tv - 4) - 8 * j0;
   for (int c = 0; c < 4; c++) {
      int b;
      int v00 = tap1(t, level, i0, j0, mu, mv, bc, c, &b);
      int v10 = tap1(t, level, i0 + 1, j0, mu, mv, bc, c, &b);
      int v01 = tap1(t, level, i0, j0 + 1, mu, mv, bc, c, &b);
      int v11 = tap1(t, level, i0 + 1, j0 + 1, mu, mv, bc, c, &b);
      /* every channel is a multiple of 8, so this division is exact */
      int num = v00 * (8 - fu) * (8 - fv) + v10 * fu * (8 - fv) + v01 * (8 - fu) * fv + v11 * fu * fv;
      out[c] = (num + 32) / 64;   /* round to nearest, as the UNORM8 write does */
   }
}

/* ---------------------------------------------------------------- cases */
enum group { G_SAMP, G_BLEND, G_DS };
enum img { I_PAT, I_BLK, I_FLAT };
enum { SH_TEXLOD = 0, SH_IMPL = 1, SH_SOLID = 2 };

struct samp {
   VkSamplerAddressMode mu, mv;
   VkFilter minf, magf;
   VkSamplerMipmapMode mm;
   float aniso;          /* 0 = anisotropyEnable false */
   int linear;           /* which filter the reference applies */
   int lvl0, lvl1, w1;   /* level0, blended with level1 by w1/8 (w1 = 0: single level) */
};

struct blstate {
   int on;
   VkBlendFactor sf, df, saf, daf;
   VkBlendOp op;
   VkColorComponentFlags mask;
   int bconst;
};

struct dsstate {
   int depth_test, depth_write;
   VkCompareOp dcmp;
   float dmin, dmax;
   int clamp, clip_off;
   int stencil;
   VkCompareOp scmp[2];
   VkStencilOp sfail[2], sdfail[2], spass[2];
   uint32_t scmpmask, swmask, sref;
   int cw;
};

struct scase {
   char id[64];
   enum group g;
   enum img img;
   int shader;
   float uvs[4], col0[4], z;
   struct samp s;
   int bc[4];
   VkBorderColor border_color;
   float border_f[4];
   int tu0, tv0, tustep, tvstep;         /* tu(px) = tu0 + px*tustep, in 1/8 texels of lvl0 */
   int tu0_1, tv0_1, tustep_1;           /* the same grid in 1/8 texels of lvl1 (for the level blend) */
   struct blstate b;
   float bconst_f[4];
   uint8_t dst8[4], src8[4];
   struct dsstate d;
   float z_base;
   uint8_t base8[4], test8[4];
   int ndraw;   /* 1: a single quad; 2: base quad (shared pipeline) then the case pipeline */
   const char *skip;   /* set when the device cannot express the row: reported as SKIP, not PASS */
   VkPipeline pipe;
   VkDescriptorSet dset;
};

/* the --only predicate, used by every per-case loop so an unselected row cannot fail the run */
static int
case_selected(const struct scase *c)
{
   return !g_only || !strncmp(c->id, g_only, strlen(g_only));
}

static struct scase *cases;

static struct scase *
newcase(enum group g, const char *fmt, ...)
{
   if (g_ncase >= NCASE)
      die("too many cases: raise NROWS");
   struct scase *c = &cases[g_ncase++];
   memset(c, 0, sizeof *c);
   c->g = g;
   c->ndraw = 1;
   c->s.minf = c->s.magf = VK_FILTER_NEAREST;
   c->s.mm = VK_SAMPLER_MIPMAP_MODE_NEAREST;
   c->s.mu = c->s.mv = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
   c->s.lvl0 = 0;
   c->s.lvl1 = 0;
   c->d.dmin = 0.0f;
   c->d.dmax = 1.0f;
   c->d.dcmp = VK_COMPARE_OP_ALWAYS;
   c->d.scmpmask = 0xff;
   c->d.swmask = 0xff;
   c->b.mask = 0xf;
   c->border_color = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(c->id, sizeof c->id, fmt, ap);
   va_end(ap);
   return c;
}

static const char *
wrapname(VkSamplerAddressMode m)
{
   switch (m) {
   case VK_SAMPLER_ADDRESS_MODE_REPEAT: return "repeat";
   case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return "mirror";
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: return "clamp";
   case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: return "mclamp";
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: return "border";
   default: return "?";
   }
}

static const char *
filtname(VkFilter f) { return f == VK_FILTER_LINEAR ? "lin" : "near"; }

static const char *
bfname(VkBlendFactor f)
{
   switch (f) {
   case VK_BLEND_FACTOR_ZERO: return "zero";
   case VK_BLEND_FACTOR_ONE: return "one";
   case VK_BLEND_FACTOR_SRC_COLOR: return "srcc";
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return "isrcc";
   case VK_BLEND_FACTOR_DST_COLOR: return "dstc";
   case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return "idstc";
   case VK_BLEND_FACTOR_SRC_ALPHA: return "srca";
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return "isrca";
   case VK_BLEND_FACTOR_DST_ALPHA: return "dsta";
   case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return "idsta";
   case VK_BLEND_FACTOR_CONSTANT_COLOR: return "cc";
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return "icc";
   case VK_BLEND_FACTOR_CONSTANT_ALPHA: return "ca";
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return "ica";
   default: return "?";
   }
}

static const char *
opname(VkBlendOp o)
{
   switch (o) {
   case VK_BLEND_OP_ADD: return "add";
   case VK_BLEND_OP_SUBTRACT: return "sub";
   case VK_BLEND_OP_REVERSE_SUBTRACT: return "rsub";
   case VK_BLEND_OP_MIN: return "min";
   case VK_BLEND_OP_MAX: return "max";
   default: return "?";
   }
}

static const char *
cmpname(VkCompareOp o)
{
   switch (o) {
   case VK_COMPARE_OP_NEVER: return "never";
   case VK_COMPARE_OP_LESS: return "less";
   case VK_COMPARE_OP_EQUAL: return "equal";
   case VK_COMPARE_OP_LESS_OR_EQUAL: return "lequal";
   case VK_COMPARE_OP_GREATER: return "greater";
   case VK_COMPARE_OP_NOT_EQUAL: return "nequal";
   case VK_COMPARE_OP_GREATER_OR_EQUAL: return "gequal";
   case VK_COMPARE_OP_ALWAYS: return "always";
   default: return "?";
   }
}

static const char *
stname(VkStencilOp o)
{
   switch (o) {
   case VK_STENCIL_OP_KEEP: return "keep";
   case VK_STENCIL_OP_ZERO: return "zero";
   case VK_STENCIL_OP_REPLACE: return "repl";
   case VK_STENCIL_OP_INCREMENT_AND_CLAMP: return "incclamp";
   case VK_STENCIL_OP_DECREMENT_AND_CLAMP: return "decclamp";
   case VK_STENCIL_OP_INVERT: return "invert";
   case VK_STENCIL_OP_INCREMENT_AND_WRAP: return "incwrap";
   case VK_STENCIL_OP_DECREMENT_AND_WRAP: return "decwrap";
   default: return "?";
   }
}

static struct scase *
st_case(int face, VkStencilOp op, const char *id, uint32_t wmask)
{
   struct scase *c = newcase(G_DS, "%s", id);
   c->shader = SH_SOLID;
   c->ndraw = 1;
   c->d.depth_test = 1;   /* ALWAYS + no write: the stencil unit still gets the standard ZSD path */
   c->d.depth_write = 0;
   c->d.dcmp = VK_COMPARE_OP_ALWAYS;
   c->d.stencil = 1;
   c->d.cw = face;
   c->d.scmp[0] = c->d.scmp[1] = VK_COMPARE_OP_ALWAYS;
   c->d.sfail[0] = c->d.sfail[1] = VK_STENCIL_OP_KEEP;
   c->d.sdfail[0] = c->d.sdfail[1] = VK_STENCIL_OP_KEEP;
   c->d.spass[0] = c->d.spass[1] = VK_STENCIL_OP_KEEP;
   /* the op goes into passOp, depthFailOp and failOp of the face this row targets; the other face gets ZERO, whose
    * result (0x00) no op can produce from the 0x33 clear, so a driver that ignores cw cannot pass a face row */
   for (int f = 0; f < 2; f++) {
      VkStencilOp fo = f == face ? op : VK_STENCIL_OP_ZERO;
      c->d.sfail[f] = c->d.sdfail[f] = c->d.spass[f] = fo;
   }
   c->d.sref = 0x5a;
   c->d.swmask = wmask;
   c->d.scmpmask = 0xff;
   c->test8[0] = 200, c->test8[1] = 150, c->test8[2] = 100, c->test8[3] = 255;
   c->z = 0.5f;
   return c;
}

static void
build_cases(void)
{
   static const VkSamplerAddressMode wraps[] = {
      VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
   static const VkBlendFactor fac[] = {
      VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
      VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
      VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA};

   /* ---- samplers. tu = 16*px - 20 -> texel coordinate 2*px - 2.5, i.e. exact texel centres from -2.5 to 12.5, so
    * both sides of the [0,1] range are covered and the nearest-neighbour result has no rounding question at all. */
   for (unsigned u = 0; u < 5; u++)
      for (unsigned v = 0; v < 5; v++) {
         struct scase *c = newcase(G_SAMP, "s_nearest_%s_%s", wrapname(wraps[u]), wrapname(wraps[v]));
         c->img = I_PAT;
         c->shader = SH_TEXLOD;
         c->s.mu = wraps[u], c->s.mv = wraps[v];
         c->tu0 = -20, c->tustep = 16, c->tv0 = -20, c->tvstep = 16;
         c->uvs[0] = 2.0f, c->uvs[1] = 2.0f, c->uvs[2] = -0.4375f, c->uvs[3] = -0.4375f;
      }
   /* linear, frac 1/4: T = 2px - 2.25 */
   for (unsigned u = 0; u < 5; u++)
      for (unsigned v = 0; v < 5; v++) {
         struct scase *c = newcase(G_SAMP, "s_linear_q1_%s_%s", wrapname(wraps[u]), wrapname(wraps[v]));
         c->img = I_PAT;
         c->shader = SH_TEXLOD;
         c->s.mu = wraps[u], c->s.mv = wraps[v];
         c->s.minf = c->s.magf = VK_FILTER_LINEAR;
         c->s.linear = 1;
         c->tu0 = -18, c->tustep = 16, c->tv0 = -18, c->tvstep = 16;
         c->uvs[0] = 2.0f, c->uvs[1] = 2.0f, c->uvs[2] = -0.40625f, c->uvs[3] = -0.40625f;
      }
   /* linear, frac 3/4: T = 2px - 1.75 */
   for (unsigned u = 0; u < 4; u++)
      for (unsigned v = 0; v < 4; v++) {
         VkSamplerAddressMode m = wraps[u == 3 ? 4 : u], n = wraps[v == 3 ? 4 : v];
         struct scase *c = newcase(G_SAMP, "s_linear_q3_%s_%s", wrapname(m), wrapname(n));
         c->img = I_PAT;
         c->shader = SH_TEXLOD;
         c->s.mu = m, c->s.mv = n;
         c->s.minf = c->s.magf = VK_FILTER_LINEAR;
         c->s.linear = 1;
         c->tu0 = -14, c->tustep = 16, c->tv0 = -14, c->tvstep = 16;
         c->uvs[0] = 2.0f, c->uvs[1] = 2.0f, c->uvs[2] = -0.34375f, c->uvs[3] = -0.34375f;
      }
   /* minFilter vs magFilter: explicit LOD -0.5 magnifies, LOD 1.0 minifies (and selects level 1) */
   for (int i = 0; i < 4; i++) {
      VkFilter mn = (i & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
      VkFilter mg = (i & 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
      for (int k = 0; k < 2; k++) {
         struct scase *c = newcase(G_SAMP, "s_%s_%s_%s", k ? "minf" : "magf", filtname(mn), filtname(mg));
         c->img = I_PAT;
         c->shader = SH_TEXLOD;
         c->s.minf = mn, c->s.magf = mg;
         c->s.linear = (k ? mn : mg) == VK_FILTER_LINEAR;
         /* frac 1/8 steps inside a texel; level 1 is 4x4 so the steps shrink there */
         /* one UV mapping, two grids: T0 = 1.5px + 0.75 in level 0, T1 = T0/2 in level 1 */
         c->tu0 = k ? 3 : 6, c->tustep = k ? 6 : 12, c->tv0 = c->tu0, c->tvstep = c->tustep;
         c->uvs[0] = 1.5f, c->uvs[1] = 1.5f, c->uvs[2] = 0.0f, c->uvs[3] = 0.0f;
         if (k) {
            c->s.lvl0 = 1;
            c->col0[0] = 1.0f;
         } else {
            c->col0[0] = -0.5f;
         }
      }
   }
   /* mipmapMode at integer LODs: exact, no level blend to guess */
   for (int m = 0; m < 2; m++)
      for (int l = 0; l <= MAXLVL; l++) {
         struct scase *c = newcase(G_SAMP, "s_mip_%s_lod%d", m ? "lin" : "near", l);
         c->img = I_PAT;
         c->shader = SH_TEXLOD;
         c->s.mu = c->s.mv = VK_SAMPLER_ADDRESS_MODE_REPEAT;
         c->s.mm = m ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
         c->s.lvl0 = l;
         c->s.lvl1 = l + 1 > MAXLVL ? MAXLVL : l + 1;
         /* one texel of level l per pixel, centred, from texel -2 to 5: frac 0 at every level, exact */
         float sc = (float)(1 << l), off = -2.0f / (float)(TEXN >> l);   /* T(py) = px - 1.5 in level-l texels */
         c->tu0 = -12, c->tustep = 8, c->tv0 = -12, c->tvstep = 8;
         c->uvs[0] = sc, c->uvs[1] = sc, c->uvs[2] = off, c->uvs[3] = off;
         c->col0[0] = (float)l;
      }
   /* mipmapMode LINEAR at LOD 0.5 with texture B: both levels hold the same colour at a texel centre, so the exact
    * level-blend weight cannot change the expectation */
   {
      struct scase *c = newcase(G_SAMP, "s_mipblend_lin_lod0.5");
      c->img = I_BLK;
      c->shader = SH_TEXLOD;
      c->s.mm = VK_SAMPLER_MIPMAP_MODE_LINEAR;
      c->tu0 = 8, c->tustep = 16, c->tv0 = 8, c->tvstep = 16;   /* T0 = 2px+1: the middle of a 2x2 block */
      c->tu0_1 = 4, c->tv0_1 = 4, c->tustep_1 = 8;             /* T1 = px+0.5: a level-1 texel centre */
      c->uvs[0] = 2.0f, c->uvs[1] = 2.0f;
      c->col0[0] = 0.5f;
   }
   /* anisotropy (implicit LOD, flat mips red/green/blue): 2:1 footprint. Without aniso the LOD follows the long axis
    * (log2 2 = 1 -> green); with maxAnisotropy >= the ratio it follows the short axis (0 -> red). Both are far from a
    * mip boundary, so the driver's 4-bit mip precision cannot decide the colour. The expected colour is exact on
    * purpose: this suite pins OUR driver's anisotropic scheme (M/notes/state-sweep.md, X4/X8 table), and the
    * s_aniso_*_iso rows are the control that isolates the LOD rule from the anisotropy rule. */
   for (int a = 0; a < 4; a++) {
      float an = a == 0 ? 0.0f : (float)(1 << (a + 1));   /* off, 4, 8, 16 */
      struct scase *c = newcase(G_SAMP, "s_aniso_%d_ratio2", (int)an);
      c->img = I_FLAT;
      c->shader = SH_IMPL;
      c->s.mu = c->s.mv = VK_SAMPLER_ADDRESS_MODE_REPEAT;   /* no clamped coordinate: the LOD is uniform */
      c->s.aniso = an;
      c->s.lvl0 = a == 0 ? 1 : 0;
      c->tu0 = 4, c->tustep = 16, c->tv0 = 4, c->tvstep = 8;
      c->uvs[0] = 2.0f, c->uvs[1] = 1.0f;
   }
   for (int a = 0; a < 2; a++) {
      float an = a ? 16.0f : 0.0f;
      struct scase *c = newcase(G_SAMP, "s_aniso_%d_iso", (int)an);
      c->img = I_FLAT;
      c->shader = SH_IMPL;
      c->s.mu = c->s.mv = VK_SAMPLER_ADDRESS_MODE_REPEAT;
      c->s.aniso = an;
      c->s.lvl0 = 1;   /* 2 texels per pixel on both axes -> lod log2(2) = 1 -> level 1, green */
      c->tu0 = 8, c->tustep = 16, c->tv0 = 8, c->tvstep = 16;
      c->uvs[0] = 2.0f, c->uvs[1] = 2.0f;
   }
   /* border colour: the two fixed colours, then a custom one (VK_EXT_custom_border_color is advertised) */
   for (int i = 0; i < 3; i++) {
      struct scase *c = newcase(G_SAMP, "s_border_%s", i == 0 ? "float_tblack" : i == 1 ? "float_oblack" : "custom");
      c->img = I_PAT;
      c->shader = SH_TEXLOD;
      c->s.mu = c->s.mv = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
      c->s.minf = c->s.magf = VK_FILTER_NEAREST;
      c->tu0 = -20, c->tustep = 16, c->tv0 = -20, c->tvstep = 16;
      c->uvs[0] = 2.0f, c->uvs[1] = 2.0f, c->uvs[2] = -0.4375f, c->uvs[3] = -0.4375f;
      if (i == 0) {
         c->border_color = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;   /* 0,0,0,0 */
      } else if (i == 1) {
         c->border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;        /* 0,0,0,1 */
         c->bc[3] = 255;
      } else {
         static const int cust[4] = {64, 128, 192, 255};
         c->border_color = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
         for (int ch = 0; ch < 4; ch++) {
            c->bc[ch] = cust[ch];
            c->border_f[ch] = cust[ch] / 255.0f;
         }
      }
   }

   /* ---- blend. src = 153,102,204,128; dst = 102,153,51,64; constant = 51 (all in 1/255 units) */
   static const uint8_t src8v[4] = {153, 102, 204, 128}, dst8v[4] = {102, 153, 51, 64};
   for (unsigned i = 0; i < 14; i++)
      for (unsigned j = 0; j < 14; j++) {
         struct scase *c = newcase(G_BLEND, "b_add_%s_%s", bfname(fac[i]), bfname(fac[j]));
         c->shader = SH_SOLID;
         c->ndraw = 2;
         c->b.on = 1;
         c->b.sf = c->b.saf = fac[i];
         c->b.df = c->b.daf = fac[j];
         c->b.op = VK_BLEND_OP_ADD;
         c->b.bconst = 51;
         for (int ch = 0; ch < 4; ch++) c->bconst_f[ch] = 51 / 255.0f;
         memcpy(c->src8, src8v, 4);
         memcpy(c->dst8, dst8v, 4);
      }
   {
      static const VkBlendFactor d2[] = {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                         VK_BLEND_FACTOR_DST_COLOR};
      for (int o = 0; o < 2; o++)
         for (unsigned i = 0; i < 14; i++)
            for (unsigned j = 0; j < 3; j++) {
               VkBlendOp op = o ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_SUBTRACT;
               struct scase *c = newcase(G_BLEND, "b_%s_%s_%s", opname(op), bfname(fac[i]), bfname(d2[j]));
               c->shader = SH_SOLID;
               c->ndraw = 2;
               c->b.on = 1;
               c->b.sf = c->b.saf = fac[i];
               c->b.df = c->b.daf = d2[j];
               c->b.op = op;
               c->b.bconst = 51;
               for (int ch = 0; ch < 4; ch++) c->bconst_f[ch] = 51 / 255.0f;
               memcpy(c->src8, src8v, 4);
               memcpy(c->dst8, dst8v, 4);
            }
      for (int o = 0; o < 2; o++)
         for (int k = 0; k < 2; k++) {
            struct scase *c = newcase(G_BLEND, "b_%s_f%d", opname(o ? VK_BLEND_OP_MAX : VK_BLEND_OP_MIN), k);
            c->shader = SH_SOLID;
            c->ndraw = 2;
            c->b.on = 1;
            /* MIN/MAX ignore the factors; ONE/ZERO keeps that question out of the expectation */
            c->b.sf = c->b.saf = VK_BLEND_FACTOR_ONE;
            c->b.df = c->b.daf = VK_BLEND_FACTOR_ZERO;
            c->b.op = o ? VK_BLEND_OP_MAX : VK_BLEND_OP_MIN;
            memcpy(c->src8, src8v, 4);
            memcpy(c->dst8, dst8v, 4);
         }
      static const VkBlendFactor af[] = {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_ALPHA,
                                         VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
                                         VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA};
      for (unsigned i = 0; i < 6; i++)
         for (unsigned j = 0; j < 6; j++) {
            struct scase *c = newcase(G_BLEND, "b_alpha_%s_%s", bfname(af[i]), bfname(af[j]));
            c->shader = SH_SOLID;
            c->ndraw = 2;
            c->b.on = 1;
            c->b.sf = c->b.df = VK_BLEND_FACTOR_ONE;   /* colour: src + dst */
            c->b.saf = af[i];
            c->b.daf = af[j];
            c->b.op = VK_BLEND_OP_ADD;
            memcpy(c->src8, src8v, 4);
            memcpy(c->dst8, dst8v, 4);
         }
      for (unsigned m = 0; m < 16; m++) {
         struct scase *c = newcase(G_BLEND, "b_wmask_%x", m);
         c->shader = SH_SOLID;
         c->ndraw = 2;
         c->b.on = 1;
         c->b.sf = c->b.df = c->b.saf = c->b.daf = VK_BLEND_FACTOR_ONE;
         c->b.op = VK_BLEND_OP_ADD;
         c->b.mask = m;
         memcpy(c->src8, src8v, 4);
         memcpy(c->dst8, dst8v, 4);
      }
   }

   /* ---- depth compare with and without write. Base quad at z = 0.375 (exact in D24: 0x600000). */
   {
      static const VkCompareOp ops[] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL,
                                        VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL,
                                        VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
      /* test z per op; the same float as the base for EQUAL / LESS_OR_EQUAL / GREATER_OR_EQUAL so both quantize
       * identically, and a large margin for the rest */
      static const float zt[] = {0.375f, 0.1875f, 0.375f, 0.375f, 0.5625f, 0.5625f, 0.375f, 0.5625f};
      static const uint8_t base8v[4] = {32, 64, 96, 255}, test8v[4] = {200, 150, 100, 255};
      for (unsigned i = 0; i < 8; i++)
         for (int w = 0; w < 2; w++) {
            struct scase *c = newcase(G_DS, "d_cmp_%s_w%d", cmpname(ops[i]), w);
            c->shader = SH_SOLID;
            c->ndraw = 2;
            c->d.depth_test = 1;
            c->d.depth_write = w;
            c->d.dcmp = ops[i];
            c->z_base = 0.375f;
            c->z = zt[i];
            memcpy(c->base8, base8v, 4);
            memcpy(c->test8, test8v, 4);
         }
   }
   /* ---- depth clip / depth clamp: viewport depth range 0.75..1.0 and a quad at z = -0.5 (beyond the near plane) or
    * 1.5 (beyond the far plane). Comparison ALWAYS + write ON so the depth read-back states exactly what the
    * rasterizer produced; the colour says whether the quad survived at all. */
   {
      struct {
         const char *id;
         float z;
         int clamp, clip_off;
      } dc[] = {
         {"d_dc_near_clip_on", -0.5f, 0, 0},     {"d_dc_near_clip_off", -0.5f, 0, 1},
         {"d_dc_near_clip_off_clamp", -0.5f, 1, 1}, {"d_dc_far_clip_on", 1.5f, 0, 0},
         {"d_dc_far_clip_off", 1.5f, 0, 1},
      };
      static const uint8_t base8v[4] = {32, 64, 96, 255}, test8v[4] = {200, 150, 100, 255};
      for (unsigned i = 0; i < 5; i++) {
         struct scase *c = newcase(G_DS, "%s", dc[i].id);
         c->shader = SH_SOLID;
         c->ndraw = 2;
         c->d.depth_test = 1;
         c->d.depth_write = 1;
         c->d.dcmp = VK_COMPARE_OP_ALWAYS;
         c->d.dmin = 0.75f;
         c->d.dmax = 1.0f;
         c->d.clamp = dc[i].clamp;
         c->d.clip_off = dc[i].clip_off;
         c->z_base = 0.5f;   /* -> 0.875 through the 0.75..1.0 range */
         c->z = dc[i].z;
         memcpy(c->base8, base8v, 4);
         memcpy(c->test8, test8v, 4);
      }
   }
   /* ---- stencil: every op on one face, ref 0x5a, then masks and the compare */
   {
      static const VkStencilOp ops[] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE,
                                        VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                                        VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP,
                                        VK_STENCIL_OP_DECREMENT_AND_WRAP};
      for (int face = 0; face < 2; face++)
         for (unsigned i = 0; i < 8; i++) {
            char id[64];
            snprintf(id, sizeof id, "d_st_%s_%s", face ? "back" : "front", stname(ops[i]));
            st_case(face, ops[i], id, 0xff);
         }
      st_case(0, VK_STENCIL_OP_REPLACE, "d_st_front_repl_wmask0f", 0x0f);
      {
         struct scase *c = newcase(G_DS, "d_st_cmp_equal_pass");
         *c = cases[g_ncase - 2];   /* the wmask REPLACE case, then rewrite what differs */
         memset(c->id, 0, sizeof c->id);
         snprintf(c->id, sizeof c->id, "d_st_cmp_equal_pass");
         c->d.scmp[0] = c->d.scmp[1] = VK_COMPARE_OP_EQUAL;
         c->d.swmask = 0xff;
         c->d.spass[0] = c->d.spass[1] = VK_STENCIL_OP_REPLACE;
         c->d.sref = 0x33;   /* EQUAL 0x33 against the cleared 0x33 -> passes, REPLACE writes 0x33 */
         c = newcase(G_DS, "d_st_cmp_equal_fail");
         *c = cases[g_ncase - 2];
         memset(c->id, 0, sizeof c->id);
         snprintf(c->id, sizeof c->id, "d_st_cmp_equal_fail");
         c->d.scmp[0] = c->d.scmp[1] = VK_COMPARE_OP_EQUAL;
         c->d.swmask = 0xff;
         c->d.spass[0] = c->d.spass[1] = VK_STENCIL_OP_REPLACE;
         c->d.sref = 0x11;   /* 0x11 != 0x33 -> the fragment is dropped, the colour stays at the clear value */
         memcpy(c->test8, c->base8, 4);
      }
   }
}

/* ---------------------------------------------------------------- pipelines */
static VkRenderPass rp;
static VkFramebuffer fb;
static VkPipelineLayout pl;
static VkDescriptorSetLayout dsl;
static VkDescriptorPool dpool;
static VkShaderModule vs_mod, fs_tex, fs_impl, fs_solid;
static VkPipeline pipe_tex, pipe_impl, pipe_solid, pipe_base;
static VkSampler smp_dummy;
static VkDescriptorSet dset_dummy;

struct pipekey {
   int shader;
   struct blstate b;
   int depth_test, depth_write;
   VkCompareOp dcmp;
   int clamp, clip_off;
   int stencil;
   VkCompareOp scmp[2];
   VkStencilOp sfail[2], sdfail[2], spass[2];
   uint32_t scmpmask, swmask, sref;
   int cw;
   float dmin, dmax;
};

static VkPipeline
make_pipe(const struct pipekey *k)
{
   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = vs_mod, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = k->shader == SH_TEXLOD ? fs_tex : k->shader == SH_IMPL ? fs_impl : fs_solid, .pName = "main"},
   };
   VkPipelineVertexInputStateCreateInfo vin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                           .viewportCount = 1, .scissorCount = 1};
   VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE, .frontFace = k->cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .depthClampEnable = k->clamp, .depthBiasEnable = VK_FALSE, .lineWidth = 1.0f};
   VkPipelineRasterizationDepthClipStateCreateInfoEXT dce = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT,
      .depthClipEnable = k->clip_off ? VK_FALSE : VK_TRUE};
   rs.pNext = &dce;
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                              .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineDepthStencilStateCreateInfo dss = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = k->depth_test,
                                                .depthWriteEnable = k->depth_write,
                                                .depthCompareOp = k->dcmp,
                                                .stencilTestEnable = k->stencil,
                                                .minDepthBounds = 0.0f,
                                                .maxDepthBounds = 1.0f};
   for (int i = 0; i < 2; i++) {
      VkStencilOpState *s = i ? &dss.back : &dss.front;
      s->failOp = k->sfail[i];
      s->passOp = k->spass[i];
      s->depthFailOp = k->sdfail[i];
      s->compareOp = k->scmp[i];
      s->compareMask = k->scmpmask;
      s->writeMask = k->swmask;
      s->reference = k->sref;
   }
   VkPipelineColorBlendAttachmentState cba = {.blendEnable = k->b.on,
                                              .srcColorBlendFactor = k->b.sf,
                                              .dstColorBlendFactor = k->b.df,
                                              .colorBlendOp = k->b.op,
                                              .srcAlphaBlendFactor = k->b.saf,
                                              .dstAlphaBlendFactor = k->b.daf,
                                              .alphaBlendOp = k->b.op,
                                              .colorWriteMask = k->b.on ? k->b.mask : 0xf};
   VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                             .attachmentCount = 1, .pAttachments = &cba};
   if (k->b.on && k->b.bconst >= 0) {
      cb.blendConstants[0] = cb.blendConstants[1] = cb.blendConstants[2] = cb.blendConstants[3] =
         k->b.bconst / 255.0f;
   }
   VkDynamicState dynv[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
   VkPipelineDynamicStateCreateInfo dyn = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dynv};
   VkGraphicsPipelineCreateInfo gpi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .stageCount = 2, .pStages = st,
                                       .pVertexInputState = &vin, .pInputAssemblyState = &ia,
                                       .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pDepthStencilState = &dss,
                                       .pColorBlendState = &cb, .pDynamicState = &dyn,
                                       .layout = pl, .renderPass = rp, .subpass = 0};
   VkPipeline p;
   VkResult r = CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &p);
   if (r != VK_SUCCESS) {
      fprintf(stderr, "t_state_sweep: pipeline '%s' failed (VkResult %d)\n", k->b.on ? "blend" : "state", r);
      return VK_NULL_HANDLE;
   }
   return p;
}

/* ---------------------------------------------------------------- reference */
/* Quantize a window depth to the 24-bit UNORM the D24S8 attachment stores. */
static uint32_t
qz(float z)
{
   double v = (double)z * (double)D24MAX;
   if (v < 0) v = 0;
   if (v > (double)D24MAX) v = (double)D24MAX;
   return (uint32_t)lround(v);
}

static int
dcmp(VkCompareOp op, uint32_t a, uint32_t b)
{
   switch (op) {
   case VK_COMPARE_OP_NEVER: return 0;
   case VK_COMPARE_OP_LESS: return a < b;
   case VK_COMPARE_OP_EQUAL: return a == b;
   case VK_COMPARE_OP_LESS_OR_EQUAL: return a <= b;
   case VK_COMPARE_OP_GREATER: return a > b;
   case VK_COMPARE_OP_NOT_EQUAL: return a != b;
   case VK_COMPARE_OP_GREATER_OR_EQUAL: return a >= b;
   default: return 1;
   }
}

/* The depth the rasterizer produces for a clip-space z: clamp to [0,1] when depth clip is off, then the viewport
 * depth range, then (depthClampEnable) the range again. */
static float
window_z(const struct scase *c, float zndc)
{
   if (c->d.clip_off && (zndc < 0.0f || zndc > 1.0f))
      zndc = zndc < 0.0f ? 0.0f : 1.0f;
   float z = c->d.dmin + zndc * (c->d.dmax - c->d.dmin);
   if (c->d.clamp) {
      if (z < c->d.dmin) z = c->d.dmin;
      if (z > c->d.dmax) z = c->d.dmax;
   }
   return z;
}

/* Blend factor in 1/255 units for one channel. */
static int
bfac(VkBlendFactor f, const uint8_t src[4], const uint8_t dst[4], int c, int bconst)
{
   switch (f) {
   case VK_BLEND_FACTOR_ZERO: return 0;
   case VK_BLEND_FACTOR_ONE: return 255;
   case VK_BLEND_FACTOR_SRC_COLOR: return src[c];
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return 255 - src[c];
   case VK_BLEND_FACTOR_DST_COLOR: return dst[c];
   case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return 255 - dst[c];
   case VK_BLEND_FACTOR_SRC_ALPHA: return src[3];
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return 255 - src[3];
   case VK_BLEND_FACTOR_DST_ALPHA: return dst[3];
   case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return 255 - dst[3];
   case VK_BLEND_FACTOR_CONSTANT_COLOR: return bconst;
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return 255 - bconst;
   case VK_BLEND_FACTOR_CONSTANT_ALPHA: return bconst;
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return 255 - bconst;
   default: return 0;
   }
}

/* Exact reference for a blend case: every value is an integer in 1/255 units, and 255 is odd, so no division here
 * ever lands on a .5 tie. */
static void
ref_blend(const struct scase *c, int out[4])
{
   for (int ch = 0; ch < 4; ch++) {
      int s = c->src8[ch], d = c->dst8[ch];
      int fs = bfac(ch == 3 ? c->b.saf : c->b.sf, c->src8, c->dst8, ch, c->b.bconst);
      int fd = bfac(ch == 3 ? c->b.daf : c->b.df, c->src8, c->dst8, ch, c->b.bconst);
      int num = s * fs + d * fd, v;   /* ADD */
      if (c->b.op == VK_BLEND_OP_ADD) v = (num + 127) / 255;
      else if (c->b.op == VK_BLEND_OP_SUBTRACT) v = (int)lround((double)(s * fs - d * fd) / 255.0);
      else if (c->b.op == VK_BLEND_OP_REVERSE_SUBTRACT) v = (int)lround((double)(d * fd - s * fs) / 255.0);
      else if (c->b.op == VK_BLEND_OP_MIN) v = s < d ? s : d;
      else v = s > d ? s : d;
      if (v < 0) v = 0;
      if (v > 255) v = 255;
      out[ch] = (c->b.mask & (1u << ch)) ? v : c->dst8[ch];
   }
}

/* ---------------------------------------------------------------- main */

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) g_spvdir = argv[++i];
      else if (!strcmp(argv[i], "--only") && i + 1 < argc) g_only = argv[++i];
      else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) g_ppm = argv[++i];
      else {
         fprintf(stderr, "usage: t_state_sweep [--direct icd.so] [--spv-dir dir] [--only prefix] [--ppm file]\n");
         return 2;
      }
   }
   const char *be = getenv("MALI_BACKEND");
   g_fake = be && !strcmp(be, "fake");
   cases = calloc(NCASE, sizeof *cases);
   if (!cases)
      die("out of memory");

   void *lib = dlopen(icd ? icd : "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      fprintf(stderr, "t_state_sweep: dlopen %s: %s\n", icd ? icd : "libvulkan.so.1", dlerror());
      return 2;
   }
   PFN_vkGetInstanceProcAddr gipa;
   if (icd) {
      gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
      VkResult (*neg)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
      uint32_t iv = 5;
      if (!gipa || !neg || neg(&iv)) {
         fprintf(stderr, "t_state_sweep: %s is not an ICD\n", icd);
         return 2;
      }
   } else {
      gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
   }
   if (!gipa)
      die("no vkGetInstanceProcAddr");
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   const VkApplicationInfo appi = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &appi};
   VkInstance inst;
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS)
      die("vkCreateInstance");
#define I(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n)
   I(EnumeratePhysicalDevices);
   I(GetPhysicalDeviceProperties);
   I(GetPhysicalDeviceFeatures);
   I(GetPhysicalDeviceQueueFamilyProperties);
   I(CreateDevice);
   I(GetDeviceProcAddr);
#undef I
   GetPhysicalDeviceMemoryProperties_ = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
   if (!GetPhysicalDeviceMemoryProperties_)
      die("no vkGetPhysicalDeviceMemoryProperties");
   uint32_t n = 1;
   VkResult er = EnumeratePhysicalDevices(inst, &n, &pd);
   /* a loader with more than one device answers VK_INCOMPLETE (or SUCCESS) and fills the slot: both are usable */
   if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || n < 1)
      die("no physical device");
   GetPhysicalDeviceProperties(pd, &g_props);
   GetPhysicalDeviceFeatures(pd, &g_feat);

   uint32_t nqf = 0;
   GetPhysicalDeviceQueueFamilyProperties(pd, &nqf, NULL);
   VkQueueFamilyProperties qf[16];
   if (nqf > 16) nqf = 16;
   GetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qf);
   uint32_t qfi = UINT32_MAX;
   for (uint32_t i = 0; i < nqf; i++)
      if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qfi == UINT32_MAX)
         qfi = i;
   if (qfi == UINT32_MAX)
      die("no graphics queue");

   /* depth clip and custom border colours are only legal with their extension enabled */
   VkPhysicalDeviceDepthClipEnableFeaturesEXT dcf = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, .depthClipEnable = VK_TRUE};
   VkPhysicalDeviceCustomBorderColorFeaturesEXT cbf = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,
      .customBorderColors = VK_TRUE, .customBorderColorWithoutFormat = VK_TRUE};
   const char *en_ext[4];
   uint32_t nx = 0;
   void *chain = NULL;
   {
      PFN_vkEnumerateDeviceExtensionProperties EDE =
         (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
      uint32_t ne = 0;
      if (EDE)
         EDE(pd, NULL, &ne, NULL);
      VkExtensionProperties eps[128];
      if (ne > 128) ne = 128;
      if (EDE && ne)
         EDE(pd, NULL, &ne, eps);
      int has_clip = 0, has_cb = 0;
      for (uint32_t i = 0; i < ne; i++) {
         has_clip |= !strcmp(eps[i].extensionName, "VK_EXT_depth_clip_enable");
         has_cb |= !strcmp(eps[i].extensionName, "VK_EXT_custom_border_color");
         g_has_mc |= !strcmp(eps[i].extensionName, "VK_KHR_sampler_mirror_clamp_to_edge");
      }
      /* advertising the extension is not enough: both EXT features have to be answered true, and
       * customBorderColorWithoutFormat decides whether the custom colour may be used with a format-less
       * request (we pass a real format, so it is not needed, but asking for it ungated would be invalid) */
      PFN_vkGetPhysicalDeviceFeatures2KHR gpf2 =
         (PFN_vkGetPhysicalDeviceFeatures2KHR)gipa(inst, "vkGetPhysicalDeviceFeatures2KHR");
      if (gpf2 && (has_clip || has_cb)) {
         VkPhysicalDeviceCustomBorderColorFeaturesEXT cbq = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
         VkPhysicalDeviceDepthClipEnableFeaturesEXT dcq = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
         cbq.pNext = &dcq;
         VkPhysicalDeviceFeatures2KHR f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
                                            .pNext = &cbq};
         gpf2(pd, &f2);
         has_cb &= cbq.customBorderColors;
         has_clip &= dcq.depthClipEnable;
         cbf.customBorderColorWithoutFormat =
            cbq.customBorderColorWithoutFormat ? VK_TRUE : VK_FALSE;
      } else {
         has_cb = has_clip = 0;
      }
      if (has_clip) {
         en_ext[nx++] = "VK_EXT_depth_clip_enable";
         dcf.pNext = chain;
         chain = &dcf;
      }
      if (has_cb) {
         en_ext[nx++] = "VK_EXT_custom_border_color";
         cbf.pNext = chain;
         chain = &cbf;
      }
      if (g_has_mc)
         en_ext[nx++] = "VK_KHR_sampler_mirror_clamp_to_edge";
      g_has_clip = has_clip, g_has_cb = has_cb;
      printf("{\"ext\":{\"depth_clip_enable\":%d,\"custom_border_color\":%d,\"mirror_clamp_to_edge\":%d}}\n",
             has_clip, has_cb, g_has_mc);
   }
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfi,
                                  .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = chain,
                             .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                             .enabledExtensionCount = nx,
                             .ppEnabledExtensionNames = en_ext};
   VkPhysicalDeviceFeatures want = {.depthClamp = g_feat.depthClamp, .samplerAnisotropy = g_feat.samplerAnisotropy};
   dci.pEnabledFeatures = &want;
   if (CreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS)
      die("vkCreateDevice");
   PFN_vkGetDeviceProcAddr gdpa = GetDeviceProcAddr;
#define D(n) n = (PFN_vk##n)gdpa(dev, "vk" #n); if (!n) die("missing device function vk" #n);
   DEV_FNS(D)
#undef D
   GetDeviceQueue(dev, qfi, 0, &queue);

   build_cases();
   int nsel = 0;
   for (int i = 0; i < g_ncase; i++) {
      struct scase *c = &cases[i];
      if (!case_selected(c))
         continue;
      /* a row this device cannot express is a SKIP with the reason, never a PASS */
      if (c->g == G_SAMP && c->s.aniso > 0.0f) {
         if (!g_feat.samplerAnisotropy)
            c->skip = "samplerAnisotropy unsupported";
         else if (c->s.aniso > g_props.limits.maxSamplerAnisotropy)
            c->skip = "maxSamplerAnisotropy below the case";
      }
      if (c->g == G_SAMP && !g_has_mc &&
          (c->s.mu == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
           c->s.mv == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE))
         c->skip = "VK_KHR_sampler_mirror_clamp_to_edge not enabled";
      if (c->g == G_SAMP && c->border_color == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT && !g_has_cb)
         c->skip = "VK_EXT_custom_border_color not enabled";
      nsel++;
   }

   /* ---- attachments, textures, pipeline layout */
   VkDeviceMemory colmem, depmem, texmem[3];
   fill_tex(&T_PAT);
   fill_tex(&T_BLK);
   fill_tex(&T_FLAT);
   struct texdata *tds[3] = {&T_PAT, &T_BLK, &T_FLAT};
   VkImage colimg = make_image(VK_FORMAT_R8G8B8A8_UNORM, FW, FH, 1,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &colmem);
   VkImage depimg = make_image(VK_FORMAT_D24_UNORM_S8_UINT, FW, FH, 1,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &depmem);
   VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   vci.image = colimg;
   VkImageView colview;
   if (CreateImageView(dev, &vci, NULL, &colview) != VK_SUCCESS)
      die("colour view");
   VkImageViewCreateInfo vdi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_D24_UNORM_S8_UINT, .image = depimg,
                                .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1}};
   VkImageView depview;
   if (CreateImageView(dev, &vdi, NULL, &depview) != VK_SUCCESS)
      die("depth/stencil view");

   VkImage teximg[3];
   VkImageView texview[3];
   for (int i = 0; i < 3; i++) {
      teximg[i] = make_image(VK_FORMAT_R8G8B8A8_UNORM, TEXN, TEXN, NMIP,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &texmem[i]);
      VkImageViewCreateInfo v = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                 .image = teximg[i],
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, NMIP, 0, 1}};
      if (CreateImageView(dev, &v, NULL, &texview[i]) != VK_SUCCESS)
         die("texture view");
   }

   VkAttachmentDescription att[2] = {
      {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
       .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
      {.format = VK_FORMAT_D24_UNORM_S8_UINT, .samples = VK_SAMPLE_COUNT_1_BIT,
       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
       .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
   };
   VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkAttachmentReference dref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                              .pColorAttachments = &cref, .pDepthStencilAttachment = &dref};
   VkSubpassDependency dep[2] = {
      {VK_SUBPASS_EXTERNAL, 0,
       VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0},
      {0, VK_SUBPASS_EXTERNAL,
       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
       VK_PIPELINE_STAGE_TRANSFER_BIT,
       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0},
   };
   VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2,
                                 .pAttachments = att, .subpassCount = 1, .pSubpasses = &sp,
                                 .dependencyCount = 2, .pDependencies = dep};
   if (CreateRenderPass(dev, &rpi, NULL, &rp) != VK_SUCCESS)
      die("vkCreateRenderPass");
   VkImageView atts[2] = {colview, depview};
   VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
                                  .attachmentCount = 2, .pAttachments = atts, .width = FW, .height = FH, .layers = 1};
   if (CreateFramebuffer(dev, &fbi, NULL, &fb) != VK_SUCCESS)
      die("vkCreateFramebuffer");

   VkDescriptorSetLayoutBinding lb = {.binding = 0, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 1, .pBindings = &lb};
   if (CreateDescriptorSetLayout(dev, &dli, NULL, &dsl) != VK_SUCCESS)
      die("descriptor set layout");
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NCASE + 8};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = NCASE + 8, .poolSizeCount = 1, .pPoolSizes = &ps};
   if (CreateDescriptorPool(dev, &dpi, NULL, &dpool) != VK_SUCCESS)
      die("descriptor pool");
   VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   if (CreatePipelineLayout(dev, &pli, NULL, &pl) != VK_SUCCESS)
      die("pipeline layout");

   vs_mod = load_spv("st_quad.vert.spv");
   fs_tex = load_spv("st_tex.frag.spv");
   fs_impl = load_spv("st_impl.frag.spv");
   fs_solid = load_spv("st_solid.frag.spv");

   /* ---- samplers + descriptor sets (one per sampler case) */
   VkSamplerCreateInfo sd = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                             .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                             .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                             .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                             .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                             .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                             .maxLod = (float)MAXLVL};
   if (CreateSampler(dev, &sd, NULL, &smp_dummy) != VK_SUCCESS)
      die("dummy sampler");
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   if (AllocateDescriptorSets(dev, &dai, &dset_dummy) != VK_SUCCESS)
      die("dummy descriptor set");
   VkDescriptorImageInfo dii = {.sampler = smp_dummy, .imageView = texview[0],
                                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   VkWriteDescriptorSet wds = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset_dummy,
                               .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii};
   UpdateDescriptorSets(dev, 1, &wds, 0, NULL);

   for (int i = 0; i < g_ncase; i++) {
      struct scase *c = &cases[i];
      if (c->g != G_SAMP || !case_selected(c) || c->skip)
         continue;
      VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = c->s.magf, .minFilter = c->s.minf, .mipmapMode = c->s.mm,
                                .addressModeU = c->s.mu, .addressModeV = c->s.mv,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .anisotropyEnable = c->s.aniso > 0.0f ? VK_TRUE : VK_FALSE,
                                .maxAnisotropy = c->s.aniso > 0.0f ? c->s.aniso : 1.0f,
                                .minLod = 0.0f, .maxLod = (float)MAXLVL,
                                .borderColor = c->border_color};
      VkSamplerCustomBorderColorCreateInfoEXT cbci = {
         .sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
         .format = VK_FORMAT_R8G8B8A8_UNORM};
      for (int ch = 0; ch < 4; ch++)
         cbci.customBorderColor.float32[ch] = c->border_f[ch];
      if (c->border_f[0] != 0.0f || c->border_f[1] != 0.0f || c->border_f[2] != 0.0f)
         si.pNext = &cbci;
      VkSampler sp2;
      if (CreateSampler(dev, &si, NULL, &sp2) != VK_SUCCESS) {
         fprintf(stderr, "t_state_sweep: sampler for %s failed\n", c->id);
         return 1;
      }
      VkDescriptorSetAllocateInfo a = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl};
      if (AllocateDescriptorSets(dev, &a, &c->dset) != VK_SUCCESS)
         die("descriptor set");
      VkDescriptorImageInfo di = {.sampler = sp2, .imageView = texview[c->img],
                                  .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->dset,
                                .dstBinding = 0, .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &di};
      UpdateDescriptorSets(dev, 1, &w, 0, NULL);
   }

   /* ---- pipelines: three shared ones, then one per blend / depth / stencil case */
   struct pipekey k;
   memset(&k, 0, sizeof k);
   k.shader = SH_TEXLOD;
   k.dcmp = VK_COMPARE_OP_ALWAYS;
   k.b.bconst = -1;
   k.scmpmask = 0xff;
   pipe_tex = make_pipe(&k);
   k.shader = SH_IMPL;
   pipe_impl = make_pipe(&k);
   k.shader = SH_SOLID;
   pipe_solid = make_pipe(&k);
   /* base quad: depth test ALWAYS + write, the case's depth range comes from the pipeline's viewport range, which is
    * static state, so the base pipeline is created per depth range below */

   for (int i = 0; i < g_ncase; i++) {
      struct scase *c = &cases[i];
      if (!case_selected(c) || c->skip)
         continue;
      if (c->g == G_SAMP) {
         c->pipe = c->shader == SH_TEXLOD ? pipe_tex : pipe_impl;
         c->ndraw = 1;
         continue;
      }
      struct pipekey pk;
      memset(&pk, 0, sizeof pk);
      pk.b.bconst = -1;
      pk.scmpmask = 0xff;
      pk.shader = SH_SOLID;
      if (c->g == G_BLEND) {
         pk.b = c->b;
         pk.depth_test = 0;
         pk.depth_write = 0;
         pk.dcmp = VK_COMPARE_OP_ALWAYS;
         c->ndraw = 2;
      } else {
         pk.depth_test = c->d.depth_test;
         pk.depth_write = c->d.depth_write;
         pk.dcmp = c->d.dcmp;
         pk.clamp = c->d.clamp;
         pk.clip_off = c->d.clip_off;
         pk.stencil = c->d.stencil;
         pk.cw = c->d.cw;
         pk.dmin = c->d.dmin;
         pk.dmax = c->d.dmax;
         for (int f = 0; f < 2; f++) {
            pk.scmp[f] = c->d.scmp[f];
            pk.sfail[f] = c->d.sfail[f];
            pk.sdfail[f] = c->d.sdfail[f];
            pk.spass[f] = c->d.spass[f];
         }
         pk.scmpmask = c->d.scmpmask;
         pk.swmask = c->d.swmask;
         pk.sref = c->d.sref;
      }
      if (getenv("MALI_SS_DBG") && c->g == G_DS)
         fprintf(stderr, "dbg %s stencil=%d cw=%d cmp=%d/%d spass=%d/%d sw=%02x ref=%02x cmpw=%02x dt=%d dw=%d dcmp=%d\n",
                 c->id, pk.stencil, pk.cw, pk.scmp[0], pk.scmp[1], pk.spass[0], pk.spass[1], pk.swmask, pk.sref,
                 pk.scmpmask, pk.depth_test, pk.depth_write, pk.dcmp);
      c->pipe = make_pipe(&pk);
      if (!c->pipe)
         g_fail = 1;
   }
   /* base pipeline for the depth-group cases: depth ALWAYS + write, no stencil, no blend */
   struct pipekey bk;
   memset(&bk, 0, sizeof bk);
   bk.shader = SH_SOLID;
   bk.depth_test = 1;
   bk.depth_write = 1;
   bk.dcmp = VK_COMPARE_OP_ALWAYS;
   bk.b.bconst = -1;
   bk.scmpmask = 0xff;
   pipe_base = make_pipe(&bk);

   /* every shared pipeline the selected rows bind: a NULL one would otherwise reach CmdBindPipeline */
   if (!pipe_tex || !pipe_impl || !pipe_solid || !pipe_base) {
      printf("FAIL shared pipeline: tex=%p impl=%p solid=%p base=%p\n", (void *)pipe_tex, (void *)pipe_impl,
             (void *)pipe_solid, (void *)pipe_base);
      return 1;
   }

   /* The fake backend executes nothing, but it must still accept every sampler, descriptor set and pipeline the
    * hardware path creates: the skip sits after that state exists, so a creation failure cannot hide behind it. */
   if (g_fake) {
      if (g_fail) {
         printf("FAIL fake backend: a case pipeline failed to create\n");
         return 1;
      }
      for (int i = 0; i < g_ncase; i++)
         if (case_selected(&cases[i]))
            printf("case=%s SKIP fake-backend-executes-nothing\n", cases[i].id);
      printf("{\"test\":\"state_sweep\",\"device\":\"%s\",\"cases\":%d,\"skipped\":\"fake backend\",\"ok\":true}\n",
             g_props.deviceName, nsel);
      return 0;
   }

   /* ---- buffers: texture staging, readback */
   VkDeviceMemory cbmem, dbmem, sbmem, tbmem[3];
   void *cmap, *dmap, *smap;
   VkBuffer cbuf = make_buffer((VkDeviceSize)FW * FH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &cbmem, &cmap);
   VkBuffer dbuf = make_buffer((VkDeviceSize)FW * FH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dbmem, &dmap);
   VkBuffer sbuf = make_buffer((VkDeviceSize)FW * FH, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &sbmem, &smap);
   VkBuffer tbuf[3];
   for (int i = 0; i < 3; i++) {
      void *m;
      tbuf[i] = make_buffer((VkDeviceSize)TEXN * TEXN * 4 * NMIP, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &tbmem[i], &m);
      for (int n = 0; n < NMIP; n++) {
         int w = TEXN >> n;
         for (int y = 0; y < w; y++)
            memcpy((uint8_t *)m + ((size_t)(n * TEXN * TEXN + y * w) * 4), &tds[i]->mip[n][y * TEXN * 4], (size_t)w * 4);
      }
   }

   /* ---- record */
   VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi};
   VkCommandPool cpool;
   if (CreateCommandPool(dev, &cpci, NULL, &cpool) != VK_SUCCESS)
      die("command pool");
   VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
   VkCommandBuffer cb;
   if (AllocateCommandBuffers(dev, &cbai, &cb) != VK_SUCCESS)
      die("command buffer");
   VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   if (BeginCommandBuffer(cb, &cbbi) != VK_SUCCESS) {
      printf("FAIL vkBeginCommandBuffer\n");
      g_fail = 1;
      return 1;
   }

   /* upload the three textures */
   {
      for (int i = 0; i < 3; i++) {
         /* fresh barriers per texture: the second image must not inherit the first one's post-copy layouts/access */
         VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, NMIP, 0, 1}};
         b.image = teximg[i];
         CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                            1, &b);
         for (int n = 0; n < NMIP; n++) {
            int w = TEXN >> n;
            VkBufferImageCopy r = {.bufferOffset = (VkDeviceSize)(n * TEXN * TEXN * 4),
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)n, 0, 1},
                                   .imageExtent = {(uint32_t)w, (uint32_t)w, 1}};
            CmdCopyBufferToImage(cb, tbuf[i], teximg[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
         }
         b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
         b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
         b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
         b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
         CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                            NULL, 1, &b);
      }
   }

   VkClearValue clears[2];
   clears[0].color.float32[0] = 0.5f;
   clears[0].color.float32[1] = 0.5f;
   clears[0].color.float32[2] = 0.5f;
   clears[0].color.float32[3] = 1.0f;
   clears[1].depthStencil.depth = 1.0f;
   clears[1].depthStencil.stencil = 0x33;   /* non-zero: a read-back that returns 0 for a KEEP case is then visible */
   VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
                                .framebuffer = fb, .renderArea = {{0, 0}, {FW, FH}}, .clearValueCount = 2,
                                .pClearValues = clears};
   CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

   /* every case gets its own 8x8 tile */
   for (int i = 0; i < g_ncase; i++) {
      struct scase *c = &cases[i];
      if (!case_selected(c) || c->skip)
         continue;
      if (!c->pipe && c->g != G_SAMP)
         continue;
      int tx = (i % GRID) * TW, ty = (i / GRID) * TW;
      VkViewport vp = {(float)tx, (float)ty, (float)TW, (float)TW, c->d.dmin, c->d.dmax};
      VkRect2D sc = {{tx, ty}, {TW, TW}};
      CmdSetViewport(cb, 0, 1, &vp);
      CmdSetScissor(cb, 0, 1, &sc);
      CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, c->dset ? &c->dset : &dset_dummy, 0, NULL);
      if (c->g == G_BLEND) {
         /* 1: the destination, written by the shared opaque pipeline */
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_base);
         float pcd[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
         for (int ch = 0; ch < 4; ch++)
            pcd[4 + ch] = c->dst8[ch] / 255.0f;
         CmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, pcd);
         CmdDraw(cb, 6, 1, 0, 0);
         /* 2: the blend source */
         float pcs[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
         for (int ch = 0; ch < 4; ch++)
            pcs[4 + ch] = c->src8[ch] / 255.0f;
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipe);
         CmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, pcs);
         CmdDraw(cb, 6, 1, 0, 0);
         continue;
      }
      if (c->g == G_DS && c->ndraw == 2) {
         /* base quad with the shared depth-write-ALWAYS pipeline, at the same viewport depth range */
         float pcb[12] = {0.5f, 0.5f, 0.5f, 0.5f, 0, 0, 0, 0, 0, 0, 0, 0};
         for (int ch = 0; ch < 4; ch++)
            pcb[4 + ch] = c->base8[ch] / 255.0f;
         pcb[8] = c->z_base;
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_base);
         CmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, pcb);
         CmdDraw(cb, 6, 1, 0, 0);
      }
      if (c->g == G_DS) {
         float pcc[12] = {0.5f, 0.5f, 0.5f, 0.5f, 0, 0, 0, 0, 0, 0, 0, 0};
         for (int ch = 0; ch < 4; ch++)
            pcc[4 + ch] = c->test8[ch] / 255.0f;
         pcc[8] = c->z;
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipe);
         CmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, pcc);
         CmdDraw(cb, 6, 1, 0, 0);
         continue;
      }
      float p0[12] = {c->uvs[0], c->uvs[1], c->uvs[2], c->uvs[3], c->col0[0], c->col0[1], c->col0[2], c->col0[3],
                      0.5f, 0, 0, 0};
      CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipe);
      CmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, p0);
      CmdDraw(cb, 6, 1, 0, 0);
   }
   CmdEndRenderPass(cb);

   /* ---- read back colour, depth, stencil */
   VkBufferImageCopy rc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {FW, FH, 1}};
   CmdCopyImageToBuffer(cb, colimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cbuf, 1, &rc);
   VkBufferImageCopy rd = {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                           .imageExtent = {FW, FH, 1}};
   CmdCopyImageToBuffer(cb, depimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dbuf, 1, &rd);
   VkBufferImageCopy rs = {.imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1},
                           .imageExtent = {FW, FH, 1}};
   CmdCopyImageToBuffer(cb, depimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sbuf, 1, &rs);
   if (EndCommandBuffer(cb) != VK_SUCCESS) {
      printf("FAIL vkEndCommandBuffer: nothing was submitted\n");
      g_fail = 1;
      return 1;
   }
   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   if (CreateFence(dev, &fci, NULL, &fence) != VK_SUCCESS)
      die("fence");
   VkSubmitInfo si2 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   if (QueueSubmit(queue, 1, &si2, fence) != VK_SUCCESS)
      die("vkQueueSubmit");
   if (WaitForFences(dev, 1, &fence, VK_TRUE, 120ull * 1000000000ull) != VK_SUCCESS)
      die("fence timeout");
   DeviceWaitIdle(dev);

   if (g_ppm) {
      FILE *f = fopen(g_ppm, "wb");
      if (f) {
         fprintf(f, "P6\n%d %d\n255\n", FW, FH);
         const uint8_t *p = cmap;
         for (int i = 0; i < FW * FH; i++)
            fwrite(p + i * 4, 1, 3, f);
         fclose(f);
      }
   }

   /* ---- compare */
   const uint8_t *col = cmap;
   const uint32_t *depb = dmap;
   const uint8_t *stc = smap;
   int npass = 0, nfail = 0, nskip = 0;
   for (int i = 0; i < g_ncase; i++) {
      struct scase *c = &cases[i];
      if (!case_selected(c))
         continue;
      if (c->skip) {
         printf("case=%s SKIP %s\n", c->id, c->skip);
         nskip++;
         continue;
      }
      if (!c->pipe && c->g != G_SAMP) {
         printf("case=%s SKIP pipeline-create-failed\n", c->id);
         nskip++;
         continue;
      }
      int tx = (i % GRID) * TW, ty = (i / GRID) * TW;
      uint8_t exp8[4] = {0, 0, 0, 0}, got8[4] = {0, 0, 0, 0};
      uint32_t expd = 0, gotd = 0, expd_hi = 0, exps = 0, gots = 0;
      int has_depth = 0, has_stencil = 0, bad = 0, badx = 0, bady = 0;
      g_alt_formula = 0, g_blend_1lsb = 0;

      if (c->g == G_SAMP) {
         /* a filtered tap sum can land on the last bit of the hardware's weight precision, so allow 1 LSB there;
          * NEAREST cases and every other group are compared exactly */
         int tol = c->s.linear ? 1 : 0;
         int alt = (c->s.mu == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
                    c->s.mv == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE);
         for (int formula = 0; formula < (alt ? 2 : 1); formula++) {
            g_mclamp_clamp = formula;
            bad = 0, badx = bady = 0;
            for (int py = 0; py < TW && !bad; py++)
               for (int px = 0; px < TW && !bad; px++) {
                  int ry = py;   /* measured on Intel LNL: framebuffer row py samples q.y = (py+0.5)/8 (no flip) */
                  int tu = c->tu0 + px * c->tustep, tv = c->tv0 + ry * c->tvstep, e[4], e2[4];
                  sample_level(tds[c->img], c->s.lvl0, tu, tv, c->s.linear, c->s.mu, c->s.mv, c->bc, e);
                  if (c->s.w1) {
                     sample_level(tds[c->img], c->s.lvl1, c->tu0_1 + px * c->tustep_1,
                                  c->tv0_1 + ry * c->tustep_1, c->s.linear, c->s.mu, c->s.mv, c->bc, e2);
                     for (int ch = 0; ch < 4; ch++)
                        e[ch] = (e[ch] * (8 - c->s.w1) + e2[ch] * c->s.w1 + 4) / 8;
                  }
                  const uint8_t *g = &col[((ty + py) * FW + tx + px) * 4];
                  for (int ch = 0; ch < 4; ch++) {
                     int d = (int)g[ch] - e[ch];
                     if (d < -tol || d > tol) {
                        bad = 1, badx = px, bady = py;
                        break;
                     }
                  }
                  if (bad || (px == 0 && py == 0))
                     for (int ch = 0; ch < 4; ch++) {
                        exp8[ch] = (uint8_t)e[ch];
                        got8[ch] = g[ch];
                     }
               }
            if (!bad) {
               if (formula)
                  g_alt_formula = 1;
               break;
            }
         }
         g_mclamp_clamp = 0;
      } else if (c->g == G_BLEND) {
         int e[4];
         ref_blend(c, e);
         for (int ch = 0; ch < 4; ch++)
            exp8[ch] = (uint8_t)e[ch];
         memcpy(got8, &col[(ty * FW + tx) * 4], 4);   /* report the tile's first pixel, also for a PASS */
         /* The Mali blender applies the 8-bit factors in 1/256 fixed point, so a blend result whose exact value lands
          * between two UNORM8 codes can be 1 LSB below the round-to-nearest value the spec's conversion asks for
          * (e.g. 134.5 -> 134, not 135). Allow exactly that 1 LSB; anything larger is a packing error and still fails. */
         int lsb = 0;
         for (int py = 0; py < TW && !bad; py++)
            for (int px = 0; px < TW && !bad; px++) {
               const uint8_t *q = &col[((ty + py) * FW + tx + px) * 4];
               for (int ch = 0; ch < 4; ch++) {
                  /* the 1-LSB allowance belongs to the channels the blend writes; a masked-off channel must hold
                   * the destination bit for bit (b_wmask_0 red is 102, a 103 has already destroyed it) */
                  int tolch = ((c->b.mask >> ch) & 1) ? 1 : 0;
                  int d = (int)q[ch] - exp8[ch];
                  if (d < -tolch || d > tolch) {
                     bad = 1, badx = px, bady = py;
                     memcpy(got8, q, 4);
                     break;
                  }
                  if (d) lsb = 1;
               }
            }
         if (!bad && lsb) g_blend_1lsb = 1;
      } else {
         has_depth = 1;
         has_stencil = c->d.stencil;
         const uint8_t *g = &col[(ty * FW + tx) * 4];
         memcpy(got8, g, 4);
         if (c->d.stencil) {
            gotd = depb[(ty * FW + tx)] & 0xffffffu;
            gots = stc[ty * FW + tx];
            if (getenv("MALI_SS_DBG")) {
               fprintf(stderr, "dbg %s tile stencil:", c->id);
               for (int q = 0; q < TW; q++)
                  fprintf(stderr, " %02x", stc[(ty + q) * FW + tx + q]);
               fprintf(stderr, "  row0:");
               for (int q = 0; q < TW; q++)
                  fprintf(stderr, " %02x", stc[ty * FW + tx + q]);
               fprintf(stderr, "\n");
            }
            int B = 0x33;   /* the render pass clears the stencil to 0x33 */
            exps = (uint32_t)B;
            VkStencilOp op = c->d.spass[c->d.cw];
            int cmp_ok = dcmp(c->d.scmp[c->d.cw], c->d.sref & c->d.scmpmask, B & c->d.scmpmask);
            if (cmp_ok) {
               uint32_t r = c->d.sref & 0xff;
               switch (op) {
               case VK_STENCIL_OP_KEEP: exps = B; break;
               case VK_STENCIL_OP_ZERO: exps = 0; break;
               case VK_STENCIL_OP_REPLACE: exps = (r & c->d.swmask) | (B & ~c->d.swmask); break;
               case VK_STENCIL_OP_INCREMENT_AND_CLAMP: exps = B + 1 > 0xff ? 0xff : (uint32_t)B + 1; break;
               case VK_STENCIL_OP_DECREMENT_AND_CLAMP: exps = B - 1 < 0 ? 0 : (uint32_t)B - 1; break;
               case VK_STENCIL_OP_INVERT: exps = (~(uint32_t)B) & 0xff; break;
               case VK_STENCIL_OP_INCREMENT_AND_WRAP: exps = (B + 1) & 0xff; break;
               case VK_STENCIL_OP_DECREMENT_AND_WRAP: exps = (B - 1) & 0xff; break;
               default: exps = B; break;
               }
            }
            if (!cmp_ok) {   /* failOp is the same op: REPLACE writes the reference, the colour is not written */
               VkStencilOp fop = c->d.sfail[c->d.cw];
               exps = fop == VK_STENCIL_OP_REPLACE ? ((c->d.sref & c->d.swmask) | (B & ~c->d.swmask))
                     : fop == VK_STENCIL_OP_KEEP    ? (uint32_t)B
                                                    : exps;
            }
            if (cmp_ok)
               memcpy(exp8, c->test8, 4);
            else {   /* the fragment is dropped: the tile keeps the render pass clear */
               exp8[0] = exp8[1] = exp8[2] = 128;
               exp8[3] = 255;
            }
            if (gots != exps)
               bad = 1;
            for (int ch = 0; ch < 4; ch++)
               if (got8[ch] != exp8[ch]) bad = 1;
         } else {
            uint32_t zb = qz(window_z(c, c->z_base));
            uint32_t zt = qz(window_z(c, c->z));
            int clipped = !c->d.clip_off && (c->z < 0.0f || c->z > 1.0f);
            int pass = clipped ? 0 : dcmp(c->d.dcmp, zt, zb);
            memcpy(exp8, pass ? c->test8 : c->base8, 4);
            expd = c->d.depth_write && pass ? zt : zb;
            expd_hi = expd;
            if (c->d.clip_off && !c->d.clamp && (c->z < 0.0f || c->z > 1.0f)) {
               /* depth clip off: the spec does not say whether z is clamped to [0,1] before the viewport depth
                * range, so accept both forms (Intel: no clamp; Mali: TBD) */
               float zl = c->d.dmin + (c->z < 0.0f ? 0.0f : 1.0f) * (c->d.dmax - c->d.dmin);
               float zu = c->d.dmin + (c->z < 0.0f ? c->z : c->z) * (c->d.dmax - c->d.dmin);   /* no clamp at all */
               uint32_t a = qz(zl), b = qz(zu);
               expd = a < b ? a : b;
               expd_hi = a < b ? b : a;
            }
            gotd = depb[(ty * FW + tx)] & 0xffffffu;
            for (int ch = 0; ch < 4; ch++)
               if (got8[ch] != exp8[ch]) bad = 1;
            if (gotd < expd || gotd > expd_hi)
               bad = 1;
         }
      }

      if (!bad) {
         printf("case=%s PASS expected=%d,%d,%d,%d got=%d,%d,%d,%d", c->id, exp8[0], exp8[1], exp8[2], exp8[3],
                got8[0], got8[1], got8[2], got8[3]);
         if (g_alt_formula)
            printf(" note=mclamp-clamp-formula");
         if (g_blend_1lsb)
            printf(" note=blend-1lsb");
         if (has_depth && !has_stencil)
            printf(" depth=%06x", gotd);
         if (has_stencil)
            printf(" stencil=%02x depth=%06x", gots, gotd);
         printf("\n");
         npass++;
      } else {
         if (badx || bady)
            printf("case=%s FAIL at=%d,%d expected=%d,%d,%d,%d got=%d,%d,%d,%d", c->id, badx, bady, exp8[0],
                   exp8[1], exp8[2], exp8[3], got8[0], got8[1], got8[2], got8[3]);
         else
            printf("case=%s FAIL expected=%d,%d,%d,%d got=%d,%d,%d,%d", c->id, exp8[0], exp8[1], exp8[2], exp8[3],
                   got8[0], got8[1], got8[2], got8[3]);
         if (has_depth && !has_stencil)
            printf(" depth=%06x want=%06x..%06x", gotd, expd, expd_hi);
         if (has_stencil)
            printf(" stencil=%02x want=%02x", gots, exps);
         printf("\n");
         nfail++;
         g_fail = 1;
      }
   }
   printf("{\"test\":\"state_sweep\",\"device\":\"%s\",\"cases\":%d,\"pass\":%d,\"fail\":%d,\"skip\":%d,"
          "\"ok\":%s}\n",
          g_props.deviceName, npass + nfail + nskip, npass, nfail, nskip,
          (nfail || g_fail) ? "false" : "true");
   return g_fail;
}
