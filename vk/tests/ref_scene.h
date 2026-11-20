/* SPDX-License-Identifier: MIT */
/* One deterministic scene, two implementations execute it: t_vk_ref on a phone (our ICD) and the same binary on
 * a desktop GPU (RADV) to make the reference image. See docs/desktop-gpu.md.
 *
 * Shared on purpose: geometry, texture, draw list, clear values, attachment formats, viewport and blend state live
 * here, so the only variable between the two runs is the Vulkan implementation that executes the scene. If the two
 * clients drifted apart the diff would prove nothing.
 *
 * The scene is built to make attachment-store bugs visible rather than pretty:
 *   - the colour image is VK_IMAGE_TILING_OPTIMAL (AFBC on Mali) and is read back with vkCmdCopyImageToBuffer, so a
 *     store/decode that loses or shifts a 64-row AFBC block shows up as exactly one or two 64-pixel bands;
 *   - content is non-trivial in every 64-row band (texture detail, depth-ordered quads, alpha blending);
 *   - a second pass with loadOp=LOAD draws again, so store->load of the same attachment is exercised;
 *   - depth is a real D32_SFLOAT attachment, so the draw order is decided by the depth test, not by the draw order.
 */
#ifndef REF_SCENE_H
#define REF_SCENE_H

#include <stdint.h>

#define REF_W 512
#define REF_H 512
#define REF_TEX 256
#define REF_BLOCK 64                       /* AFBC superblock row height: the diff and the JSON digest use it */
#define REF_NBLOCK (REF_H / REF_BLOCK)

struct ref_vertex {
   float pos[2];  /* NDC, y up */
   float uv[2];
   float col[4];
};

struct ref_draw {
   unsigned first, count;
   float z;       /* pushed to the vertex shader: clip-space z in [0,1] (Vulkan depth range) */
   float alpha;   /* multiplied into the vertex alpha */
   float tex;     /* 1 = sample the texture, 0 = vertex colour only */
};

/* 8 quads max, 2 triangles each */
#define REF_MAX_VERTS 48

/* Clear colour of the colour attachment, and the depth clear. Both are exactly representable in RGBA8 / D32F. */
#define REF_CLEAR_R 0.05f
#define REF_CLEAR_G 0.07f
#define REF_CLEAR_B 0.10f
#define REF_CLEAR_A 1.0f

static void
ref_quad(struct ref_vertex *v, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
         float r, float g, float b, float a)
{
   const struct ref_vertex q[6] = {
      {{x0, y0}, {u0, v0}, {r, g, b, a}}, {{x1, y0}, {u1, v0}, {r, g, b, a}}, {{x1, y1}, {u1, v1}, {r, g, b, a}},
      {{x0, y0}, {u0, v0}, {r, g, b, a}}, {{x1, y1}, {u1, v1}, {r, g, b, a}}, {{x0, y1}, {u0, v1}, {r, g, b, a}},
   };
   for (unsigned i = 0; i < 6; i++)
      v[i] = q[i];
}

/* Fill `v` (REF_MAX_VERTS) and return the number of vertices; `pass2_first` gets the vertex index of the LOAD-pass quad.
 * Draw order in pass 1 is background, then three overlapping quads whose visible parts are decided by depth. */
static unsigned
ref_build_vertices(struct ref_vertex *v, unsigned *pass2_first)
{
   unsigned n = 0;
   ref_quad(v + n, -1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
   n += 6;
   ref_quad(v + n, -0.85f, 0.10f, -0.15f, 0.85f, 0.10f, 0.10f, 0.90f, 0.90f, 0.95f, 0.15f, 0.10f, 1.0f);
   n += 6;
   ref_quad(v + n, -0.30f, -0.80f, 0.45f, -0.05f, 0.00f, 0.00f, 1.00f, 1.00f, 0.10f, 0.90f, 0.25f, 0.60f);
   n += 6;
   ref_quad(v + n, 0.25f, 0.20f, 0.95f, 0.90f, 0.25f, 0.75f, 0.75f, 0.25f, 0.15f, 0.35f, 1.00f, 1.0f);
   n += 6;
   *pass2_first = n;
   ref_quad(v + n, -0.60f, -0.60f, 0.60f, 0.60f, 0.0f, 0.0f, 1.0f, 1.0f, 1.00f, 1.00f, 0.00f, 0.50f);
   n += 6;
   return n;
}

/* Pass 1: z of each quad, in draw order (background last would also be correct: the depth test decides). */
static const struct ref_draw ref_draws_p1[] = {
   {0, 6, 0.95f, 1.0f, 1.0f},  /* background, textured */
   {6, 6, 0.50f, 1.0f, 1.0f},  /* red tile */
   {12, 6, 0.35f, 1.0f, 1.0f}, /* green tile, alpha 0.6 in the vertex colour: blended */
   {18, 6, 0.20f, 1.0f, 1.0f}, /* blue tile */
};
/* Pass 2 (loadOp LOAD): z 0.85 lands between the background and every tile, so its visible area is decided by depth. */
#define REF_P2_Z 0.85f

/* Texture content: 16x16 checker cells (bilinear-safe), a 4-row band modulation (visible at the 64-row scale), and
 * three saturated squares at known places so a shifted band is obvious to the eye in a diff image. */
static uint32_t
ref_texel(unsigned x, unsigned y)
{
   unsigned r = ((((x >> 4) ^ (y >> 4)) & 1u) ? 235u : 20u) + (((y >> 2) & 7u) * 3u);
   unsigned g = (x * 255u) / (REF_TEX - 1);
   unsigned b = (y * 255u) / (REF_TEX - 1);
   if (x >= 32 && x < 96 && y >= 32 && y < 96) { r = 255; g = 0; b = 0; }
   if (x >= 160 && x < 224 && y >= 128 && y < 192) { r = 0; g = 255; b = 0; }
   if (x >= 96 && x < 160 && y >= 192 && y < 256) { r = 0; g = 0; b = 255; }
   return (r & 255u) | ((g & 255u) << 8) | ((b & 255u) << 16) | (255u << 24);
}

static void
ref_texture(uint8_t *dst)
{
   for (unsigned y = 0; y < REF_TEX; y++)
      for (unsigned x = 0; x < REF_TEX; x++) {
         uint32_t t = ref_texel(x, y);
         uint8_t *p = dst + (y * REF_TEX + x) * 4;
         p[0] = (uint8_t)(t & 255u);
         p[1] = (uint8_t)((t >> 8) & 255u);
         p[2] = (uint8_t)((t >> 16) & 255u);
         p[3] = (uint8_t)((t >> 24) & 255u);
      }
}

/* FNV-1a 64 of one 64-row band, RGB only: the JSON summary a phone run is compared on before any image is copied. */
/* FNV-1a 64 over the whole frame in row order, RGB only. Unlike an XOR of the per-block digests this changes when two
 * bands swap places, which is the shifted-band failure the block comparison exists to catch. */
static uint64_t
ref_frame_digest(const uint8_t *px, unsigned pitch)
{
   uint64_t h = 14695981039346656037ull;
   for (unsigned y = 0; y < REF_H; y++)
      for (unsigned x = 0; x < REF_W; x++)
         for (unsigned c = 0; c < 3; c++) {
            h ^= px[y * pitch + x * 4 + c];
            h *= 1099511628211ull;
         }
   return h;
}

static uint64_t
ref_block_digest(const uint8_t *px, unsigned pitch, unsigned block)
{
   uint64_t h = 14695981039346656037ull;
   for (unsigned y = block * REF_BLOCK; y < (block + 1) * REF_BLOCK; y++)
      for (unsigned x = 0; x < REF_W; x++)
         for (unsigned c = 0; c < 3; c++) {
            h ^= px[y * pitch + x * 4 + c];
            h *= 1099511628211ull;
         }
   return h;
}

#endif
