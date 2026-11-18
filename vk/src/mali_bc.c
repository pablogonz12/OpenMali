/* SPDX-License-Identifier: MIT */
/* R4-M7: BC decode-to-shadow, Mesa-free core. See mali_bc.h. */
#include "mali_bc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "mali_bc_spv.h"

#define F(v, n, sh, m, bs, shd, bpt, s) { VK_FORMAT_##v, n, sh, m, bs, VK_FORMAT_##shd, bpt, s }
static const struct mali_bc_fmt g_fmt[] = {
   F(BC1_RGB_UNORM_BLOCK, "BC1_RGB_UNORM", MALI_BC_BC1, 3, 8, R8G8B8A8_UNORM, 4, false),
   F(BC1_RGB_SRGB_BLOCK, "BC1_RGB_SRGB", MALI_BC_BC1, 3, 8, R8G8B8A8_SRGB, 4, true),
   F(BC1_RGBA_UNORM_BLOCK, "BC1_RGBA_UNORM", MALI_BC_BC1, 0, 8, R8G8B8A8_UNORM, 4, false),
   F(BC1_RGBA_SRGB_BLOCK, "BC1_RGBA_SRGB", MALI_BC_BC1, 0, 8, R8G8B8A8_SRGB, 4, true),
   F(BC2_UNORM_BLOCK, "BC2_UNORM", MALI_BC_BC1, 1, 16, R8G8B8A8_UNORM, 4, false),
   F(BC2_SRGB_BLOCK, "BC2_SRGB", MALI_BC_BC1, 1, 16, R8G8B8A8_SRGB, 4, true),
   F(BC3_UNORM_BLOCK, "BC3_UNORM", MALI_BC_BC1, 2, 16, R8G8B8A8_UNORM, 4, false),
   F(BC3_SRGB_BLOCK, "BC3_SRGB", MALI_BC_BC1, 2, 16, R8G8B8A8_SRGB, 4, true),
   F(BC4_UNORM_BLOCK, "BC4_UNORM", MALI_BC_BC4, 0, 8, R8_UNORM, 1, false),
   F(BC4_SNORM_BLOCK, "BC4_SNORM", MALI_BC_BC4, 1, 8, R8_SNORM, 1, false),
   F(BC5_UNORM_BLOCK, "BC5_UNORM", MALI_BC_BC4, 2, 16, R8G8_UNORM, 2, false),
   F(BC5_SNORM_BLOCK, "BC5_SNORM", MALI_BC_BC4, 3, 16, R8G8_SNORM, 2, false),
   F(BC6H_UFLOAT_BLOCK, "BC6H_UFLOAT", MALI_BC_BC6H, 0, 16, R16G16B16A16_SFLOAT, 8, false),
   F(BC6H_SFLOAT_BLOCK, "BC6H_SFLOAT", MALI_BC_BC6H, 1, 16, R16G16B16A16_SFLOAT, 8, false),
   F(BC7_UNORM_BLOCK, "BC7_UNORM", MALI_BC_BC7, 0, 16, R8G8B8A8_UNORM, 4, false),
   F(BC7_SRGB_BLOCK, "BC7_SRGB", MALI_BC_BC7, 0, 16, R8G8B8A8_SRGB, 4, true),
   /* ETC2 / EAC: hardware samples these natively (mali_format.c), the shadow only serves as a blit / copy source (mali_meta_vk.c). mode: etc2.comp */
   F(ETC2_R8G8B8_UNORM_BLOCK, "ETC2_RGB8_UNORM", MALI_BC_ETC2, 0, 8, R8G8B8A8_UNORM, 4, false),
   F(ETC2_R8G8B8_SRGB_BLOCK, "ETC2_RGB8_SRGB", MALI_BC_ETC2, 0, 8, R8G8B8A8_SRGB, 4, true),
   F(ETC2_R8G8B8A1_UNORM_BLOCK, "ETC2_RGB8A1_UNORM", MALI_BC_ETC2, 1, 8, R8G8B8A8_UNORM, 4, false),
   F(ETC2_R8G8B8A1_SRGB_BLOCK, "ETC2_RGB8A1_SRGB", MALI_BC_ETC2, 1, 8, R8G8B8A8_SRGB, 4, true),
   F(ETC2_R8G8B8A8_UNORM_BLOCK, "ETC2_RGBA8_UNORM", MALI_BC_ETC2, 2, 16, R8G8B8A8_UNORM, 4, false),
   F(ETC2_R8G8B8A8_SRGB_BLOCK, "ETC2_RGBA8_SRGB", MALI_BC_ETC2, 2, 16, R8G8B8A8_SRGB, 4, true),
   F(EAC_R11_UNORM_BLOCK, "EAC_R11_UNORM", MALI_BC_ETC2, 3, 8, R16_UNORM, 2, false),
   F(EAC_R11_SNORM_BLOCK, "EAC_R11_SNORM", MALI_BC_ETC2, 3 | 8, 8, R16_SNORM, 2, false),
   F(EAC_R11G11_UNORM_BLOCK, "EAC_RG11_UNORM", MALI_BC_ETC2, 4, 16, R16G16_UNORM, 4, false),
   F(EAC_R11G11_SNORM_BLOCK, "EAC_RG11_SNORM", MALI_BC_ETC2, 4 | 8, 16, R16G16_SNORM, 4, false),
};
#undef F

const struct mali_bc_fmt *
mali_bc_format(VkFormat f)
{
   if (f < VK_FORMAT_BC1_RGB_UNORM_BLOCK || f > VK_FORMAT_EAC_R11G11_SNORM_BLOCK) return NULL;
   return &g_fmt[f - VK_FORMAT_BC1_RGB_UNORM_BLOCK];
}
bool mali_bc_is_bc(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }
bool mali_bc_is_etc2(VkFormat f) { return f >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && f <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK; }

VkFormatFeatureFlags
mali_bc_optimal_features(VkFormat f)
{
   if (!mali_bc_is_bc(f)) return 0;
   return VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
}

VkResult
mali_bc_image_supported(VkFormat f, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, uint32_t samples)
{
   const VkResult no = VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (!mali_bc_is_bc(f)) return no;
   if (type == VK_IMAGE_TYPE_1D || tiling != VK_IMAGE_TILING_OPTIMAL || samples != 1) return no;
   const VkImageUsageFlags ok = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   if (usage & ~ok) return no;
   const VkImageCreateFlags fok = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT |
                                  VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
   if (flags & ~fok) return no;
   if ((flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && type != VK_IMAGE_TYPE_2D) return no;
   if ((flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT) && type != VK_IMAGE_TYPE_3D) return no;
   return VK_SUCCESS;
}

static uint32_t mip(uint32_t v, uint32_t l) { v >>= l; return v ? v : 1; }
static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

int
mali_bc_layout_init(VkFormat f, VkImageType type, VkExtent3D ext, uint32_t levels, uint32_t layers, struct mali_bc_layout *o)
{
   memset(o, 0, sizeof *o);
   const struct mali_bc_fmt *fm = mali_bc_format(f);
   if (!fm || type == VK_IMAGE_TYPE_1D || levels == 0 || levels > MALI_BC_MAX_MIPS || layers == 0) return -EINVAL;
   if (!ext.width || !ext.height || !ext.depth || ext.width > 16384 || ext.height > 16384 || ext.depth > 2048) return -EINVAL;
   if (type == VK_IMAGE_TYPE_2D && ext.depth != 1) return -EINVAL;
   if (type == VK_IMAGE_TYPE_3D && layers != 1) return -EINVAL;
   o->fmt = fm; o->type = type; o->ext = ext; o->levels = levels; o->layers = layers; o->alignment = MALI_BC_ALIGN;
   uint64_t coff = 0, soff = 0;
   for (uint32_t l = 0; l < levels; l++) {
      uint32_t w = mip(ext.width, l), h = mip(ext.height, l), d = type == VK_IMAGE_TYPE_3D ? mip(ext.depth, l) : 1;
      uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
      struct mali_bc_level *c = &o->comp.lv[l], *s = &o->shadow.lv[l];
      c->w = s->w = w; c->h = s->h = h; c->d = s->d = d; c->bw = s->bw = bw; c->bh = s->bh = bh;
      c->off = coff; c->row = bw * fm->block_bytes; c->slice = (uint64_t)c->row * bh; c->size = c->slice * d;
      s->off = soff; s->row = (uint32_t)align_up((uint64_t)bw * 4 * fm->shadow_bpt, MALI_BC_ALIGN);
      s->slice = (uint64_t)s->row * bh * 4; s->size = s->slice * d;
      coff = align_up(coff + c->size, MALI_BC_ALIGN);
      soff = align_up(soff + s->size, MALI_BC_ALIGN);
   }
   o->comp.array_stride = coff; o->shadow.array_stride = soff;
   o->comp.size = coff * layers; o->shadow.size = soff * layers;
   o->comp.base = 0;
   o->shadow.base = align_up(o->comp.size, MALI_BC_ALIGN);
   o->size = o->shadow.base + o->shadow.size;
   if (o->comp.size > 0xffffffffull || o->shadow.size > 0xffffffffull) return -ERANGE;
   return 0;
}

void mali_bc_list_free(struct mali_bc_list *l) { free(l->j); memset(l, 0, sizeof *l); }

static struct mali_bc_job *
job_new(struct mali_bc_list *l)
{
   if (l->n == l->cap) {
      unsigned nc = l->cap ? l->cap * 2 : 8;
      struct mali_bc_job *p = realloc(l->j, nc * sizeof *p);
      if (!p) return NULL;
      l->j = p; l->cap = nc;
   }
   struct mali_bc_job *j = &l->j[l->n++];
   memset(j, 0, sizeof *j);
   return j;
}

int
mali_bc_plan_decode(struct mali_bc_list *l, const struct mali_bc_layout *lay, uint64_t image_va, uint32_t level, uint32_t layer0, uint32_t nlayers,
                    VkOffset3D off, VkExtent3D ext)
{
   if (!lay->fmt || level >= lay->levels || nlayers == 0 || layer0 + nlayers > lay->layers) return -EINVAL;
   const struct mali_bc_level *c = &lay->comp.lv[level], *s = &lay->shadow.lv[level];
   const bool is3d = lay->type == VK_IMAGE_TYPE_3D;
   if (off.x < 0 || off.y < 0 || off.z < 0 || !ext.width || !ext.height || !ext.depth) return -EINVAL;
   if (off.x & 3 || off.y & 3) return -EINVAL;
   if ((uint32_t)off.x + ext.width > c->w || (uint32_t)off.y + ext.height > c->h) return -EINVAL;
   if (((uint32_t)off.x + ext.width) & 3 && (uint32_t)off.x + ext.width != c->w) return -EINVAL;
   if (((uint32_t)off.y + ext.height) & 3 && (uint32_t)off.y + ext.height != c->h) return -EINVAL;
   uint32_t nz;
   uint64_t src_z, dst_z, src_base = c->off, dst_base = s->off;
   if (is3d) {
      if (nlayers != 1 || (uint32_t)off.z + ext.depth > c->d) return -EINVAL;
      nz = ext.depth; src_z = c->slice; dst_z = s->slice;
      src_base += (uint64_t)off.z * c->slice; dst_base += (uint64_t)off.z * s->slice;
   } else {
      if (off.z != 0 || ext.depth != 1) return -EINVAL;
      nz = nlayers; src_z = lay->comp.array_stride; dst_z = lay->shadow.array_stride;
      src_base += (uint64_t)layer0 * src_z; dst_base += (uint64_t)layer0 * dst_z;
   }
   const uint32_t bx0 = (uint32_t)off.x / 4, by0 = (uint32_t)off.y / 4;
   const uint32_t bw = ((uint32_t)off.x + ext.width + 3) / 4 - bx0, bh = ((uint32_t)off.y + ext.height + 3) / 4 - by0;
   const struct mali_bc_fmt *f = lay->fmt;
   src_base += (uint64_t)by0 * c->row + (uint64_t)bx0 * f->block_bytes;
   dst_base += (uint64_t)by0 * 4 * s->row + (uint64_t)bx0 * 4 * f->shadow_bpt;
   if (src_base > 0xffffffffull || dst_base > 0xffffffffull || src_z > 0xffffffffull || dst_z > 0xffffffffull) return -ERANGE;

   struct mali_bc_job *j = job_new(l);
   if (!j) return -ENOMEM;
   j->sh = f->sh; j->fmt = f;
   uint32_t *p = j->push;
   p[0] = (uint32_t)src_base; p[1] = c->row; p[2] = (uint32_t)src_z;
   p[3] = (uint32_t)dst_base; p[4] = s->row; p[5] = (uint32_t)dst_z;
   p[6] = bw; p[7] = bh; p[8] = nz; p[9] = f->mode;
   j->push_bytes = 48;
   j->bind[0] = (struct mali_bc_bind){ image_va + lay->comp.base, (uint32_t)lay->comp.size };
   j->bind[1] = (struct mali_bc_bind){ image_va + lay->shadow.base, (uint32_t)lay->shadow.size };
   j->nbind = 2;
   uint64_t total = (uint64_t)bw * bh * nz, g = (total + 63) / 64;
   j->groups[0] = g > 32768 ? 32768 : (uint32_t)g;
   j->groups[1] = (uint32_t)((g + 32767) / 32768);
   j->groups[2] = 1;
   return 0;
}

int
mali_bc_plan_decode_all(struct mali_bc_list *l, const struct mali_bc_layout *lay, uint64_t image_va)
{
   for (uint32_t lv = 0; lv < lay->levels; lv++) {
      const struct mali_bc_level *c = &lay->comp.lv[lv];
      int r = mali_bc_plan_decode(l, lay, image_va, lv, 0, lay->type == VK_IMAGE_TYPE_3D ? 1 : lay->layers, (VkOffset3D){ 0, 0, 0 },
                                  (VkExtent3D){ c->w, c->h, lay->type == VK_IMAGE_TYPE_3D ? c->d : 1 });
      if (r) return r;
   }
   return 0;
}

void
mali_bc_ref_run(const struct mali_bc_job *j, mali_bc_block_fn dec)
{
   const uint32_t *p = j->push;
   const uint8_t *src = (const uint8_t *)(uintptr_t)j->bind[0].va;
   uint8_t *dst = (uint8_t *)(uintptr_t)j->bind[1].va;
   const unsigned bpt = j->fmt->shadow_bpt, bs = j->fmt->block_bytes;
   uint8_t tmp[16 * 8];
   uint64_t total = (uint64_t)p[6] * p[7] * p[8];
   for (uint64_t i = 0; i < total; i++) {
      uint32_t bx = i % p[6]; uint64_t r = i / p[6]; uint32_t by = r % p[7], z = (uint32_t)(r / p[7]);
      const uint8_t *so = src + p[0] + (uint64_t)z * p[2] + (uint64_t)by * p[1] + (uint64_t)bx * bs;
      uint8_t *d0 = dst + p[3] + (uint64_t)z * p[5] + (uint64_t)by * 4 * p[4] + (uint64_t)bx * 4 * bpt;
      dec(j->fmt, so, tmp);
      for (unsigned row = 0; row < 4; row++) memcpy(d0 + (uint64_t)row * p[4], tmp + row * 4 * bpt, 4 * bpt);
   }
}

const uint32_t *
mali_bc_spirv(enum mali_bc_shader s, size_t *words)
{
   switch (s) {
   case MALI_BC_BC1: *words = sizeof bc1_spv / 4; return bc1_spv;
   case MALI_BC_BC4: *words = sizeof bc4_spv / 4; return bc4_spv;
   case MALI_BC_BC6H: *words = sizeof bc6h_spv / 4; return bc6h_spv;
   case MALI_BC_BC7: *words = sizeof bc7_spv / 4; return bc7_spv;
   case MALI_BC_ETC2: *words = sizeof etc2_spv / 4; return etc2_spv;
   default: *words = 0; return NULL;
   }
}
