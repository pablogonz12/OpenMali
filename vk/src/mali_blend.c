/* SPDX-License-Identifier: MIT */
/* Mali fixed-function blend equation packing: which VkBlendFactor/VkBlendOp pairs the blender can express and how they are
 * encoded, ported from panfrost lib/pan_blend.c to_mali_function (Mesa, cited only). Header-free on purpose: tests/t_blend_eq.c
 * links this file alone and checks the packed equation against the Vulkan formula on the host.
 *
 * The hardware equation (genxml BlendEquation, common_pack.h:86-135) is
 *     result = A + B * C
 * with A in {ZERO=1, SRC=2, DEST=3}, B in {SRC_MINUS_DEST=0, SRC_PLUS_DEST=1, SRC=2, DEST=3},
 * C in {ZERO=1, SRC=2, DEST=3, SRC_X_2=4, SRC_ALPHA=5, DEST_ALPHA=6, CONSTANT=7}, B negatable (bit 7) and C invertible (bit 11).
 * panfrost reaches the same form: "src*dest + dest*src = 2*src*dest = 0 + dest*(2*src)" (pan_blend.c:456-461). */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "mali_blend.h"


struct bfn { unsigned a, na, b, nb, c, ic; };
struct bfac { unsigned c; bool inv; };

static bool
bfac_of(unsigned vk, struct bfac *o)
{
   switch (vk) {
   case 0: *o = (struct bfac){1, false}; return true;   /* ZERO */
   case 1: *o = (struct bfac){1, true}; return true;    /* ONE */
   case 2: case 3: *o = (struct bfac){2, vk == 3}; return true;      /* SRC_COLOR */
   case 4: case 5: *o = (struct bfac){3, vk == 5}; return true;      /* DST_COLOR */
   case 6: case 7: *o = (struct bfac){5, vk == 7}; return true;      /* SRC_ALPHA */
   case 8: case 9: *o = (struct bfac){6, vk == 9}; return true;      /* DST_ALPHA */
   case 10: case 11: case 12: case 13: *o = (struct bfac){7, vk & 1}; return true;   /* CONSTANT_COLOR / CONSTANT_ALPHA */
   default: return false;
   }
}

static bool
bfn_of(unsigned op, unsigned sf, unsigned df, struct bfn *f)
{
   struct bfac s, d;
   if (op > 2 || !bfac_of(sf, &s) || !bfac_of(df, &d))
      return false;
   memset(f, 0, sizeof *f);
   bool same_base = sf >= 2 && sf <= 13 && df >= 2 && df <= 13 && ((sf - 2) >> 1) == ((df - 2) >> 1);
   if (sf == 0) {
      f->a = 1; f->b = 3; f->nb = op == 1; f->ic = d.inv; f->c = d.c;
   } else if (sf == 1) {
      f->a = 2; f->b = 3;
      if (op == 1) f->nb = 1; else if (op == 2) f->na = 1;
      f->ic = d.inv; f->c = d.c;
   } else if (df == 0) {
      f->a = 1; f->b = 2; f->nb = op == 2; f->ic = s.inv; f->c = s.c;
   } else if (df == 1) {
      f->a = 3; f->b = 2;
      if (op == 1) f->na = 1; else if (op == 2) f->nb = 1;
      f->ic = s.inv; f->c = s.c;
   } else if (sf == df) {
      f->a = 1; f->ic = s.inv; f->c = s.c;
      if (op == 0) f->b = 1;
      else { f->b = 0; f->nb = op == 2; }
   } else if (op == 0 && sf == 4 && df == 2) {
      /* DST_COLOR as the source factor and SRC_COLOR as the destination factor is the one order where both terms are the
       * same product: src*dst + dst*src = 2*src*dst = 0 + dst*(2*src). The other order (SRC_COLOR, DST_COLOR) is
       * src*src + dst*dst, which A + B*C cannot express (it has a single product term) and which this branch used to
       * pack as the same doubled product: on the X4 that gave 122,122,82,64 where the spec says 133,133,173,80
       * (tests/t_state_sweep.c case b_add_srcc_dstc). It falls through to the blend shader instead. */
      f->a = 1; f->b = 3; f->c = 4;
   } else if (same_base && s.inv != d.inv) {
      f->a = 3; f->ic = s.inv; f->c = s.c;
      if (op == 0) f->b = 0;
      else if (op == 2) { f->b = 1; f->nb = 1; }
      else { f->b = 1; f->na = 1; }
   } else {
      return false;
   }
   return true;
}

static unsigned
bconst_bits(unsigned vk)
{
   return vk >= 10 && vk <= 11 ? 7u : vk >= 12 && vk <= 13 ? 8u : 0u;
}

int
mali_blend_eq(const struct mali_blend_eq_in *fx, uint32_t *eq, unsigned *cm)
{
   struct bfn c, a;
   if (!fx->en) {
      *eq = 0x122u | (0x122u << 12);   /* replace: src */
      *cm = 0;
      return 0;
   }
   if (!bfn_of(fx->op_c, fx->src_c, fx->dst_c, &c) || !bfn_of(fx->op_a, fx->src_a, fx->dst_a, &a))
      return -1;
   uint32_t ec = c.a | (c.na << 3) | (c.b << 4) | (c.nb << 7) | (c.c << 8) | (c.ic << 11);
   uint32_t ea = a.a | (a.na << 3) | (a.b << 4) | (a.nb << 7) | (a.c << 8) | (a.ic << 11);
   *eq = ec | (ea << 12);
   *cm = bconst_bits(fx->src_c) | bconst_bits(fx->dst_c) | bconst_bits(fx->src_a) | bconst_bits(fx->dst_a);
   return 0;
}

