/* SPDX-License-Identifier: MIT */
/* Software reference decoder for the ETC2 / EAC shadow formats (mali_bc.h table entries ETC2_* / EAC_*): one block -> 16 shadow texels, row-major.
 * A plain-C port of Mesa's src/mesa/main/texcompress_etc.c (etc2_rgb8_parse_block / fetch_texel, alpha8, r11, signed r11), which is not part of
 * libmesa_util (only the ETC1 decoder util_format_etc1_rgb8_* is: t_bc.c cross-checks this port against it for every ETC1-compatible block). */
#ifndef T_ETC2_REF_H
#define T_ETC2_REF_H
#include <stdint.h>
#include <string.h>

static const int e2_dist[8] = { 3, 6, 11, 16, 23, 32, 41, 64 };
static const int e2_mod[16][8] = {
   { -3, -6, -9, -15, 2, 5, 8, 14 },  { -3, -7, -10, -13, 2, 6, 9, 12 }, { -2, -5, -8, -13, 1, 4, 7, 12 }, { -2, -4, -6, -13, 1, 3, 5, 12 },
   { -3, -6, -8, -12, 2, 5, 7, 11 },  { -3, -7, -9, -11, 2, 6, 8, 10 },  { -4, -7, -8, -11, 3, 6, 7, 10 }, { -3, -5, -8, -11, 2, 4, 7, 10 },
   { -2, -6, -8, -10, 1, 5, 7, 9 },   { -2, -5, -8, -10, 1, 4, 7, 9 },   { -2, -4, -8, -10, 1, 3, 7, 9 },  { -2, -5, -7, -10, 1, 4, 6, 9 },
   { -3, -4, -7, -10, 2, 3, 6, 9 },   { -1, -2, -3, -10, 0, 1, 2, 9 },   { -4, -6, -8, -9, 3, 5, 7, 8 },   { -3, -5, -7, -9, 2, 4, 6, 8 },
};
static const int e1_mod[8][4] = { { 2, 8, -2, -8 }, { 5, 17, -5, -17 }, { 9, 29, -9, -29 }, { 13, 42, -13, -42 },
                                  { 18, 60, -18, -60 }, { 24, 80, -24, -80 }, { 33, 106, -33, -106 }, { 47, 183, -47, -183 } };
static const int e2_lk[8] = { 0, 1, 2, 3, -4, -3, -2, -1 };

static inline int e2_cl(int c) { return c < 0 ? 0 : c > 255 ? 255 : c; }

/* RGB8 / RGB8A1 block (8 bytes) -> rgba */
static void e2_rgb(const uint8_t *s, int pt, int x, int y, uint8_t *o)
{
   int R = (s[0] >> 3) + e2_lk[s[0] & 7], G = (s[1] >> 3) + e2_lk[s[1] & 7], B = (s[2] >> 3) + e2_lk[s[2] & 7];
   int opaque = 1, diffbit = 0;
   if (pt) opaque = !!(s[3] & 2); else diffbit = !!(s[3] & 2);
   uint32_t pix = ((uint32_t)s[4] << 24) | (s[5] << 16) | (s[6] << 8) | s[7];
   int bit = y + x * 4, idx = ((pix >> (15 + bit)) & 2) | ((pix >> bit) & 1);
   int base[3][3], paint[4][3], dist = 0, ind = 0, dif = 0, tm = 0, hm = 0, pl = 0;
   if (!diffbit && !pt) {
      ind = 1;
      for (int i = 0; i < 3; i++) { base[0][i] = (s[i] & 0xf0) | (s[i] >> 4); base[1][i] = ((s[i] & 0xf) << 4) | (s[i] & 0xf); }
   } else if (R < 0 || R > 31) {
      tm = 1;
      int x0[3] = { (((s[0] >> 3) & 3) << 2) | (s[0] & 3), s[1] >> 4, s[1] & 0xf }, x1[3] = { s[2] >> 4, s[2] & 0xf, s[3] >> 4 };
      for (int i = 0; i < 3; i++) { base[0][i] = (x0[i] << 4) | x0[i]; base[1][i] = (x1[i] << 4) | x1[i]; }
      dist = e2_dist[(((s[3] >> 2) & 3) << 1) | (s[3] & 1)];
      for (int i = 0; i < 3; i++) {
         paint[0][i] = e2_cl(base[0][i]); paint[1][i] = e2_cl(base[1][i] + dist);
         paint[2][i] = e2_cl(base[1][i]); paint[3][i] = e2_cl(base[1][i] - dist);
      }
   } else if (G < 0 || G > 31) {
      hm = 1;
      int x0[3] = { (s[0] >> 3) & 0xf, ((s[0] & 7) << 1) | ((s[1] >> 4) & 1), (s[1] & 8) | ((s[1] & 3) << 1) | ((s[2] >> 7) & 1) };
      int x1[3] = { (s[2] >> 3) & 0xf, ((s[2] & 7) << 1) | ((s[3] >> 7) & 1), (s[3] >> 3) & 0xf };
      for (int i = 0; i < 3; i++) { base[0][i] = (x0[i] << 4) | x0[i]; base[1][i] = (x1[i] << 4) | x1[i]; }
      int v0 = (base[0][0] << 16) + (base[0][1] << 8) + base[0][2], v1 = (base[1][0] << 16) + (base[1][1] << 8) + base[1][2];
      dist = e2_dist[(s[3] & 4) | ((s[3] & 1) << 1) | (v0 >= v1)];
      for (int i = 0; i < 3; i++) {
         paint[0][i] = e2_cl(base[0][i] + dist); paint[1][i] = e2_cl(base[0][i] - dist);
         paint[2][i] = e2_cl(base[1][i] + dist); paint[3][i] = e2_cl(base[1][i] - dist);
      }
   } else if (B < 0 || B > 31) {
      pl = 1; opaque = 1;
      unsigned t;
      t = (s[0] >> 1) & 0x3f; base[0][0] = (t << 2) | (t >> 4);
      t = ((s[0] & 1) << 6) | ((s[1] >> 1) & 0x3f); base[0][1] = (t << 1) | (t >> 6);
      t = ((s[1] & 1) << 5) | (s[2] & 0x18) | (((s[2] & 3) << 1) | ((s[3] >> 7) & 1)); base[0][2] = (t << 2) | (t >> 4);
      t = ((s[3] & 0x7c) >> 1) | (s[3] & 1); base[1][0] = (t << 2) | (t >> 4);
      t = (s[4] >> 1) & 0x7f; base[1][1] = (t << 1) | (t >> 6);
      t = ((s[4] & 1) << 5) | ((s[5] >> 3) & 0x1f); base[1][2] = (t << 2) | (t >> 4);
      t = ((s[5] & 7) << 3) | ((s[6] >> 5) & 7); base[2][0] = (t << 2) | (t >> 4);
      t = ((s[6] & 0x1f) << 2) | ((s[7] >> 6) & 3); base[2][1] = (t << 1) | (t >> 6);
      t = s[7] & 0x3f; base[2][2] = (t << 2) | (t >> 4);
   } else {
      dif = 1;
      for (int i = 0; i < 3; i++) {
         base[0][i] = (s[i] & 0xf8) | (s[i] >> 5);
         int n = (s[i] >> 3) + e2_lk[s[i] & 7];
         base[1][i] = (n << 3) | (n >> 2);
      }
   }
   if (ind || dif) {
      int t1 = (s[3] >> 5) & 7, t2 = (s[3] >> 2) & 7, flip = s[3] & 1;
      int blk = flip ? (y >= 2) : (x >= 2), t = blk ? t2 : t1, m;
      if (!pt || opaque) m = e1_mod[t][idx];
      else { static const int no[4] = { 0, 1, 0, -1 }; m = no[idx] * e1_mod[t][1]; }
      if (pt && !opaque && idx == 2) { o[0] = o[1] = o[2] = o[3] = 0; return; }
      for (int i = 0; i < 3; i++) o[i] = (uint8_t)e2_cl(base[blk][i] + m);
      o[3] = 255;
   } else if (tm || hm) {
      if (pt && !opaque && idx == 2) { o[0] = o[1] = o[2] = o[3] = 0; return; }
      for (int i = 0; i < 3; i++) o[i] = (uint8_t)paint[idx][i];
      o[3] = 255;
   } else if (pl) {
      for (int i = 0; i < 3; i++)
         o[i] = (uint8_t)e2_cl((x * (base[1][i] - base[0][i]) + y * (base[2][i] - base[0][i]) + 4 * base[0][i] + 2) >> 2);
      o[3] = 255;
   }
}

static inline int e2_aidx(const uint8_t *s, int x, int y)
{
   uint64_t v = 0;
   for (int i = 2; i < 8; i++) v = (v << 8) | s[i];
   return (int)((v >> (((3 - y) + (3 - x) * 4) * 3)) & 7);
}

static inline int e2_alpha(const uint8_t *s, int x, int y)
{
   return e2_cl(s[0] + e2_mod[s[1] & 15][e2_aidx(s, x, y)] * (s[1] >> 4));
}

/* EAC 11 bit -> 16 bit extended value, as the uint16 / int16 bit pattern */
static inline uint16_t e2_r11(const uint8_t *s, int x, int y, int sg)
{
   int mult = s[1] >> 4, m = e2_mod[s[1] & 15][e2_aidx(s, x, y)], c;
   if (!sg) {
      c = ((s[0] << 3) | 4) + (mult ? ((m * mult) << 3) : m);
      c = c < 0 ? 0 : c > 2047 ? 2047 : c;
      return (uint16_t)((c << 5) | (c >> 6));
   }
   int base = (int8_t)s[0];
   if (base == -128) base = -127;
   c = (base * 8) + (mult ? ((m * mult) * 8) : m);
   c = c < -1023 ? -1023 : c > 1023 ? 1023 : c;
   int a = c < 0 ? -c : c;
   a = (a << 5) | (a >> 5);
   return (uint16_t)(int16_t)(c < 0 ? -a : a);
}

/* mali_bc_block_fn: one block of format `f` (mode field of the mali_bc table) -> 16 texels */
static void etc2_ref_block(unsigned mode, const uint8_t *b, uint8_t *out)
{
   unsigned kind = mode & 7, sg = !!(mode & 8);
   for (int y = 0; y < 4; y++) {
      for (int x = 0; x < 4; x++) {
         uint8_t *o = out + (y * 4 + x) * (kind == 3 ? 2 : 4);
         if (kind <= 1) e2_rgb(b, kind == 1, x, y, o);
         else if (kind == 2) { e2_rgb(b + 8, 0, x, y, o); o[3] = (uint8_t)e2_alpha(b, x, y); }
         else if (kind == 3) { uint16_t v = e2_r11(b, x, y, sg); memcpy(o, &v, 2); }
         else { uint16_t v[2] = { e2_r11(b, x, y, sg), e2_r11(b + 8, x, y, sg) }; memcpy(o, v, 4); }
      }
   }
}
#endif
