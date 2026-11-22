/* SPDX-License-Identifier: MIT */
/* Host check of the fixed-function blend equation packing (vk/src/mali_blend.c). No GPU, no Vulkan: it packs every
 * (op, src factor, dst factor) the driver can meet and *evaluates* the packed hardware equation against the Vulkan
 * formula on sample colours, so a packable-but-wrong equation (the SRC_COLOR/DST_COLOR class) fails on the host.
 *
 * Hardware equation (genxml BlendEquation, common_pack.h:86-135): result = A + B * C, A in {ZERO,SRC,DEST},
 * B in {SRC_MINUS_DEST,SRC_PLUS_DEST,SRC,DEST}, C in {ZERO,SRC,DEST,SRC_X_2,SRC_ALPHA,DEST_ALPHA,CONSTANT},
 * B negatable, C invertible. Values are compared in 1/255 units with a 1 LSB allowance (the blender works in 1/256),
 * so only a *semantic* error can fail.
 *
 * Exit 0 = every packed pair matches the formula, 1 = at least one mismatch, 2 = usage.
 *   t_blend_eq [-v]
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mali_blend.h"

/* sample colours, the same ones the X4 state sweep uses: src = 153,102,204,128  dst = 102,153,51,64  const = 51 */
static const int SRCP[4] = {153, 102, 204, 128};
static const int DSTP[4] = {102, 153, 51, 64};
#define BCONST 51

static const char *
fname(unsigned f)
{
   static const char *n[] = {"zero", "one", "srcc", "isrcc", "dstc", "idstc", "srca", "isrca",
                             "dsta", "idsta", "cc", "icc", "ca", "ica"};
   return f < 14 ? n[f] : "?";
}

static const char *
oname(unsigned o)
{
   static const char *n[] = {"add", "sub", "rsub"};
   return o < 3 ? n[o] : "?";
}

/* A channel of a factor, in 1/255 units (VK_BLEND_FACTOR_* order). */
static int
fac(unsigned f, const int s[4], const int d[4], int ch)
{
   switch (f) {
   case 0: return 0;
   case 1: return 255;
   case 2: return s[ch];
   case 3: return 255 - s[ch];
   case 4: return d[ch];
   case 5: return 255 - d[ch];
   case 6: return s[3];
   case 7: return 255 - s[3];
   case 8: return d[3];
   case 9: return 255 - d[3];
   case 10: return BCONST;
   case 11: return 255 - BCONST;
   case 12: return BCONST;
   case 13: return 255 - BCONST;
   default: return 0;
   }
}

/* The Vulkan formula: src*fs op dst*fd, in 1/255 units, rounded to nearest (255 is odd, so no .5 ties). */
static int
spec(unsigned op, unsigned sf, unsigned df, const int s[4], const int d[4], int ch)
{
   int fs = fac(sf, s, d, ch), fd = fac(df, s, d, ch), num;
   if (op == 0)
      num = s[ch] * fs + d[ch] * fd;
   else if (op == 1)
      num = s[ch] * fs - d[ch] * fd;
   else
      num = d[ch] * fd - s[ch] * fs;
   /* round to nearest of num/255, then clamp */
   int v = num >= 0 ? (num + 127) / 255 : -((-num + 127) / 255);
   return v < 0 ? 0 : v > 255 ? 255 : v;
}

static int
op_a(unsigned a, const int s[4], const int d[4], int ch)
{
   switch (a) {
   case 1: return 0;          /* A_ZERO */
   case 2: return s[ch];      /* A_SRC */
   case 3: return d[ch];      /* A_DEST */
   default: return -1000;
   }
}

static int
op_b(unsigned b, const int s[4], const int d[4], int ch)
{
   switch (b) {
   case 0: return s[ch] - d[ch];   /* B_SRC_MINUS_DEST */
   case 1: return s[ch] + d[ch];   /* B_SRC_PLUS_DEST */
   case 2: return s[ch];           /* B_SRC */
   case 3: return d[ch];           /* B_DEST */
   default: return -1000;
   }
}

static int
op_c(unsigned c, const int s[4], const int d[4], int ch)
{
   switch (c) {
   case 1: return 0;                                    /* C_ZERO */
   case 2: return s[ch];                                /* C_SRC */
   case 3: return d[ch];                                /* C_DEST */
   case 4: return s[ch] * 2;                            /* C_SRC_X_2: a factor up to 2.0, not a value */
   case 5: return s[3];                                 /* C_SRC_ALPHA */
   case 6: return d[3];                                 /* C_DEST_ALPHA */
   case 7: return BCONST;                               /* C_CONSTANT */
   default: return -1000;
   }
}

/* Evaluate one packed 12-bit half of the equation word. */
static int
eval12(unsigned e, const int s[4], const int d[4], int ch)
{
   int A = op_a((e >> 0) & 7, s, d, ch);
   int na = (e >> 3) & 1;   /* bit 3: negate A */
   int nb = (e >> 7) & 1;
   int B = op_b((e >> 4) & 7, s, d, ch);
   int ic = (e >> 11) & 1;
   int C = op_c((e >> 8) & 7, s, d, ch);
   if (A < -999 || B < -999 || C < -999)
      return -1000;
   if (ic)
      C = 255 - C;
   if (na)
      A = -A;
   if (nb)
      B = -B;
   int v = A + (B * C) / 255;   /* the blender's fixed point; the 1 LSB allowance below covers its 1/256 scale */
   return v < 0 ? 0 : v > 255 ? 255 : v;
}

int
main(int argc, char **argv)
{
   int verbose = 0;
   if (argc > 1 && !strcmp(argv[1], "-v"))
      verbose = 1;
   int fail = 0, packed = 0, declined = 0;
   for (unsigned op = 0; op < 3; op++)
      for (unsigned sf = 0; sf < 14; sf++)
         for (unsigned df = 0; df < 14; df++) {
            struct mali_blend_eq_in in = {.en = true, .src_c = sf, .dst_c = df, .src_a = sf, .dst_a = df,
                                          .op_c = op, .op_a = op};
            uint32_t eq = 0;
            unsigned cm = 0;
            if (mali_blend_eq(&in, &eq, &cm)) {
               declined++;   /* the caller compiles a blend shader for it */
               continue;
            }
            packed++;
            for (int ch = 0; ch < 4; ch++) {
               unsigned half = ch == 3 ? (eq >> 12) & 0xfffu : eq & 0xfffu;
               int got = eval12(half, SRCP, DSTP, ch);
               int exp = spec(op, sf, df, SRCP, DSTP, ch);
               int d = got - exp;
               if (d < 0)
                  d = -d;
               if (d > 1) {
                  printf("FAIL op=%s src=%s dst=%s channel=%d packed=%03x expected=%d got=%d\n", oname(op), fname(sf),
                         fname(df), ch, half, exp, got);
                  fail = 1;
               } else if (verbose) {
                  printf("ok   op=%s src=%s dst=%s channel=%d expected=%d got=%d\n", oname(op), fname(sf), fname(df),
                         ch, exp, got);
               }
            }
         }
   printf("{\"test\":\"blend_eq\",\"pairs\":%d,\"packed\":%d,\"declined\":%d,\"ok\":%s}\n", packed + declined, packed,
          declined, fail ? "false" : "true");
   return fail;
}
