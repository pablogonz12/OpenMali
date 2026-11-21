// SPDX-License-Identifier: MIT
// Prints the ASTC decoder LUTs (Mesa's texcompress_astc_luts.cpp, from Granite) as GLSL const arrays for astc.comp.in (gen-bc-spv.sh).
#include <cstdio>
#include "texcompress_astc_luts.h"

template <class T> static void dump(const char *n, const T *v, size_t c)
{
   printf("const uint %s[%zu] = uint[%zu](", n, c, c);
   for (size_t i = 0; i < c; i++) printf("%s%uu", i ? (i % 24 ? "," : ",\n   ") : "\n   ", (unsigned)v[i]);
   printf(");\n");
}

int main()
{
   auto &l = Granite::get_astc_luts();
   dump("LUT_CE", &l.color_endpoint.lut[0][0][0], 9 * 128 * 4);
   dump("LUT_EU", l.color_endpoint.unquant_lut, l.color_endpoint.unquant_offset);
   dump("LUT_WQ", &l.weights.lut[0][0], sizeof l.weights.lut);
   dump("LUT_WU", l.weights.unquant_lut, l.weights.unquant_offset);
   dump("LUT_TQ", l.integer.trits_quints, 256 + 128);
   return 0;
}
